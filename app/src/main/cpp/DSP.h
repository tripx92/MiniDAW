#pragma once
// DSP.h — Biquad, Stereo Reverb, Tempo Ping-Pong Delay, Master Limiter, Warp/Resampler.
// All buffers are allocated in init()/constructors (main thread). process() never allocates.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr int kSampleRate = 48000;
constexpr float kDenorm = 1e-18f;

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

// Rational tanh approximation, exact +-1 for |x| >= 3.
inline float fastTanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Soft clipper with drive 0..1. Unity gain at |x| = 1 when drive > 0.
inline float softClip(float x, float drive) {
    const float g = 1.0f + 7.0f * drive;
    return fastTanh(x * g) / fastTanh(g);
}

struct Smoother {
    float cur = 0.0f, target = 0.0f, coef = 0.0015f;
    inline void snap(float v) { cur = target = v; }
    inline float next() { cur += (target - cur) * coef; return cur; }
};

// ---------------------------------------------------------------- Biquad
struct BiquadStereo {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }

    void design(bool highPass, float freq, float q, float sr = (float)kSampleRate) {
        freq = std::clamp(freq, 20.0f, sr * 0.45f);
        q = std::clamp(q, 0.3f, 18.0f);
        const float w0 = kTwoPi * freq / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float alpha = sw / (2.0f * q);
        const float a0 = 1.0f + alpha;
        if (!highPass) {
            b0 = (1.0f - cw) * 0.5f / a0;
            b1 = (1.0f - cw) / a0;
        } else {
            b0 = (1.0f + cw) * 0.5f / a0;
            b1 = -(1.0f + cw) / a0;
        }
        b2 = b0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    void setLowPass(float f, float q) { design(false, f, q); }
    void setHighPass(float f, float q) { design(true, f, q); }

    inline float processCh(int c, float x) {
        x += kDenorm;
        const float y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
    inline void process(float& l, float& r) {
        l = processCh(0, l);
        r = processCh(1, r);
    }
};

// ---------------------------------------------------------------- Reverb (Freeverb topology)
class StereoReverb {
public:
    void init(float sr = (float)kSampleRate) {
        static const int combTune[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int apTune[4] = {556, 441, 341, 225};
        const float scale = sr / 44100.0f;
        for (int i = 0; i < 8; ++i) {
            combL_[i].init((int)(combTune[i] * scale));
            combR_[i].init((int)((combTune[i] + 23) * scale));
        }
        for (int i = 0; i < 4; ++i) {
            apL_[i].init((int)(apTune[i] * scale));
            apR_[i].init((int)((apTune[i] + 23) * scale));
        }
    }
    // Input: mono send; Output: wet stereo only.
    inline void process(float in, float& outL, float& outR) {
        const float x = in * 0.015f;
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < 8; ++i) {
            l += combL_[i].process(x);
            r += combR_[i].process(x);
        }
        for (int i = 0; i < 4; ++i) {
            l = apL_[i].process(l);
            r = apR_[i].process(r);
        }
        outL = l * 3.0f;
        outR = r * 3.0f;
    }

private:
    struct Comb {
        std::vector<float> buf;
        int idx = 0;
        float store = 0.0f, fb = 0.86f, damp = 0.25f;
        void init(int n) { buf.assign((size_t)std::max(n, 8), 0.0f); idx = 0; store = 0.0f; }
        inline float process(float in) {
            const float out = buf[idx];
            store = out * (1.0f - damp) + store * damp + kDenorm;
            buf[idx] = in + store * fb;
            if (++idx >= (int)buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        int idx = 0;
        void init(int n) { buf.assign((size_t)std::max(n, 8), 0.0f); idx = 0; }
        inline float process(float in) {
            const float b = buf[idx];
            const float out = -in + b;
            buf[idx] = in + b * 0.5f + kDenorm;
            if (++idx >= (int)buf.size()) idx = 0;
            return out;
        }
    };
    Comb combL_[8], combR_[8];
    Allpass apL_[4], apR_[4];
};

// ---------------------------------------------------------------- Ping-Pong Delay
class PingPongDelay {
public:
    void init(float sr = (float)kSampleRate, float maxSeconds = 2.5f) {
        size_ = (int)(sr * maxSeconds);
        bl_.assign((size_t)size_, 0.0f);
        br_.assign((size_t)size_, 0.0f);
        w_ = 0;
        cur_ = target_ = sr * 0.25f;
    }
    // mode 0 = 1/8, 1 = 1/8 dotted, 2 = 1/4
    static float delaySamples(int mode, float bpm, float sr = (float)kSampleRate) {
        const float beats = (mode == 0) ? 0.5f : (mode == 1) ? 0.75f : 1.0f;
        return beats * 60.0f / std::max(bpm, 1.0f) * sr;
    }
    void setDelaySamples(float d) { target_ = std::clamp(d, 64.0f, (float)(size_ - 4)); }
    void setFeedback(float fb) { fb_ = std::clamp(fb, 0.0f, 0.92f); }

    inline void process(float in, float& outL, float& outR) {
        cur_ += (target_ - cur_) * 0.0004f;
        float rp = (float)w_ - cur_;
        if (rp < 0.0f) rp += (float)size_;
        const int i0 = (int)rp;
        const float fr = rp - (float)i0;
        const int i1 = (i0 + 1 >= size_) ? 0 : i0 + 1;
        const float dl = bl_[i0] + (bl_[i1] - bl_[i0]) * fr;
        const float dr = br_[i0] + (br_[i1] - br_[i0]) * fr;
        lpL_ += (dl - lpL_) * 0.55f + kDenorm;
        lpR_ += (dr - lpR_) * 0.55f + kDenorm;
        bl_[w_] = in + lpR_ * fb_;   // right echo feeds the left line
        br_[w_] = lpL_ * fb_;        // left echo feeds the right line
        if (++w_ >= size_) w_ = 0;
        outL = dl;
        outR = dr;
    }

private:
    std::vector<float> bl_, br_;
    int size_ = 1, w_ = 0;
    float cur_ = 12000.0f, target_ = 12000.0f, fb_ = 0.45f, lpL_ = 0.0f, lpR_ = 0.0f;
};

// ---------------------------------------------------------------- Master Limiter
// Lookahead brickwall limiter with soft knee above the threshold.
class Limiter {
public:
    static constexpr int N = 96;  // 2 ms lookahead at 48 kHz
    Limiter() { reset(); }
    void reset() {
        for (int i = 0; i < N; ++i) { dl_[i] = dr_[i] = 0.0f; gr_[i] = 1.0f; }
        pos_ = 0;
        gain_ = 1.0f;
    }
    inline void process(float& l, float& r) {
        const float peak = std::max(std::fabs(l), std::fabs(r));
        float req = 1.0f;
        if (peak > thr_) {
            const float t = ceil_ - thr_;
            const float target = thr_ + t * fastTanh((peak - thr_) / t);
            req = target / peak;
        }
        const float oldReq = gr_[pos_];
        const float outL = dl_[pos_], outR = dr_[pos_];
        gr_[pos_] = req;
        dl_[pos_] = l;
        dr_[pos_] = r;
        if (++pos_ >= N) pos_ = 0;
        float m = oldReq;
        for (int i = 0; i < N; ++i) m = std::min(m, gr_[i]);
        gain_ += (m - gain_) * (m < gain_ ? kAtt : kRel);
        l = std::clamp(outL * gain_, -ceil_, ceil_);
        r = std::clamp(outR * gain_, -ceil_, ceil_);
    }
    float gain() const { return gain_; }

private:
    static constexpr float kAtt = 0.0405f;    // ~ N/4 samples time constant
    static constexpr float kRel = 0.00026f;   // ~80 ms
    float dl_[N], dr_[N], gr_[N];
    int pos_ = 0;
    float gain_ = 1.0f, thr_ = 0.89f, ceil_ = 0.98f;
};

// ---------------------------------------------------------------- Resampler / Warp
// Sample data is interleaved stereo float.
inline float hermite(float ym1, float y0, float y1, float y2, float t) {
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * t + c2) * t + c1) * t + y0;
}

inline void readCubic(const float* d, int frames, double pos, float& l, float& r) {
    int i = (int)pos;
    const float t = (float)(pos - (double)i);
    if (i >= frames) i = frames - 1;
    const int im = i > 0 ? i - 1 : 0;
    const int i1 = (i + 1 < frames) ? i + 1 : frames - 1;
    const int i2 = (i + 2 < frames) ? i + 2 : frames - 1;
    l = hermite(d[2 * im], d[2 * i], d[2 * i1], d[2 * i2], t);
    r = hermite(d[2 * im + 1], d[2 * i + 1], d[2 * i1 + 1], d[2 * i2 + 1], t);
}

// Loop length is M = frames - xf. The head [0, xf) is cross-faded with the tail beyond M,
// so wrapping from M back to 0 is click-free (xf = 240 frames = 5 ms).
inline int crossfadeFrames(int frames) { return frames >= 960 ? 240 : 0; }

inline void readLoop(const float* d, int frames, int xf, double pos, float& l, float& r) {
    readCubic(d, frames, pos, l, r);
    if (xf > 0 && pos < (double)xf) {
        const float g = (float)(pos / (double)xf);
        float l2, r2;
        readCubic(d, frames, pos + (double)(frames - xf), l2, r2);
        l = l * g + l2 * (1.0f - g);
        r = r * g + r2 * (1.0f - g);
    }
}

// Granular overlap-add time stretcher (Hann, 2 grains, 50% overlap).
// The source playhead is supplied by the master clock, so warped clips stay locked
// to the bar grid for any BPM; `pitch` is the playback ratio inside each grain.
class GrainStretcher {
public:
    static constexpr int L = 2048;
    static constexpr int H = L / 2;

    static const float* window() {
        static std::vector<float> win = [] {
            std::vector<float> w(L);
            for (int i = 0; i < L; ++i) w[i] = 0.5f - 0.5f * std::cos(kTwoPi * (float)i / (float)L);
            return w;
        }();
        return win.data();
    }

    void reset(double playhead, double pitch, double M) {
        g_[0] = {true, wrap(playhead - (double)H * pitch, M), H};
        g_[1] = {true, playhead, 0};
        next_ = 0;
        hop_ = H;
    }

    inline void process(const float* d, int frames, int xf, double M, double playhead,
                        double pitch, float& outL, float& outR) {
        if (hop_ <= 0) {
            g_[next_] = {true, playhead, 0};
            next_ ^= 1;
            hop_ = H;
        }
        --hop_;
        const float* win = window();
        float sl = 0.0f, sr = 0.0f;
        for (int k = 0; k < 2; ++k) {
            Grain& g = g_[k];
            if (!g.on) continue;
            double pos = g.src + (double)g.age * pitch;
            if (pos >= M) pos = std::fmod(pos, M);
            float l, r;
            readLoop(d, frames, xf, pos, l, r);
            const float w = win[g.age];
            sl += l * w;
            sr += r * w;
            if (++g.age >= L) g.on = false;
        }
        outL = sl;
        outR = sr;
    }

private:
    struct Grain { bool on = false; double src = 0.0; int age = 0; };
    static double wrap(double p, double M) {
        p = std::fmod(p, M);
        return p < 0.0 ? p + M : p;
    }
    Grain g_[2];
    int next_ = 0, hop_ = 0;
};

}  // namespace dsp
