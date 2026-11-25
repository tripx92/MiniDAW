#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <oboe/Oboe.h>

#include "DSP.h"

constexpr int kTracks = 4;
constexpr int kScenes = 5;
constexpr int kSlots = kTracks * kScenes;
constexpr int kMeterChannels = 7;          // 4 tracks, Return A, Return B, Master
constexpr int kMeterValues = kMeterChannels * 4;  // peakL, peakR, rmsL, rmsR
constexpr int kStatusSize = 64;
constexpr int kMaxInChannels = 8;
constexpr int kMaxBlock = 4096;
constexpr double kRecordSeconds = 40.0;

enum TrackParam {
    TP_VOLUME = 0, TP_PAN, TP_SEND_A, TP_SEND_B, TP_ACTIVE, TP_SOLO, TP_XF_ASSIGN,
    TP_FILTER_TYPE, TP_CUTOFF, TP_RESONANCE, TP_DRIVE, TP_COUNT
};
enum GlobalParam {
    GP_MASTER_VOL = 0, GP_RET_A_VOL, GP_RET_B_VOL, GP_CROSSFADER, GP_DELAY_MODE,
    GP_CLICK_VOL, GP_METRONOME, GP_BPM, GP_QUANT, GP_COUNT
};
enum SlotState {
    SS_STOPPED = 0, SS_PLAYING, SS_QUEUED, SS_STOP_QUEUED, SS_RECORDING, SS_REC_QUEUED, SS_REC_STOP_QUEUED
};
enum CmdType {
    CMD_SLOT_PRESS = 1, CMD_LAUNCH_SCENE, CMD_STOP_TRACK, CMD_STOP_ALL, CMD_SET_CLIP,
    CMD_SET_REC_BUF, CMD_PLAY, CMD_STOP
};
enum EventType { EV_REC_DONE = 1, EV_RETIRE = 2 };

// Immutable sample payload. `data` is interleaved stereo float at 48 kHz.
struct ClipData {
    float* data = nullptr;
    int frames = 0;
    double beats = 4.0;     // natural loop length in beats
    int defaultBars = 1;
    ClipData() = default;
    ClipData(const ClipData&) = delete;
    ClipData& operator=(const ClipData&) = delete;
    ~ClipData() { delete[] data; }
};

struct Cmd {
    int type = 0, a = 0, b = 0, c = 0;
    double d = 0.0;
    void* p = nullptr;
};

// Lock-free single-producer / single-consumer ring buffer.
template <typename T, int N>
class SpscRing {
public:
    bool push(const T& v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t n = (h + 1) % N;
        if (n == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = v;
        head_.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buf_[t];
        tail_.store((t + 1) % N, std::memory_order_release);
        return true;
    }
private:
    T buf_[N];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine();
    ~AudioEngine() override;

    bool start();
    void stop();

    // ---- transport / launching (UI thread)
    void play();
    void stopTransport();
    void slotPress(int track, int scene);
    void launchScene(int scene);
    void stopTrack(int track);
    void stopAll();
    void setArm(int track, bool armed);
    void setGlobal(int id, float value);
    void setTrackParam(int track, int id, float value);
    void setClipParams(int track, int scene, int warp, int bars, int transpose);

    // Copies interleaved stereo float data into a new clip and hands it to the audio thread.
    bool loadClip(int track, int scene, const float* interleaved, int frames, const char* name);

    // ---- polling (UI thread)
    void getStatus(float* out) const;
    void getMeters(float* out) const;
    bool pollEvent(int* out);           // out[0..4] = type, track, scene, frames, bars
    std::string clipName(int track, int scene) const;
    void clipInfo(int track, int scene, int* out) const;   // has, bars, warp, transpose, frames

    // oboe::AudioStreamCallback
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                          int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    struct VoiceParams {
        bool warp = true;
        double loopBeats = 4.0;
        double pitch = 1.0;
    };
    struct Voice {
        ClipData* clip = nullptr;
        int slot = -1;
        double phase = 0.0;      // beats since launch (warp mode)
        double pos = 0.0;        // source frames (varispeed mode)
        float env = 0.0f, envTarget = 0.0f, envInc = 0.0f;
        dsp::GrainStretcher grain;
        bool started = false;
        bool active() const { return clip != nullptr; }
    };

    // --- engine internals (audio thread)
    void handleCommand(const Cmd& c);
    void slotPressAudio(int t, int s);
    void launchNow(int t, int s);
    void stopNow(int t);
    void startRecordNow(int t);
    void finishRecordNow(int t);
    void processBoundary();
    void processPendingNow();
    void startTransportFromZero();
    void stopVoice(Voice& v, bool fadeOut, int t);
    void installClip(int t, int s, ClipData* c, bool keepVoice);
    void renderVoice(Voice& v, double beatInc, const VoiceParams& vp, float& l, float& r);
    void publishStates();
    void pushEvent(const Cmd& e);
    bool anyPending() const;
    double quantBeats() const;

    // --- demo set (main thread, before start)
    void buildDemoSet();
    void setClipOwned(int t, int s, ClipData* c, const std::string& name, bool pushToAudio);

    bool openStreams();
    void closeStreams();
    void restartAsync();
    void postCmd(const Cmd& c);
    void drainGraveyard(bool force);
    void ensureRecBuf(int track);
    VoiceParams paramsFor(int slot) const;

    // streams
    std::shared_ptr<oboe::AudioStream> outStream_, inStream_;
    int inChannels_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> restarting_{false};

    // UI -> audio
    SpscRing<Cmd, 512> cmdRing_;
    std::atomic_flag cmdLock_ = ATOMIC_FLAG_INIT;   // serialises UI-side producers only
    std::atomic<float> trackParam_[kTracks][TP_COUNT];
    std::atomic<float> globalParam_[GP_COUNT];
    std::atomic<int> clipWarp_[kSlots], clipBars_[kSlots], clipTranspose_[kSlots];
    std::atomic<int> armed_[kTracks];

    // audio -> UI
    SpscRing<Cmd, 128> eventRing_;
    std::atomic<int> slotState_[kSlots];
    std::atomic<int> slotHas_[kSlots];
    std::atomic<int> trackScene_[kTracks];
    std::atomic<int> trackStopQueued_[kTracks];
    std::atomic<float> trackProgress_[kTracks];
    std::atomic<float> meters_[kMeterValues];
    std::atomic<int> playing_{0};
    std::atomic<double> beatsOut_{0.0};

    // main-thread bookkeeping
    std::string names_[kSlots];
    int slotFrames_[kSlots];
    std::vector<std::pair<ClipData*, std::chrono::steady_clock::time_point>> graveyard_;
    bool recBufSent_[kTracks];

    // ---- audio-thread state
    ClipData* clips_[kSlots];
    ClipData* recBuf_[kTracks];
    Voice cur_[kTracks], fade_[kTracks];
    int pendingScene_[kTracks];
    bool pendingStop_[kTracks];
    int recState_[kTracks];     // 0 idle, 1 waiting start, 2 recording, 3 waiting stop
    int recScene_[kTracks];
    int recFrames_[kTracks];
    double recBeats_[kTracks];
    bool playingA_ = false;
    double beats_ = 0.0;
    bool stopPressedOnce_ = false;
    bool pendingFlag_ = false;
    bool immediate_ = false;   // scene launched from a stopped transport: ignore quantisation

    // metronome
    int clickPos_ = -1;
    float clickFreq_ = 1000.0f;

    // channel strips
    dsp::Smoother vol_[kTracks], pan_[kTracks], sendA_[kTracks], sendB_[kTracks], gate_[kTracks];
    dsp::Smoother retA_, retB_, master_, xfader_, clickVol_;
    dsp::BiquadStereo filt_[kTracks];
    float lastCutoff_[kTracks], lastRes_[kTracks];
    int lastFiltType_[kTracks];
    dsp::StereoReverb reverb_;
    dsp::PingPongDelay delay_;
    dsp::Limiter limiter_;
    float meterPeak_[kMeterChannels][2];
    float inBuf_[kMaxInChannels * kMaxBlock];
};
