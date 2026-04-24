#pragma once
#include "modulator.h"

// 5th-order delta-sigma modulator — no noise shaping coefficients.
// All integrator feedback weights are 1.0. Quantization noise is flat
// across the spectrum. Included as a baseline for A/B comparison.
class NaiveModulator : public Modulator {
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

    const char* name()        const override { return "naive"; }
    const char* description() const override {
        return "5th-order, no NTF coefficients — flat quantization noise (baseline)";
    }

private:
    float stateL[5] = {0};
    float stateR[5] = {0};

    float step(float in, float s[5]) {
        s[0] = clamp(s[0] + in    - s[4], -16.0f, 16.0f);
        s[1] = clamp(s[1] + s[0]  - s[4], -16.0f, 16.0f);
        s[2] = clamp(s[2] + s[1]  - s[4], -16.0f, 16.0f);
        s[3] = clamp(s[3] + s[2]  - s[4], -16.0f, 16.0f);
        s[4] = (s[3] >= 0.0f) ? 1.0f : -1.0f;
        return s[4];
    }
};
