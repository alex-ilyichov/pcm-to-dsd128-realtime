#pragma once
#include "modulator.h"
#include "naive_params.h"

// 7th-order CRFB noise-shaping modulator.
//
// State equations and coefficients from SoX sdm.c (Mans Rullgard, LGPL),
// "clans-7" filter at 128x44100 = 5644800 Hz.
// CLANS = Closed-Loop Analysis of Noise Shapers (Risbo 1994).
//
// 7th order improves in-band SNR relative to the 5th-order loop at the cost
// of a tighter stability margin.  Keep as the experimental/high-performance
// option until it passes the same stress tests as the 5th-order reference.
//
// DITHER STRATEGY (v8 — 4th-order HP inner-state injection):
//   Same as modulator_shaped.h but with three injection points (d[1], d[2], d[3]).
//   See modulator_shaped.h header comment for full rationale.
//
// xL[16]/xR[16]: 16 pre-FIR-interpolated samples at 5644800 Hz from caller.

class Fir7Modulator : public Modulator {
public:
    void reset() override {
        for (int i = 0; i < 7; ++i) sL[i] = sR[i] = 0.0;
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
        outL = packBitsMSBFirst(bitsL);
        outR = packBitsMSBFirst(bitsR);
    }

    const char* name()        const override { return "fir7"; }
    const char* description() const override {
        return "7th-order CLANS CRFB (SoX clans-7 @ DSD128), HP-RPDF at d[1]/d[2]/d[3]";
    }

private:
    static constexpr double INPUT_SCALE = 0.75;
    static constexpr double STATE_CLAMP = 4.0;
    static constexpr double LEAK = 1.0 - 2e-6;

    // 4th-order Butterworth HP biquad cascade (same as ShapedModulator).
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
    static constexpr double S1_B0 =  9.797274528209514e-01;
    static constexpr double S1_B1 = -1.959454905641903e+00;
    static constexpr double S1_B2 =  9.797274528209514e-01;
    static constexpr double S1_A1 = -1.959212113981752e+00;
    static constexpr double S1_A2 =  9.596976973020537e-01;
    static constexpr double S2_B0 =  9.914305680857198e-01;
    static constexpr double S2_B1 = -1.982861136171440e+00;
    static constexpr double S2_B2 =  9.914305680857198e-01;
    static constexpr double S2_A1 = -1.982615444297790e+00;
    static constexpr double S2_A2 =  9.831068280450894e-01;

    static constexpr double INNER_DITHER_D1 = 4.0;
    static constexpr double INNER_DITHER_D2 = 2.0;
    static constexpr double INNER_DITHER_D3 = 1.0;

    // SoX sdm.c "clans-7" at 128*44100 Hz
    static constexpr double a[7] = {
        8.98180853333862e-01,
        3.27985497323439e-01,
        6.38803466871112e-02,
        7.18262647412857e-03,
        4.51845004995476e-04,
        1.49685651672331e-05,
        4.22554681245302e-08,
    };
    static constexpr double g[6] = {
        0,                    9.92163123766340e-05,
        0,                    3.31199917300393e-04,
        0,                    5.42540771343282e-04,
    };

    double sL[7] = {0}, sR[7] = {0};
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

    static double step(double x, double s[7], double& prevY, uint32_t& rng,
                       BiquadHP hp[2]) {
        double y = prevY;
        double d[7];

        d[0] = s[0] - g[0] * s[1] + x - y;
        double v = x + a[0] * d[0];

        for (int i = 1; i < 6; ++i) {
            d[i] = s[i] + s[i - 1] - g[i] * s[i + 1];
            v += a[i] * d[i];
        }
        d[6] = s[6] + s[5];
        v += a[6] * d[6];

        y = (v >= 0.0) ? 1.0 : -1.0;
        prevY = y;

        // 4th-order Butterworth HP filtered RPDF at d[1]/d[2]/d[3].
        double raw = rpdfDither64(rng, naive_params::SHAPED_DITHER_GAIN);
        double filtered = hp[1].process(hp[0].process(raw));
        d[1] += filtered * INNER_DITHER_D1;
        d[2] += filtered * INNER_DITHER_D2;
        d[3] += filtered * INNER_DITHER_D3;

        for (int i = 0; i < 7; ++i)
            s[i] = clamp(d[i] * LEAK, -STATE_CLAMP, STATE_CLAMP);

        return y;
    }
};
