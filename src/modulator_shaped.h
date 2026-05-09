#pragma once
#include "modulator.h"
#include "naive_params.h"

// 5th-order CRFB noise-shaping modulator.
//
// State equations and coefficients from SoX sdm.c (Mans Rullgard, LGPL),
// "clans-5" filter at 128x44100 = 5644800 Hz.
// CLANS = Closed-Loop Analysis of Noise Shapers (Risbo 1994).
//
// SoX sdm_filter_calc state equations:
//   d[0] = s[0] - g[0]*s[1] + x - y        (x=input, y=quantizer +/-1)
//   d[i] = s[i] + s[i-1] - g[i]*s[i+1]     (i = 1..order-2)
//   d[N] = s[N] + s[N-1]
//   v    = x + sum(a[i]*d[i])               (quantizer input)
//   y    = sign(v)
//   s[i] = d[i]                             (d[] becomes new state)
//
// DITHER STRATEGY (v8 — 4th-order HP inner-state injection):
//   4th-order Butterworth HP filtered (fc=20 kHz, 24 dB/oct) RPDF dither is
//   injected at inner integrator states d[1] and d[2] AFTER the quantizer
//   decision, BEFORE the leak/clamp state writeback.
//
//   The steep HP filter is critical:
//   - d[1] reaches v via a[1]≈0.50 — nearly unattenuated.  A 1-pole HP
//     (6 dB/oct) left audible dither in the audio band.
//   - 4th-order Butterworth gives -104 dB at 1 kHz, -184 dB at 100 Hz —
//     dither contribution is invisible below 20 kHz.
//   - Ultrasonic dither energy hides under the NTF noise wall.
//   - Trajectory disruption for limit-cycle suppression does not depend on
//     dither frequency content (Reefman 2003 §4.3).
//
//   Key: dither must NOT be added to x or v — both pass through unattenuated.
//
// xL[16]/xR[16]: 16 pre-FIR-interpolated samples at 5644800 Hz from caller.

class ShapedModulator : public Modulator {
public:
    void reset() override {
        for (int i = 0; i < 5; ++i) sL[i] = sR[i] = 0.0;
        prevYL = prevYR = -1.0;
        hpL[0].reset(); hpL[1].reset();
        hpR[0].reset(); hpR[1].reset();
    }

    void process(const float* xL, const float* xR,
                 uint16_t& outL, uint16_t& outR) override {
        float bitsL[16], bitsR[16];
        for (int i = 0; i < 16; ++i) {
            double inL = clamp((double)xL[i] * INPUT_SCALE, -1.0, 1.0);
            double inR = clamp((double)xR[i] * INPUT_SCALE, -1.0, 1.0);
            bitsL[i] = (float)step(inL, sL, prevYL, rngL, hpL);
            bitsR[i] = (float)step(inR, sR, prevYR, rngR, hpR);
        }
        outL = packBitsLSBFirst(bitsL);
        outR = packBitsLSBFirst(bitsR);
    }

    const char* name()        const override { return "shaped"; }
    const char* description() const override {
        return "5th-order CLANS CRFB (SoX clans-5 @ DSD128), HP-RPDF at d[1]/d[2]";
    }

private:
    static constexpr double INPUT_SCALE = 1.0;
    static constexpr double STATE_CLAMP = 4.0;
    static constexpr double LEAK = 1.0 - 2e-6;

    // 4th-order Butterworth HP filter for dither: fc = 20 kHz at 5644800 Hz.
    // Two cascaded biquad sections.  24 dB/octave rolloff gives:
    //   -104 dB at 1 kHz, -184 dB at 100 Hz — dither invisible in audio band.
    struct BiquadHP {
        double b0, b1, b2, a1, a2;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double process(double in) {
            double out = b0*in + b1*x1 + b2*x2 - a1*y1 - a2*y2;
            x2 = x1; x1 = in; y2 = y1; y1 = out;
            return out;
        }
        void reset() { x1 = x2 = y1 = y2 = 0; }
    };
    // Section 1 (Q=0.5412)
    static constexpr double S1_B0 =  9.797274528209514e-01;
    static constexpr double S1_B1 = -1.959454905641903e+00;
    static constexpr double S1_B2 =  9.797274528209514e-01;
    static constexpr double S1_A1 = -1.959212113981752e+00;
    static constexpr double S1_A2 =  9.596976973020537e-01;
    // Section 2 (Q=1.3066)
    static constexpr double S2_B0 =  9.914305680857198e-01;
    static constexpr double S2_B1 = -1.982861136171440e+00;
    static constexpr double S2_B2 =  9.914305680857198e-01;
    static constexpr double S2_A1 = -1.982615444297790e+00;
    static constexpr double S2_A2 =  9.831068280450894e-01;

    // Inner-state dither multipliers relative to SHAPED_DITHER_GAIN.
    static constexpr double INNER_DITHER_D1 = 4.0;
    static constexpr double INNER_DITHER_D2 = 2.0;

    // SoX sdm.c "clans-5" coefficients at 128*44100 Hz
    static constexpr double a[5] = {
        1.12849522129362e+00,
        5.02128177800632e-01,
        1.10084368682902e-01,
        1.18635667860902e-02,
        4.71059243536326e-04,
    };
    static constexpr double g[4] = {
        0,                    1.74653153894942e-04,
        0,                    4.94580504383930e-04,
    };

    double sL[5] = {0}, sR[5] = {0};
    double prevYL = -1.0, prevYR = -1.0;
    BiquadHP hpL[2] = {
        {S1_B0,S1_B1,S1_B2,S1_A1,S1_A2, 0,0,0,0},
        {S2_B0,S2_B1,S2_B2,S2_A1,S2_A2, 0,0,0,0}
    };
    BiquadHP hpR[2] = {
        {S1_B0,S1_B1,S1_B2,S1_A1,S1_A2, 0,0,0,0},
        {S2_B0,S2_B1,S2_B2,S2_A1,S2_A2, 0,0,0,0}
    };
    uint32_t rngL = 2463534242u;
    uint32_t rngR = 3141592653u;

    static double step(double x, double s[5], double& prevY, uint32_t& rng,
                       BiquadHP hp[2]) {
        double y = prevY;
        double d[5];

        d[0] = s[0] - g[0] * s[1] + x - y;
        double v = x + a[0] * d[0];

        for (int i = 1; i < 4; ++i) {
            d[i] = s[i] + s[i - 1] - g[i] * s[i + 1];
            v += a[i] * d[i];
        }
        d[4] = s[4] + s[3];
        v += a[4] * d[4];

        y = (v >= 0.0) ? 1.0 : -1.0;
        prevY = y;

        // 4th-order Butterworth HP filtered RPDF at inner states d[1]/d[2].
        // 24 dB/oct rolloff: -104 dB at 1 kHz, -184 dB at 100 Hz.
        double raw = rpdfDither64(rng, naive_params::SHAPED_DITHER_GAIN);
        double filtered = hp[1].process(hp[0].process(raw));
        d[1] += filtered * INNER_DITHER_D1;
        d[2] += filtered * INNER_DITHER_D2;

        for (int i = 0; i < 5; ++i)
            s[i] = clamp(d[i] * LEAK, -STATE_CLAMP, STATE_CLAMP);

        return y;
    }
};
