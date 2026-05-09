#pragma once
#include <cmath>
#include <cstdint>
#include "modulator.h"
#include "naive_params.h"

// 3rd-order error-feedback sigma-delta modulator.
//
// Error-feedback topology:
//   feedback = SHAPE * (A*e[0] + B*e[1] + C*e[2])
//   v        = x - feedback + dither
//   y        = sign(v)
//   q        = y - v                               (quantization error)
//   e        = {q, e[0], e[1]}                     (shift register)
//
// This is simpler and more stable than a high-order CRFB loop.  The noise
// shaping is less aggressive but the structure is intrinsically bounded.
//
// DITHER STRATEGY (v2):
//   Original: TPDF (sum of two uniform RVs) at the quantizer input.
//   TPDF amplitude was 0.002 — essentially inert.
//
//   v2: RPDF (single uniform RV) at the quantizer input.  For error-feedback
//   topology there is no separate cascade of integrators, so injecting at the
//   comparator is the natural equivalent of "inner-state" injection.  RPDF with
//   peak amplitude controlled by ORDER3_DITHER_GAIN (default 0.3) stays safely
//   within the ±1 half-step range and does not cause overload, unlike TPDF
//   which spans 2 LSBs.
//   (Lipshitz & Vanderkooy 2001 §1; Reefman & Janssen 2003 §4.2.2)
//
//   Note: time-dispersion dither (Reefman §4.2.2 [20]) would be more effective
//   still, but RPDF at the comparator is a practical improvement over TPDF and
//   far better than the previous near-zero amplitude.

class Order3Modulator : public Modulator {
public:
    void reset() override {
        for (int i = 0; i < 3; ++i) {
            errL[i] = 0.0;
            errR[i] = 0.0;
        }
    }

    void process(const float* xL, const float* xR,
                 uint16_t& outL, uint16_t& outR) override {
        float bitsL[16], bitsR[16];

        for (int i = 0; i < 16; ++i) {
            bitsL[i] = step(xL[i], errL, rngL);
            bitsR[i] = step(xR[i], errR, rngR);
        }

        outL = packBitsLSBFirst(bitsL);
        outR = packBitsLSBFirst(bitsR);
    }

    const char* name() const override { return "order3"; }

    const char* description() const override {
        return "3rd-order error-feedback sigma-delta; RPDF at quantizer input";
    }

private:
    static constexpr bool   ENABLE_SOFT_CLIP  = false;
    static constexpr double SOFT_CLIP_DRIVE   = 1.2;

    double errL[3] = {0.0, 0.0, 0.0};
    double errR[3] = {0.0, 0.0, 0.0};

    uint32_t rngL = 2463534242u;
    uint32_t rngR = 3141592653u;

    // RPDF dither: single xorshift — rectangular distribution.
    // Peak amplitude = ORDER3_DITHER_GAIN (default 0.3), which is comfortably
    // below the 1-bit half-step of ±1.0, so overload is never forced.
    static inline double rpdfDither(uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const double scale = (double)naive_params::ORDER3_DITHER_GAIN / 2147483648.0;
        return (double)(int32_t)s * scale;
    }

    static inline double softclip(double x) {
        return std::tanh(SOFT_CLIP_DRIVE * x);
    }

    static inline double clampd(double x, double lo, double hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }

    float step(float in, double e[3], uint32_t& rng) {
        double x = (double)in * naive_params::ORDER3_INPUT_SCALE;

        if (ENABLE_SOFT_CLIP) {
            x = softclip(x);
        }

        x = clampd(x, -0.95, 0.95);

        double feedback = naive_params::ORDER3_SHAPE * (
            naive_params::ORDER3_A * e[0] +
            naive_params::ORDER3_B * e[1] +
            naive_params::ORDER3_C * e[2]
        );

        // RPDF at quantizer input.  For error-feedback topology this is the
        // most natural injection point (equivalent to dithering the decision
        // boundary).  RPDF peak < 1.0 guarantees no overload.
        double v = x - feedback + rpdfDither(rng);
        double y = (v >= 0.0) ? 1.0 : -1.0;

        double q = y - v;
        q = clampd(q, -naive_params::ORDER3_ERR_CLAMP, naive_params::ORDER3_ERR_CLAMP);

        e[2] = e[1];
        e[1] = e[0];
        e[0] = q;

        return (float)y;
    }
};
