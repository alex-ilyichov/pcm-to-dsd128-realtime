#pragma once
#include <cmath>
#include <cstdint>
#include "modulator.h"
#include "naive_params.h"

// 5th-order delta-sigma baseline with distributed feedback (FBSDM).
//
// NOTE ON COEFFICIENTS:
//   CK0–CK3 in naive_params.h are from an unverified simulation and do not
//   correspond to a formally designed NTF (Butterworth, Chebyshev-II, or CLANS).
//   Use ShapedModulator (clans-5) or Fir7Modulator (clans-7) for measurements
//   and listening evaluations.  This modulator is retained as a simple baseline
//   for subjective A/B comparison and parameter exploration.
//
// NOTE ON TOPOLOGY:
//   Unlike the CRFB (feedforward) topology in ShapedModulator, this uses a
//   pure distributed-feedback structure: each integrator s[0..3] has an
//   individual feedback tap from the quantizer output s[4].  The quantizer
//   input is the last integrator state s[3] only (no feedforward sum), which
//   limits NTF design flexibility.
//
// DITHER STRATEGY (v2):
//   Original: RPDF added to the input *before* the input clamp.  This was
//   doubly wrong: (a) the clamp truncated the dither whenever the signal
//   approached ±1, and (b) input-domain perturbation is the least effective
//   position for limit-cycle suppression.
//
//   v2: RPDF is injected at integrator state s[1] inside the clamp, so the
//   perturbation propagates through s[2] → s[3] → quantizer on subsequent
//   steps.  Amplitude is controlled by naive_params::DITHER_GAIN (default
//   updated to 5e-2 — see naive_params.h).

class NaiveModulator : public Modulator {
public:
    void reset() override {
        for (int i = 0; i < 5; ++i) stateL[i] = stateR[i] = 0.0f;
    }

    void process(const float* xL, const float* xR,
                 uint16_t& outL, uint16_t& outR) override {
        float bitsL[16], bitsR[16];
        for (int i = 0; i < 16; ++i) {
            bitsL[i] = step(xL[i], stateL, rngL);
            bitsR[i] = step(xR[i], stateR, rngR);
        }
        outL = packBitsMSBFirst(bitsL);
        outR = packBitsMSBFirst(bitsR);
    }

    const char* name() const override { return "naive"; }
    const char* description() const override {
        return "5th-order distributed-feedback baseline; unverified CK coefficients; RPDF at s[1]";
    }

private:
    float stateL[5] = {0};
    float stateR[5] = {0};
    uint32_t rngL = 2463534242u;
    uint32_t rngR = 3141592653u;

    // RPDF for inner-state injection.  Single xorshift (not the sum of two)
    // gives a rectangular distribution.  Amplitude is scaled by DITHER_GAIN
    // so it can be tuned via environment variable NAIVE_DITHER_GAIN.
    static inline float rpdfStateNoise(uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float scale = naive_params::DITHER_GAIN / 2147483648.0f;
        return (float)(int32_t)s * scale;
    }

    static inline float softClip(float x) {
        return std::tanh(naive_params::SOFT_CLIP_DRIVE * x);
    }

    float step(float in, float s[5], uint32_t& rng) {
        float x = in * naive_params::INPUT_SCALE;

        if (naive_params::ENABLE_SOFT_CLIP) {
            x = softClip(x);
        }

        // Input headroom clamp — no dither here (v1 dithered before this clamp,
        // which caused the dither to be truncated near ±1 where it mattered most).
        x = clamp(x, -1.0f, 1.0f);

        s[0] = clamp(s[0] * naive_params::LEAK + x    - s[4] * naive_params::CK0,
                     -naive_params::STATE_CLAMP, naive_params::STATE_CLAMP);

        // RPDF injected at s[1]: perturbation propagates through s[2] → s[3]
        // → quantizer decision on subsequent steps, dispersing limit cycles
        // without requiring large amplitude.
        s[1] = clamp(s[1] * naive_params::LEAK + s[0] - s[4] * naive_params::CK1
                     + rpdfStateNoise(rng),
                     -naive_params::STATE_CLAMP, naive_params::STATE_CLAMP);

        s[2] = clamp(s[2] * naive_params::LEAK + s[1] - s[4] * naive_params::CK2,
                     -naive_params::STATE_CLAMP, naive_params::STATE_CLAMP);
        s[3] = clamp(s[3] * naive_params::LEAK + s[2] - s[4] * naive_params::CK3,
                     -naive_params::STATE_CLAMP, naive_params::STATE_CLAMP);
        s[4] = (s[3] >= 0.0f) ? 1.0f : -1.0f;
        return s[4];
    }
};
