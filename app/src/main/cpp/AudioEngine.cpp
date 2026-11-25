#include "AudioEngine.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define LOG_TAG "MiniDAW"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using dsp::kSampleRate;

namespace {
const double kQuantTable[5] = {0.0, 0.25, 1.0, 4.0, 8.0};  // None, 1/4 beat, 1 Beat, 1 Bar, 2 Bars
}

// ============================================================ lifecycle
AudioEngine::AudioEngine() {
    for (int t = 0; t < kTracks; ++t) {
        trackParam_[t][TP_VOLUME].store(1.0f);
        trackParam_[t][TP_PAN].store(0.0f);
        trackParam_[t][TP_SEND_A].store(0.0f);
        trackParam_[t][TP_SEND_B].store(0.0f);
        trackParam_[t][TP_ACTIVE].store(1.0f);
        trackParam_[t][TP_SOLO].store(0.0f);
        trackParam_[t][TP_XF_ASSIGN].store(1.0f);
        trackParam_[t][TP_FILTER_TYPE].store(0.0f);
        trackParam_[t][TP_CUTOFF].store(1000.0f);
        trackParam_[t][TP_RESONANCE].store(0.707f);
        trackParam_[t][TP_DRIVE].store(0.0f);
        armed_[t].store(0);
        trackScene_[t].store(-1);
        trackStopQueued_[t].store(0);
        trackProgress_[t].store(0.0f);
        recBuf_[t] = nullptr;
        recBufSent_[t] = false;
        pendingScene_[t] = -1;
        pendingStop_[t] = false;
        recState_[t] = 0;
        recScene_[t] = 0;
        recFrames_[t] = 0;
        recBeats_[t] = 0.0;
        lastCutoff_[t] = -1.0f;
        lastRes_[t] = -1.0f;
        lastFiltType_[t] = -1;
        vol_[t].snap(1.0f);
        pan_[t].snap(0.0f);
        sendA_[t].snap(0.0f);
        sendB_[t].snap(0.0f);
        gate_[t].snap(1.0f);
    }
    for (int i = 0; i < GP_COUNT; ++i) globalParam_[i].store(0.0f);
    globalParam_[GP_MASTER_VOL].store(1.0f);
    globalParam_[GP_RET_A_VOL].store(1.0f);
    globalParam_[GP_RET_B_VOL].store(1.0f);
    globalParam_[GP_CROSSFADER].store(0.5f);
    globalParam_[GP_CLICK_VOL].store(0.5f);
    globalParam_[GP_BPM].store(120.0f);
    globalParam_[GP_QUANT].store(3.0f);
    retA_.snap(1.0f);
    retB_.snap(1.0f);
    master_.snap(1.0f);
    xfader_.snap(0.5f);
    clickVol_.snap(0.5f);

    for (int i = 0; i < kSlots; ++i) {
        clipWarp_[i].store(1);
        clipBars_[i].store(1);
        clipTranspose_[i].store(0);
        slotState_[i].store(SS_STOPPED);
        slotHas_[i].store(0);
        clips_[i] = nullptr;
        slotFrames_[i] = 0;
    }
    for (int i = 0; i < kMeterValues; ++i) meters_[i].store(0.0f);
    for (int c = 0; c < kMeterChannels; ++c) meterPeak_[c][0] = meterPeak_[c][1] = 0.0f;
    std::memset(inBuf_, 0, sizeof(inBuf_));

    reverb_.init();
    delay_.init();
    dsp::GrainStretcher::window();  // build the shared Hann table on the main thread
    buildDemoSet();
}

AudioEngine::~AudioEngine() {
    stop();
    // Everything below runs with the audio thread stopped.
    Cmd c;
    while (cmdRing_.pop(c)) {
        if ((c.type == CMD_SET_CLIP || c.type == CMD_SET_REC_BUF) && c.p)
            delete static_cast<ClipData*>(c.p);
    }
    while (eventRing_.pop(c)) {
        if ((c.type == EV_RETIRE) && c.p) delete static_cast<ClipData*>(c.p);
    }
    for (auto& g : graveyard_) delete g.first;
    graveyard_.clear();
    for (int i = 0; i < kSlots; ++i) delete clips_[i];
    for (int t = 0; t < kTracks; ++t) delete recBuf_[t];
}

bool AudioEngine::start() {
    if (running_.load()) return true;
    if (!openStreams()) return false;
    running_.store(true);
    return true;
}

void AudioEngine::stop() {
    running_.store(false);
    closeStreams();
}

bool AudioEngine::openStreams() {
    // Input first (optional): a failure only disables recording.
    inStream_.reset();
    inChannels_ = 0;
    {
        oboe::AudioStreamBuilder ib;
        ib.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::Float)
            ->setSampleRate(kSampleRate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setInputPreset(oboe::InputPreset::Unprocessed);
        oboe::Result r = ib.openStream(inStream_);
        if (r == oboe::Result::OK && inStream_) {
            inChannels_ = std::min(inStream_->getChannelCount(), kMaxInChannels);
            inStream_->requestStart();
        } else {
            LOGE("Input stream unavailable: %s", oboe::convertToText(r));
            inStream_.reset();
        }
    }

    oboe::AudioStreamBuilder ob;
    ob.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setSampleRate(kSampleRate)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setUsage(oboe::Usage::Media)
        ->setCallback(this);
    oboe::Result r = ob.openStream(outStream_);
    if (r != oboe::Result::OK || !outStream_) {
        LOGE("Output stream failed: %s", oboe::convertToText(r));
        if (inStream_) { inStream_->close(); inStream_.reset(); }
        return false;
    }
    outStream_->setBufferSizeInFrames(outStream_->getFramesPerBurst() * 2);
    r = outStream_->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("requestStart failed: %s", oboe::convertToText(r));
        return false;
    }
    LOGI("Audio started: %d Hz, burst %d, input channels %d", outStream_->getSampleRate(),
         outStream_->getFramesPerBurst(), inChannels_);
    return true;
}

void AudioEngine::closeStreams() {
    if (outStream_) { outStream_->stop(); outStream_->close(); outStream_.reset(); }
    if (inStream_) { inStream_->stop(); inStream_->close(); inStream_.reset(); }
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    LOGE("Stream error: %s", oboe::convertToText(error));
    if (error == oboe::Result::ErrorDisconnected) restartAsync();
}

void AudioEngine::restartAsync() {
    if (!running_.load() || restarting_.exchange(true)) return;
    std::thread([this] {
        closeStreams();
        if (running_.load()) openStreams();
        restarting_.store(false);
    }).detach();
}

// ============================================================ UI-side API
void AudioEngine::postCmd(const Cmd& c) {
    while (cmdLock_.test_and_set(std::memory_order_acquire)) { /* UI-side spin only */ }
    const bool ok = cmdRing_.push(c);
    cmdLock_.clear(std::memory_order_release);
    if (!ok) {
        LOGE("Command ring full, dropped cmd %d", c.type);
        if ((c.type == CMD_SET_CLIP || c.type == CMD_SET_REC_BUF) && c.p)
            delete static_cast<ClipData*>(c.p);
    }
}

void AudioEngine::play() { Cmd c; c.type = CMD_PLAY; postCmd(c); }
void AudioEngine::stopTransport() { Cmd c; c.type = CMD_STOP; postCmd(c); }

void AudioEngine::slotPress(int t, int s) {
    if (t < 0 || t >= kTracks || s < 0 || s >= kScenes) return;
    Cmd c; c.type = CMD_SLOT_PRESS; c.a = t; c.b = s; postCmd(c);
}
void AudioEngine::launchScene(int s) {
    if (s < 0 || s >= kScenes) return;
    Cmd c; c.type = CMD_LAUNCH_SCENE; c.a = s; postCmd(c);
}
void AudioEngine::stopTrack(int t) {
    if (t < 0 || t >= kTracks) return;
    Cmd c; c.type = CMD_STOP_TRACK; c.a = t; postCmd(c);
}
void AudioEngine::stopAll() { Cmd c; c.type = CMD_STOP_ALL; postCmd(c); }

void AudioEngine::setArm(int t, bool a) {
    if (t < 0 || t >= kTracks) return;
    armed_[t].store(a ? 1 : 0);
    if (a) ensureRecBuf(t);
}

void AudioEngine::setGlobal(int id, float v) {
    if (id < 0 || id >= GP_COUNT) return;
    if (id == GP_BPM) v = std::clamp(v, 60.0f, 200.0f);
    globalParam_[id].store(v);
}

void AudioEngine::setTrackParam(int t, int id, float v) {
    if (t < 0 || t >= kTracks || id < 0 || id >= TP_COUNT) return;
    trackParam_[t][id].store(v);
}

void AudioEngine::setClipParams(int t, int s, int warp, int bars, int transpose) {
    if (t < 0 || t >= kTracks || s < 0 || s >= kScenes) return;
    const int i = t * kScenes + s;
    clipWarp_[i].store(warp ? 1 : 0);
    clipBars_[i].store(std::clamp(bars, 1, 16));
    clipTranspose_[i].store(std::clamp(transpose, -12, 12));
}

void AudioEngine::ensureRecBuf(int t) {
    if (recBufSent_[t]) return;
    auto* c = new ClipData();
    const int cap = (int)(kRecordSeconds * kSampleRate);
    c->data = new float[(size_t)cap * 2]();
    c->frames = cap;
    Cmd cmd; cmd.type = CMD_SET_REC_BUF; cmd.a = t; cmd.p = c;
    postCmd(cmd);
    recBufSent_[t] = true;
}

void AudioEngine::setClipOwned(int t, int s, ClipData* c, const std::string& name, bool) {
    const int i = t * kScenes + s;
    names_[i] = name;
    slotFrames_[i] = c->frames;
    slotHas_[i].store(1);
    Cmd cmd; cmd.type = CMD_SET_CLIP; cmd.a = t; cmd.b = s; cmd.c = 0; cmd.p = c;
    postCmd(cmd);
}

bool AudioEngine::loadClip(int t, int s, const float* src, int frames, const char* name) {
    if (t < 0 || t >= kTracks || s < 0 || s >= kScenes || !src || frames < 960) return false;
    auto* c = new ClipData();
    c->data = new float[(size_t)frames * 2];
    std::memcpy(c->data, src, sizeof(float) * (size_t)frames * 2);
    c->frames = frames;
    // Auto-detect length in bars at 120 BPM (96000 frames per 4/4 bar).
    static const int cand[5] = {1, 2, 4, 8, 16};
    const double bars120 = (double)frames / 96000.0;
    int best = 1;
    double bestDiff = 1e9;
    for (int b : cand) {
        const double d = std::fabs(bars120 - (double)b);
        if (d < bestDiff) { bestDiff = d; best = b; }
    }
    c->defaultBars = best;
    c->beats = best * 4.0;
    const int i = t * kScenes + s;
    clipBars_[i].store(best);
    clipWarp_[i].store(1);
    clipTranspose_[i].store(0);
    setClipOwned(t, s, c, name ? name : "Clip", true);
    return true;
}

// ============================================================ polling
void AudioEngine::getStatus(float* out) const {
    std::fill(out, out + kStatusSize, 0.0f);
    const double b = beatsOut_.load();
    out[0] = (float)playing_.load();
    out[1] = globalParam_[GP_BPM].load();
    out[2] = (float)((long long)std::floor(b / 4.0) + 1);
    out[3] = (float)(((long long)std::floor(b)) % 4 + 1);
    out[4] = (float)((int)std::floor((b - std::floor(b)) * 4.0) + 1);
    out[5] = (float)b;
    for (int i = 0; i < kSlots; ++i) out[8 + i] = (float)slotState_[i].load();
    for (int t = 0; t < kTracks; ++t) {
        out[28 + t] = trackProgress_[t].load();
        out[32 + t] = (float)trackScene_[t].load();
        out[36 + t] = (float)trackStopQueued_[t].load();
    }
}

void AudioEngine::getMeters(float* out) const {
    for (int i = 0; i < kMeterValues; ++i) out[i] = meters_[i].load();
}

void AudioEngine::drainGraveyard(bool force) {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < graveyard_.size();) {
        if (force || now - graveyard_[i].second > std::chrono::seconds(3)) {
            delete graveyard_[i].first;
            graveyard_[i] = graveyard_.back();
            graveyard_.pop_back();
        } else {
            ++i;
        }
    }
}

bool AudioEngine::pollEvent(int* out) {
    drainGraveyard(false);
    Cmd e;
    while (eventRing_.pop(e)) {
        if (e.type == EV_RETIRE) {
            if (e.p) graveyard_.emplace_back(static_cast<ClipData*>(e.p), std::chrono::steady_clock::now());
            continue;
        }
        if (e.type == EV_REC_DONE) {
            const int t = e.a, s = e.b, i = t * kScenes + s;
            auto* big = static_cast<ClipData*>(e.p);
            recBufSent_[t] = false;
            // Replace the 40 s capture buffer by a trimmed copy (same audio, swapped in place).
            auto* small = new ClipData();
            small->frames = big->frames;
            small->beats = big->beats;
            small->defaultBars = big->defaultBars;
            small->data = new float[(size_t)big->frames * 2];
            std::memcpy(small->data, big->data, sizeof(float) * (size_t)big->frames * 2);
            slotFrames_[i] = small->frames;
            names_[i] = "Rec " + std::to_string(s + 1);
            Cmd cmd; cmd.type = CMD_SET_CLIP; cmd.a = t; cmd.b = s; cmd.c = 1; cmd.p = small;
            postCmd(cmd);
            if (armed_[t].load()) ensureRecBuf(t);
            out[0] = EV_REC_DONE; out[1] = t; out[2] = s; out[3] = big->frames;
            out[4] = big->defaultBars;
            return true;
        }
    }
    return false;
}

std::string AudioEngine::clipName(int t, int s) const {
    if (t < 0 || t >= kTracks || s < 0 || s >= kScenes) return "";
    return names_[t * kScenes + s];
}

void AudioEngine::clipInfo(int t, int s, int* out) const {
    const int i = t * kScenes + s;
    out[0] = slotHas_[i].load();
    out[1] = clipBars_[i].load();
    out[2] = clipWarp_[i].load();
    out[3] = clipTranspose_[i].load();
    out[4] = slotFrames_[i];
}

// ============================================================ audio thread: helpers
double AudioEngine::quantBeats() const {
    const int q = std::clamp((int)globalParam_[GP_QUANT].load(), 0, 4);
    return kQuantTable[q];
}

AudioEngine::VoiceParams AudioEngine::paramsFor(int slot) const {
    VoiceParams p;
    if (slot < 0 || slot >= kSlots) return p;
    const ClipData* c = clips_[slot];
    const int bars = clipBars_[slot].load();
    p.warp = clipWarp_[slot].load() != 0;
    p.loopBeats = (c && bars == c->defaultBars) ? c->beats : bars * 4.0;
    if (p.loopBeats < 0.25) p.loopBeats = 0.25;
    p.pitch = std::exp2((double)clipTranspose_[slot].load() / 12.0);
    return p;
}

void AudioEngine::pushEvent(const Cmd& e) { eventRing_.push(e); }

bool AudioEngine::anyPending() const {
    for (int t = 0; t < kTracks; ++t)
        if (pendingScene_[t] >= 0 || pendingStop_[t] || recState_[t] == 1 || recState_[t] == 3) return true;
    return false;
}

void AudioEngine::startTransportFromZero() {
    playingA_ = true;
    beats_ = 0.0;
    stopPressedOnce_ = false;
    if (globalParam_[GP_METRONOME].load() > 0.5f) { clickPos_ = 0; clickFreq_ = 1500.0f; }
}

void AudioEngine::stopVoice(Voice& v, bool fadeOut, int t) {
    if (fadeOut && v.clip && v.env > 0.001f) {
        fade_[t] = v;
        fade_[t].envTarget = 0.0f;
        fade_[t].envInc = -1.0f / 240.0f;
    }
    v.clip = nullptr;
    v.slot = -1;
    v.env = 0.0f;
}

void AudioEngine::launchNow(int t, int s) {
    const int idx = t * kScenes + s;
    ClipData* c = clips_[idx];
    if (!c) return;
    stopVoice(cur_[t], true, t);
    Voice& v = cur_[t];
    v.clip = c;
    v.slot = idx;
    v.phase = 0.0;
    v.pos = 0.0;
    v.started = false;
    v.env = 0.0f;
    v.envTarget = 1.0f;
    v.envInc = 1.0f / 240.0f;
    pendingScene_[t] = -1;
    pendingStop_[t] = false;
}

void AudioEngine::stopNow(int t) {
    stopVoice(cur_[t], true, t);
    pendingStop_[t] = false;
}

void AudioEngine::startRecordNow(int t) {
    recState_[t] = 2;
    recFrames_[t] = 0;
    recBeats_[t] = 0.0;
    pendingScene_[t] = -1;
    pendingStop_[t] = false;
    stopVoice(cur_[t], true, t);
}

void AudioEngine::finishRecordNow(int t) {
    ClipData* c = recBuf_[t];
    const int s = recScene_[t];
    const int frames = recFrames_[t];
    recState_[t] = 0;
    if (!c || frames < 960) return;
    double beats = std::max(0.25, std::round(recBeats_[t] * 4.0) / 4.0);
    c->frames = frames;
    c->beats = beats;
    c->defaultBars = std::clamp((int)std::lround(beats / 4.0), 1, 16);
    recBuf_[t] = nullptr;
    const int idx = t * kScenes + s;
    clipWarp_[idx].store(1);
    clipBars_[idx].store(c->defaultBars);
    clipTranspose_[idx].store(0);
    installClip(t, s, c, false);
    launchNow(t, s);
    Cmd e; e.type = EV_REC_DONE; e.a = t; e.b = s; e.c = frames; e.d = beats; e.p = c;
    pushEvent(e);
}

void AudioEngine::installClip(int t, int s, ClipData* c, bool keepVoice) {
    const int idx = t * kScenes + s;
    ClipData* old = clips_[idx];
    clips_[idx] = c;
    slotHas_[idx].store(1);
    if (!old) return;
    if (keepVoice) {
        if (cur_[t].clip == old) cur_[t].clip = c;
        if (fade_[t].clip == old) fade_[t].clip = c;
    } else if (cur_[t].clip == old) {
        stopVoice(cur_[t], true, t);
    }
    Cmd e; e.type = EV_RETIRE; e.p = old;
    pushEvent(e);   // freed on the main thread after a grace period
}

void AudioEngine::processBoundary() {
    for (int t = 0; t < kTracks; ++t) {
        if (recState_[t] == 3) finishRecordNow(t);
        if (recState_[t] == 1) startRecordNow(t);
        if (pendingStop_[t]) stopNow(t);
        if (pendingScene_[t] >= 0) launchNow(t, pendingScene_[t]);
    }
    pendingFlag_ = anyPending();
}


void AudioEngine::slotPressAudio(int t, int s) {
    const int idx = t * kScenes + s;
    const double q = immediate_ ? 0.0 : quantBeats();
    // --- interaction with a record in progress on this track
    if (recState_[t] != 0) {
        if (recScene_[t] != s) return;
        if (recState_[t] == 1) { recState_[t] = 0; return; }          // cancel queued start
        if (recState_[t] == 2) {                                       // stop recording
            if (q <= 0.0 || !playingA_) finishRecordNow(t);
            else { recState_[t] = 3; pendingFlag_ = true; }
        }
        return;
    }
    if (clips_[idx]) {                                                 // launch clip
        if (!playingA_) { startTransportFromZero(); launchNow(t, s); return; }
        if (q <= 0.0) { launchNow(t, s); return; }
        pendingScene_[t] = s;
        pendingStop_[t] = false;
        pendingFlag_ = true;
        return;
    }
    if (armed_[t].load() && recBuf_[t]) {                              // session record
        recScene_[t] = s;
        if (!playingA_) { startTransportFromZero(); startRecordNow(t); return; }
        if (q <= 0.0) { startRecordNow(t); return; }
        recState_[t] = 1;
        pendingFlag_ = true;
        return;
    }
    // empty slot: clip-stop button
    if (cur_[t].clip) {
        if (!playingA_ || q <= 0.0) { stopNow(t); }
        else { pendingStop_[t] = true; pendingScene_[t] = -1; pendingFlag_ = true; }
    } else {
        pendingScene_[t] = -1;
        pendingFlag_ = anyPending();
    }
}

void AudioEngine::handleCommand(const Cmd& c) {
    switch (c.type) {
        case CMD_PLAY:
            if (!playingA_) {
                if (beats_ <= 0.0) startTransportFromZero();
                else playingA_ = true;
            }
            stopPressedOnce_ = false;
            break;
        case CMD_STOP:
            if (playingA_) {
                playingA_ = false;
                for (int t = 0; t < kTracks; ++t) {
                    stopVoice(cur_[t], true, t);
                    pendingScene_[t] = -1;
                    pendingStop_[t] = false;
                    if (recState_[t] != 0) recState_[t] = 0;   // abort record, buffer is reused
                }
                pendingFlag_ = false;
                stopPressedOnce_ = true;
            } else {
                beats_ = 0.0;                                   // second Stop: back to 1.1.1
                stopPressedOnce_ = false;
            }
            break;
        case CMD_SLOT_PRESS: slotPressAudio(c.a, c.b); break;
        case CMD_LAUNCH_SCENE:
            if (!playingA_) { startTransportFromZero(); immediate_ = true; }
            for (int t = 0; t < kTracks; ++t) slotPressAudio(t, c.a);
            immediate_ = false;
            break;
        case CMD_STOP_TRACK: {
            const int t = c.a;
            if (recState_[t] != 0) break;
            if (cur_[t].clip) {
                if (!playingA_ || quantBeats() <= 0.0) stopNow(t);
                else { pendingStop_[t] = true; pendingScene_[t] = -1; pendingFlag_ = true; }
            } else { pendingScene_[t] = -1; }
            break;
        }
        case CMD_STOP_ALL:
            for (int t = 0; t < kTracks; ++t) {
                if (recState_[t] != 0) continue;
                if (cur_[t].clip) {
                    if (!playingA_ || quantBeats() <= 0.0) stopNow(t);
                    else { pendingStop_[t] = true; pendingScene_[t] = -1; pendingFlag_ = true; }
                } else { pendingScene_[t] = -1; }
            }
            break;
        case CMD_SET_CLIP:
            installClip(c.a, c.b, static_cast<ClipData*>(c.p), c.c != 0);
            break;
        case CMD_SET_REC_BUF:
            if (recBuf_[c.a]) { Cmd e; e.type = EV_RETIRE; e.p = recBuf_[c.a]; pushEvent(e); }
            recBuf_[c.a] = static_cast<ClipData*>(c.p);
            break;
        default: break;
    }
}

void AudioEngine::renderVoice(Voice& v, double beatInc, const VoiceParams& vp, float& l, float& r) {
    l = r = 0.0f;
    ClipData* c = v.clip;
    if (!c) return;
    const int frames = c->frames;
    const int xf = dsp::crossfadeFrames(frames);
    const double M = (double)(frames - xf);
    const double loopBeats = vp.loopBeats;
    float sl = 0.0f, sr = 0.0f;
    if (vp.warp) {
        while (v.phase >= loopBeats) v.phase -= loopBeats;
        const double playhead = v.phase / loopBeats * M;
        if (!v.started) { v.grain.reset(playhead, vp.pitch, M); v.started = true; }
        v.grain.process(c->data, frames, xf, M, playhead, vp.pitch, sl, sr);
        v.phase += beatInc;
    } else {
        if (v.pos >= M) v.pos = std::fmod(v.pos, M);
        dsp::readLoop(c->data, frames, xf, v.pos, sl, sr);
        v.pos += vp.pitch;
        if (v.pos >= M) v.pos -= M;
        v.phase += beatInc;
        while (v.phase >= loopBeats) v.phase -= loopBeats;
    }
    if (v.envInc != 0.0f) {
        v.env += v.envInc;
        if (v.envInc > 0.0f && v.env >= v.envTarget) { v.env = v.envTarget; v.envInc = 0.0f; }
        else if (v.envInc < 0.0f && v.env <= v.envTarget) {
            v.env = v.envTarget;
            v.envInc = 0.0f;
            if (v.envTarget <= 0.0f) { v.clip = nullptr; v.slot = -1; }
        }
    }
    l = sl * v.env;
    r = sr * v.env;
}

void AudioEngine::publishStates() {
    for (int i = 0; i < kSlots; ++i) slotState_[i].store(SS_STOPPED);
    for (int t = 0; t < kTracks; ++t) {
        int activeScene = -1;
        float progress = 0.0f;
        const Voice& v = cur_[t];
        if (v.clip && v.slot >= 0) {
            activeScene = v.slot - t * kScenes;
            slotState_[v.slot].store(pendingStop_[t] ? SS_STOP_QUEUED : SS_PLAYING);
            const VoiceParams vp = paramsFor(v.slot);
            const int xf = dsp::crossfadeFrames(v.clip->frames);
            const double M = (double)(v.clip->frames - xf);
            progress = vp.warp ? (float)(v.phase / vp.loopBeats) : (float)(v.pos / M);
            progress = std::clamp(progress, 0.0f, 1.0f);
        }
        if (pendingScene_[t] >= 0) slotState_[t * kScenes + pendingScene_[t]].store(SS_QUEUED);
        if (recState_[t] != 0) {
            const int st = recState_[t] == 1 ? SS_REC_QUEUED : recState_[t] == 2 ? SS_RECORDING : SS_REC_STOP_QUEUED;
            slotState_[t * kScenes + recScene_[t]].store(st);
            if (recState_[t] != 1) {
                const double cap = kRecordSeconds * kSampleRate;
                progress = (float)std::min(1.0, (double)recFrames_[t] / cap);
            }
        }
        trackScene_[t].store(activeScene);
        trackProgress_[t].store(progress);
        trackStopQueued_[t].store(pendingStop_[t] ? 1 : 0);
    }
    playing_.store(playingA_ ? 1 : 0);
    beatsOut_.store(beats_);
}

// ============================================================ audio callback
oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream*, void* audioData,
                                                   int32_t numFrames) {
    float* out = static_cast<float*>(audioData);
    int32_t done = 0;
    while (done < numFrames) {
        const int n = std::min<int32_t>(numFrames - done, kMaxBlock);
        // ---- commands from the UI (lock-free ring)
        Cmd cmd;
        while (cmdRing_.pop(cmd)) handleCommand(cmd);

        // ---- parameter snapshot
        const float bpm = std::clamp(globalParam_[GP_BPM].load(), 60.0f, 200.0f);
        const double beatInc = (double)bpm / 60.0 / (double)kSampleRate;
        const double q = quantBeats();
        const bool metOn = globalParam_[GP_METRONOME].load() > 0.5f;
        master_.target = globalParam_[GP_MASTER_VOL].load();
        retA_.target = globalParam_[GP_RET_A_VOL].load();
        retB_.target = globalParam_[GP_RET_B_VOL].load();
        clickVol_.target = globalParam_[GP_CLICK_VOL].load();
        const float xpos = std::clamp(globalParam_[GP_CROSSFADER].load(), 0.0f, 1.0f);
        const float gA = std::cos(xpos * dsp::kPi * 0.5f);   // constant-power crossfade
        const float gB = std::sin(xpos * dsp::kPi * 0.5f);
        delay_.setDelaySamples(dsp::PingPongDelay::delaySamples((int)globalParam_[GP_DELAY_MODE].load(), bpm));

        bool anySolo = false;
        for (int t = 0; t < kTracks; ++t)
            if (trackParam_[t][TP_SOLO].load() > 0.5f) anySolo = true;

        float drive[kTracks];
        int filtType[kTracks];
        VoiceParams vpCur[kTracks], vpFade[kTracks];
        for (int t = 0; t < kTracks; ++t) {
            vol_[t].target = trackParam_[t][TP_VOLUME].load();
            pan_[t].target = std::clamp(trackParam_[t][TP_PAN].load(), -1.0f, 1.0f);
            sendA_[t].target = trackParam_[t][TP_SEND_A].load();
            sendB_[t].target = trackParam_[t][TP_SEND_B].load();
            const bool active = trackParam_[t][TP_ACTIVE].load() > 0.5f;
            const bool solo = trackParam_[t][TP_SOLO].load() > 0.5f;
            const int xa = (int)trackParam_[t][TP_XF_ASSIGN].load();
            const float xg = (xa == 0) ? gA : (xa == 2) ? gB : 1.0f;
            gate_[t].target = (active && (!anySolo || solo)) ? xg : 0.0f;
            drive[t] = std::clamp(trackParam_[t][TP_DRIVE].load(), 0.0f, 1.0f);
            filtType[t] = (int)trackParam_[t][TP_FILTER_TYPE].load();
            const float cutoff = trackParam_[t][TP_CUTOFF].load();
            const float res = trackParam_[t][TP_RESONANCE].load();
            if (filtType[t] != 0 && (filtType[t] != lastFiltType_[t] || cutoff != lastCutoff_[t] || res != lastRes_[t])) {
                if (filtType[t] == 1) filt_[t].setLowPass(cutoff, res);
                else filt_[t].setHighPass(cutoff, res);
            }
            lastFiltType_[t] = filtType[t];
            lastCutoff_[t] = cutoff;
            lastRes_[t] = res;
            vpCur[t] = paramsFor(cur_[t].slot);
            vpFade[t] = paramsFor(fade_[t].slot);
        }

        // ---- full duplex input (non-blocking read)
        int inGot = 0;
        if (inStream_ && inChannels_ > 0) {
            auto res = inStream_->read(inBuf_, n, 0);
            if (res) inGot = res.value();
        }
        const int ich = std::max(inChannels_, 1);

        float bp[kMeterChannels][2] = {};
        float bs[kMeterChannels][2] = {};

        for (int i = 0; i < n; ++i) {
            // ---- master clock & quantised events
            if (playingA_) {
                const double nb = beats_ + beatInc;
                if (q > 0.0) {
                    if (std::floor(nb / q) > std::floor(beats_ / q)) processBoundary();
                } else if (pendingFlag_) {
                    processBoundary();
                }
                if (std::floor(nb) > std::floor(beats_) && metOn) {
                    clickPos_ = 0;
                    clickFreq_ = (((long long)std::floor(nb)) % 4 == 0) ? 1500.0f : 1000.0f;
                }
                beats_ = nb;
            }

            float inL = 0.0f, inR = 0.0f;
            if (i < inGot) {
                inL = inBuf_[i * ich];
                inR = ich > 1 ? inBuf_[i * ich + 1] : inL;
            }

            float mixL = 0.0f, mixR = 0.0f, busA = 0.0f, busB = 0.0f;
            for (int t = 0; t < kTracks; ++t) {
                // ---- session record
                if (recState_[t] == 2 || recState_[t] == 3) {
                    ClipData* rb = recBuf_[t];
                    if (rb && recFrames_[t] < rb->frames) {
                        rb->data[2 * recFrames_[t]] = inL;
                        rb->data[2 * recFrames_[t] + 1] = inR;
                        ++recFrames_[t];
                        recBeats_[t] += playingA_ ? beatInc : 0.0;
                        if (recFrames_[t] >= rb->frames) finishRecordNow(t);
                    }
                }
                // ---- clip playback
                float l = 0.0f, r = 0.0f, fl = 0.0f, fr = 0.0f;
                if (playingA_) renderVoice(cur_[t], beatInc, vpCur[t], l, r);
                renderVoice(fade_[t], playingA_ ? beatInc : 0.0, vpFade[t], fl, fr);
                l += fl;
                r += fr;
                // ---- channel strip: filter -> soft clipper -> fader/pan
                if (filtType[t] != 0) filt_[t].process(l, r);
                if (drive[t] > 0.001f) {
                    l = dsp::softClip(l, drive[t]);
                    r = dsp::softClip(r, drive[t]);
                }
                const float g = vol_[t].next() * gate_[t].next();
                const float p = pan_[t].next();
                const float ang = (p + 1.0f) * dsp::kPi * 0.25f;
                const float gl = std::min(1.0f, 1.41421356f * std::cos(ang));
                const float gr = std::min(1.0f, 1.41421356f * std::sin(ang));
                l *= g * gl;
                r *= g * gr;
                bp[t][0] = std::max(bp[t][0], std::fabs(l));
                bp[t][1] = std::max(bp[t][1], std::fabs(r));
                bs[t][0] += l * l;
                bs[t][1] += r * r;
                const float mono = (l + r) * 0.5f;
                busA += mono * sendA_[t].next();
                busB += mono * sendB_[t].next();
                mixL += l;
                mixR += r;
            }

            // ---- returns
            float rl, rr, dl, dr;
            reverb_.process(busA, rl, rr);
            delay_.process(busB, dl, dr);
            const float ga = retA_.next(), gb = retB_.next();
            rl *= ga; rr *= ga; dl *= gb; dr *= gb;
            bp[4][0] = std::max(bp[4][0], std::fabs(rl)); bp[4][1] = std::max(bp[4][1], std::fabs(rr));
            bs[4][0] += rl * rl; bs[4][1] += rr * rr;
            bp[5][0] = std::max(bp[5][0], std::fabs(dl)); bp[5][1] = std::max(bp[5][1], std::fabs(dr));
            bs[5][0] += dl * dl; bs[5][1] += dr * dr;
            mixL += rl + dl;
            mixR += rr + dr;

            // ---- metronome click (sine, accented on the downbeat)
            float click = 0.0f;
            if (clickPos_ >= 0) {
                const float tt = (float)clickPos_ / (float)kSampleRate;
                float env = 1.0f - (float)clickPos_ / 1440.0f;
                env *= env;
                click = std::sin(dsp::kTwoPi * clickFreq_ * tt) * env * 0.8f;
                if (++clickPos_ >= 1440) clickPos_ = -1;
            }

            // ---- master: volume, click, limiter
            const float mv = master_.next();
            float ol = mixL * mv + click * clickVol_.next();
            float orr = mixR * mv + click * clickVol_.cur;
            limiter_.process(ol, orr);
            if (!std::isfinite(ol)) ol = 0.0f;
            if (!std::isfinite(orr)) orr = 0.0f;
            out[2 * (done + i)] = ol;
            out[2 * (done + i) + 1] = orr;
            bp[6][0] = std::max(bp[6][0], std::fabs(ol));
            bp[6][1] = std::max(bp[6][1], std::fabs(orr));
            bs[6][0] += ol * ol;
            bs[6][1] += orr * orr;
        }

        // ---- meters (peak with smooth fall-off, block RMS)
        const float decay = std::exp(-(float)n / (float)kSampleRate / 0.35f);
        for (int c = 0; c < kMeterChannels; ++c) {
            for (int ch = 0; ch < 2; ++ch) {
                meterPeak_[c][ch] = std::max(bp[c][ch], meterPeak_[c][ch] * decay);
                meters_[c * 4 + ch].store(meterPeak_[c][ch], std::memory_order_relaxed);
                meters_[c * 4 + 2 + ch].store(std::sqrt(bs[c][ch] / (float)n), std::memory_order_relaxed);
            }
        }
        publishStates();
        done += n;
    }
    return oboe::DataCallbackResult::Continue;
}

// ============================================================ demo set (synthesised at start-up)
namespace {

constexpr int kBeatF = 24000;           // frames per beat at 120 BPM
constexpr int kStepF = kBeatF / 4;      // 16th note
constexpr int kBarF = kBeatF * 4;
constexpr int kLoopF = kBarF * 2;       // every demo clip is a 2-bar loop
constexpr float kSr = (float)kSampleRate;

struct Rng {
    uint32_t s = 0x1234567u;
    float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s & 0xFFFFFF) / 8388607.5f - 1.0f;
    }
};

struct Buf {
    std::vector<float> d;
    int frames;
    explicit Buf(int f) : d((size_t)f * 2, 0.0f), frames(f) {}
    void add(int i, float l, float r) {
        i %= frames;                       // tails wrap around so loops are seamless
        d[2 * (size_t)i] += l;
        d[2 * (size_t)i + 1] += r;
    }
    void normalize(float peakTarget) {
        float pk = 1e-6f;
        for (float v : d) pk = std::max(pk, std::fabs(v));
        const float g = peakTarget / pk;
        for (float& v : d) v *= g;
    }
};

inline float midiHz(float m) { return 440.0f * std::exp2((m - 69.0f) / 12.0f); }

void addKick(Buf& b, int start, float vel) {
    float ph = 0.0f;
    for (int i = 0; i < 24000; ++i) {
        const float t = (float)i / kSr;
        ph += dsp::kTwoPi * (45.0f + 130.0f * std::exp(-t * 28.0f)) / kSr;
        float s = std::sin(ph) * std::exp(-t * 6.5f);
        if (i < 60) s += (1.0f - (float)i / 60.0f) * 0.3f;
        s = dsp::fastTanh(s * 1.6f) * vel * 0.9f;
        b.add(start + i, s, s);
    }
}

void addSnare(Buf& b, int start, float vel, Rng& rng) {
    float prev = 0.0f;
    for (int i = 0; i < 16000; ++i) {
        const float t = (float)i / kSr;
        const float n = rng.next();
        const float hp = n - prev * 0.6f;
        prev = n;
        const float body = std::sin(dsp::kTwoPi * 185.0f * t) * std::exp(-t * 22.0f) * 0.5f;
        const float s = dsp::fastTanh((body + hp * std::exp(-t * 16.0f) * 0.55f) * 1.4f) * vel * 0.7f;
        b.add(start + i, s, s);
    }
}

void addClap(Buf& b, int start, float vel, Rng& rng) {
    float prev = 0.0f;
    for (int i = 0; i < 9000; ++i) {
        const float t = (float)i / kSr;
        const float n = rng.next();
        const float hp = n - prev * 0.7f;
        prev = n;
        float env = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float tk = t - 0.015f * (float)k;
            if (tk >= 0.0f) env += std::exp(-tk * (k == 2 ? 28.0f : 140.0f));
        }
        const float s = hp * env * 0.22f * vel;
        b.add(start + i, s * 0.9f, s * 1.1f);
    }
}

void addHat(Buf& b, int start, float vel, bool open, Rng& rng) {
    float prev = 0.0f;
    const int len = open ? 18000 : 6000;
    for (int i = 0; i < len; ++i) {
        const float t = (float)i / kSr;
        const float n = rng.next();
        const float hp = n - prev;
        prev = n;
        const float s = hp * std::exp(-t * (open ? 16.0f : 85.0f)) * vel * 0.3f;
        b.add(start + i, s * 0.95f, s * 1.05f);
    }
}

void addShaker(Buf& b, int start, float vel, Rng& rng) {
    float prev = 0.0f;
    for (int i = 0; i < 4800; ++i) {
        const float t = (float)i / kSr;
        const float n = rng.next();
        const float hp = n - prev * 0.8f;
        prev = n;
        const float s = hp * std::min(1.0f, t * 150.0f) * std::exp(-t * 45.0f) * vel * 0.25f;
        b.add(start + i, s * 1.1f, s * 0.9f);
    }
}

void addSub(Buf& b, int start, int len, float freq, float vel) {
    float ph = 0.0f;
    for (int i = 0; i < len; ++i) {
        ph += dsp::kTwoPi * freq / kSr;
        float amp = std::min(1.0f, (float)i / 96.0f);
        if (i > len - 480) amp *= std::max(0.0f, (float)(len - i) / 480.0f);
        const float s = dsp::fastTanh((std::sin(ph) + 0.25f * std::sin(2.0f * ph)) * 1.3f) * amp * vel * 0.8f;
        b.add(start + i, s, s);
    }
}

void addAcid(Buf& b, int start, int len, float freq, float vel, float cutBase, float cutEnv, float damp) {
    float ph = 0.0f, low = 0.0f, band = 0.0f;
    for (int i = 0; i < len; ++i) {
        const float t = (float)i / kSr;
        const float fc = std::min(4000.0f, cutBase + cutEnv * std::exp(-t * 14.0f));
        const float f = 2.0f * std::sin(dsp::kPi * fc / kSr);
        const float saw = 2.0f * ph - 1.0f;
        ph += freq / kSr;
        if (ph >= 1.0f) ph -= 1.0f;
        const float high = saw - low - damp * band;
        band += f * high;
        low += f * band;
        float amp = std::min(1.0f, (float)i / 48.0f);
        if (i > len - 240) amp *= std::max(0.0f, (float)(len - i) / 240.0f);
        const float s = dsp::fastTanh(low * 2.2f) * amp * vel * 0.6f;
        b.add(start + i, s, s);
    }
}

void addPadNote(Buf& b, int start, int len, float freq, float vel, float cutoff) {
    static const float det[3] = {0.994f, 1.0f, 1.006f};
    static const float gl[3] = {1.0f, 0.65f, 0.3f};
    static const float gr[3] = {0.3f, 0.65f, 1.0f};
    const int att = 14400, rel = 16800;
    const float a = 1.0f - std::exp(-dsp::kTwoPi * cutoff / kSr);
    float ph[3] = {0.0f, 0.31f, 0.67f};
    float lpL = 0.0f, lpR = 0.0f;
    for (int i = 0; i < len + rel; ++i) {
        float env = (i < att) ? (float)i / (float)att : 1.0f;
        if (i > len) env *= std::max(0.0f, 1.0f - (float)(i - len) / (float)rel);
        float l = 0.0f, r = 0.0f;
        for (int v = 0; v < 3; ++v) {
            const float saw = 2.0f * ph[v] - 1.0f;
            ph[v] += freq * det[v] / kSr;
            if (ph[v] >= 1.0f) ph[v] -= 1.0f;
            l += saw * gl[v];
            r += saw * gr[v];
        }
        lpL += a * (l - lpL);
        lpR += a * (r - lpR);
        b.add(start + i, lpL * env * vel * 0.12f, lpR * env * vel * 0.12f);
    }
}

}  // namespace

void AudioEngine::buildDemoSet() {
    Rng rng;
    auto finish = [&](int t, int s, Buf& b, float level, const char* name) {
        b.normalize(level);
        auto* c = new ClipData();
        c->frames = b.frames;
        c->beats = 8.0;
        c->defaultBars = 2;
        c->data = new float[b.d.size()];
        std::memcpy(c->data, b.d.data(), b.d.size() * sizeof(float));
        const int idx = t * kScenes + s;
        clipBars_[idx].store(2);
        clipWarp_[idx].store(1);
        clipTranspose_[idx].store(0);
        setClipOwned(t, s, c, name, true);
    };

    // ---------------- Track 1: kick & snare
    for (int s = 0; s < 3; ++s) {
        Buf b(kLoopF);
        for (int beat = 0; beat < 8; ++beat) addKick(b, beat * kBeatF, 1.0f);
        if (s == 2) addKick(b, 7 * kBeatF + kStepF * 2, 0.7f);
        if (s >= 1)
            for (int bar = 0; bar < 2; ++bar) {
                addSnare(b, bar * kBarF + kBeatF, 1.0f, rng);
                addSnare(b, bar * kBarF + 3 * kBeatF, 1.0f, rng);
                if (s == 2) {
                    addClap(b, bar * kBarF + kBeatF, 0.8f, rng);
                    addClap(b, bar * kBarF + 3 * kBeatF, 0.8f, rng);
                }
            }
        if (s == 2)
            for (int k = 0; k < 4; ++k)
                addSnare(b, kBarF + 3 * kBeatF + k * kStepF, 0.45f + 0.18f * (float)k, rng);
        finish(0, s, b, 0.55f, s == 0 ? "Kick" : s == 1 ? "Kick+Snare" : "Kick Snare Fill");
    }

    // ---------------- Track 2: hats & shaker
    for (int s = 0; s < 3; ++s) {
        Buf b(kLoopF);
        if (s == 0) {
            for (int i = 0; i < 8; ++i) addHat(b, i * kBeatF + kBeatF / 2, 0.9f, true, rng);
        } else {
            static const float acc[4] = {1.0f, 0.4f, 0.75f, 0.4f};
            for (int i = 0; i < 32; ++i) addHat(b, i * kStepF, acc[i % 4], false, rng);
            if (s == 2) {
                for (int i = 0; i < 8; ++i) addHat(b, i * kBeatF + kBeatF / 2, 0.8f, true, rng);
                for (int i = 1; i < 32; i += 2) addShaker(b, i * kStepF + 300, 0.8f, rng);
            }
        }
        finish(1, s, b, 0.3f, s == 0 ? "Offbeat Hat" : s == 1 ? "16th Hats" : "Hats+Shaker");
    }

    // ---------------- Track 3: sub / acid bass
    static const int acidSemi[2][16] = {
        {0, -1, 0, 12, -1, 0, 3, -1, 0, -1, 7, -1, 10, -1, 0, 5},
        {0, -1, 0, 12, -1, 0, 3, -1, 0, 12, -1, 7, 5, -1, 3, 0}};
    for (int s = 0; s < 3; ++s) {
        Buf b(kLoopF);
        for (int bar = 0; bar < 2; ++bar) {
            const float root = bar == 0 ? 33.0f : 29.0f;   // A1, F1
            if (s == 0) {
                for (int i = 0; i < 4; ++i)
                    addSub(b, bar * kBarF + i * kBeatF + kBeatF / 2, (int)(kBeatF * 0.42f), midiHz(root), 1.0f);
            } else {
                for (int i = 0; i < 16; ++i) {
                    const int semi = acidSemi[bar][i];
                    if (semi < 0) continue;
                    const bool accent = (semi == 12) || (i % 4 == 0);
                    const float cutBase = s == 1 ? 260.0f : 420.0f;
                    const float cutEnv = s == 1 ? 1500.0f : 2600.0f;
                    addAcid(b, bar * kBarF + i * kStepF, (int)(kStepF * 0.8f),
                            midiHz(root + (float)semi), accent ? 1.0f : 0.7f, cutBase,
                            accent ? cutEnv * 1.3f : cutEnv, s == 1 ? 0.35f : 0.2f);
                }
            }
        }
        finish(2, s, b, 0.5f, s == 0 ? "Deep Sub" : s == 1 ? "Acid Bass" : "Acid Peak");
    }

    // ---------------- Track 4: warm detuned pads
    static const int chordAm[3] = {57, 60, 64};
    static const int chordF[3] = {53, 57, 60};
    static const int chordAm7[4] = {57, 60, 64, 67};
    static const int chordFmaj7[4] = {53, 57, 60, 64};
    for (int s = 0; s < 3; ++s) {
        Buf b(kLoopF);
        const float cutoff = s == 2 ? 2800.0f : 1500.0f;
        for (int bar = 0; bar < 2; ++bar) {
            const int* ch;
            int n;
            if (s == 0) { ch = chordAm; n = 3; }
            else if (s == 1) { ch = bar == 0 ? chordAm : chordF; n = 3; }
            else { ch = bar == 0 ? chordAm7 : chordFmaj7; n = 4; }
            for (int k = 0; k < n; ++k)
                addPadNote(b, bar * kBarF, kBarF - 8000, midiHz((float)ch[k]), 1.0f, cutoff);
        }
        finish(3, s, b, 0.3f, s == 0 ? "Pad Am" : s == 1 ? "Pad Am-F" : "Pad Prog");
    }
}
