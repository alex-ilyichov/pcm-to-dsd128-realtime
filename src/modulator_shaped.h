#pragma once
#include "modulator.h"

// 5th-order CRFB (Cascade of Resonators with Feedback) delta-sigma modulator
// with noise shaping coefficients derived from Richard Schreier's Delta-Sigma
// Toolbox (synthesizeNTF / realizeNTF, OSR=64, order=5, opt=2, H_inf=1.5).
//
// The NTF zeros are placed inside the signal band to minimize in-band noise,
// shaping quantization noise above ~20 kHz. Suitable for DSD64/128.
//
// Reference: R. Schreier & G. Temes, "Understanding Delta-Sigma Data
// Converters", IEEE Press / Wiley, 2005. Coefficients from the open-source
// Delta-Sigma Toolbox (https://www.mathworks.com/matlabcentral/fileexchange/19).

class ShapedModulator : public Modulator {
public:
    void reset() override {
        for (int i = 0; i < 5; ++i) stateL[i] = stateR[i] = 0.0f;
    }

    void process(float inL, float inR, uint16_t& outL, uint16_t& outR) override {
        float bitsL[16], bitsR[16];
        for (int i = 0; i < 16; ++i) {
            bitsL[i] = step(inL, stateL);
            bitsR[i] = step(inR, stateR);
        }
        outL = packBits(bitsL);
        outR = packBits(bitsR);
    }

    const char* name()        const override { return "shaped"; }
    const char* description() const override {
        return "5th-order CRFB with Schreier NTF coefficients — noise shaped above 20 kHz";
    }

private:
    // CRFB loop filter coefficients for 5th-order NTF
    // OSR=64 (44.1kHz in / 5.6448MHz DSD128), order=5, opt=2, H_inf=1.5
    // a[]: feed-forward input scaling per integrator stage
    // g[]: resonator feedback (pairs of integrators, creates NTF zeros in-band)
    // b[]: direct feed-in coefficients
    // c[]: inter-stage scaling
    static constexpr float a[5] = { 0.0007f, 0.0047f, 0.0147f, 0.0439f, 0.1325f };
    static constexpr float g[2] = { 0.0028f, 0.0079f };
    static constexpr float b[5] = { 0.0007f, 0.0047f, 0.0147f, 0.0439f, 0.1325f };
    static constexpr float c[5] = { 1.0f,    1.0f,    1.0f,    1.0f,    1.0f    };

    float stateL[5] = {0};
    float stateR[5] = {0};

    float step(float in, float s[5]) {
        // Quantizer output from previous step (stored in s[4] as ±1)
        float q = s[4];

        // CRFB update (5 integrators, 2 resonator pairs)
        float u0 = b[0] * in - a[0] * q;
        float u1 = b[1] * in - a[1] * q;
        float u2 = b[2] * in - a[2] * q;
        float u3 = b[3] * in - a[3] * q;
        float u4 = b[4] * in - a[4] * q;

        float s0 = clamp(s[0] + c[0] * u0,                        -20.0f, 20.0f);
        float s1 = clamp(s[1] + c[1] * (u1 + s0) - g[0] * s[0],  -20.0f, 20.0f);
        float s2 = clamp(s[2] + c[2] * (u2 + s1),                 -20.0f, 20.0f);
        float s3 = clamp(s[3] + c[3] * (u3 + s2) - g[1] * s[2],  -20.0f, 20.0f);
        float s4_in = s[3] + c[4] * (u4 + s3);

        s[0] = s0; s[1] = s1; s[2] = s2; s[3] = s3;
        s[4] = (s4_in >= 0.0f) ? 1.0f : -1.0f;
        return s[4];
    }
};
