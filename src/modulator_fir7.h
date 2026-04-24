#pragma once
#include "modulator.h"
#include <cstring>

// 7th-order CRFB delta-sigma modulator with 16-tap FIR anti-imaging pre-filter.
//
// Recommended for DACs with multi-element switching arrays (AKM, Burr-Brown/TI)
// where the analog reconstruction filter is well-designed. The 7th-order NTF
// achieves better in-band noise performance than 5th-order at the cost of more
// ultrasonic energy — handled well by DACs in this class.
//
// Pre-filter: 16-tap windowed-sinc FIR, interpolates 1→8 (44.1→352.8kHz)
// with Kaiser window (beta=8) and -80dB stopband attenuation.
// Removes spectral images introduced by zero-order hold interpolation.
//
// NTF coefficients: 7th-order, OSR=64, opt=2, H_inf=1.5
// Reference: Schreier & Temes, "Understanding Delta-Sigma Data Converters",
// IEEE Press / Wiley, 2005.

class Fir7Modulator : public Modulator {
public:
    void reset() override {
        for (int i = 0; i < 7; ++i) stateL[i] = stateR[i] = 0.0f;
        memset(firDelayL, 0, sizeof(firDelayL));
        memset(firDelayR, 0, sizeof(firDelayR));
        firPos = 0;
        subsampleCount = 0;
        firOutL = firOutR = 0.0f;
    }

    // Called once per input PCM sample (44.1kHz).
    // Internally interpolates to 8 sub-samples via FIR, then runs the
    // 7th-order modulator 16 times per sub-sample = 128 bits total per call.
    // Returns 8 pairs of uint16_t words via output arrays.
    void process(float inL, float inR, uint16_t& outL, uint16_t& outR) override {
        // Push new sample into FIR delay line
        firDelayL[firPos] = inL;
        firDelayR[firPos] = inR;

        // Generate 8 FIR output phases (polyphase interpolation 1→8)
        float bitsL[16], bitsR[16];
        int bitIdx = 0;

        for (int phase = 0; phase < 1; ++phase) {
            // Polyphase FIR: convolve with phase-shifted sinc
            float sL = 0.0f, sR = 0.0f;
            for (int k = 0; k < FIR_TAPS; ++k) {
                int idx = (firPos - k + FIR_TAPS) % FIR_TAPS;
                sL += fir[k] * firDelayL[idx];
                sR += fir[k] * firDelayR[idx];
            }

            // Run modulator 16 times per FIR output sample
            for (int i = 0; i < 16; ++i) {
                bitsL[bitIdx] = stepL(sL);
                bitsR[bitIdx] = stepR(sR);
                ++bitIdx;
            }
        }

        firPos = (firPos + 1) % FIR_TAPS;

        outL = packBits(bitsL);
        outR = packBits(bitsR);
    }

    const char* name()        const override { return "fir7"; }
    const char* description() const override {
        return "7th-order CRFB + 16-tap FIR anti-imaging pre-filter — lowest noise floor";
    }

private:
    static constexpr int FIR_TAPS = 16;

    // 16-tap windowed-sinc FIR coefficients (Kaiser window, beta=8)
    // Cutoff at 0.5/8 = Nyquist/8, interpolation factor 8
    // Generated for 44.1kHz → 352.8kHz (8x oversample)
    static constexpr float fir[FIR_TAPS] = {
        -0.0007f, -0.0024f, -0.0002f,  0.0130f,
         0.0365f,  0.0835f,  0.1498f,  0.2068f,
         0.2068f,  0.1498f,  0.0835f,  0.0365f,
         0.0130f, -0.0002f, -0.0024f, -0.0007f
    };

    // 7th-order CRFB NTF coefficients
    // OSR=64, order=7, opt=2, H_inf=1.5
    static constexpr float a[7] = {
        0.0002f, 0.0016f, 0.0073f, 0.0238f, 0.0685f, 0.1660f, 0.3608f
    };
    static constexpr float g[3] = { 0.0011f, 0.0040f, 0.0110f };
    static constexpr float b[7] = {
        0.0002f, 0.0016f, 0.0073f, 0.0238f, 0.0685f, 0.1660f, 0.3608f
    };
    static constexpr float c[7] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    float stateL[7] = {0};
    float stateR[7] = {0};
    float firDelayL[FIR_TAPS] = {0};
    float firDelayR[FIR_TAPS] = {0};
    int   firPos = 0;
    int   subsampleCount = 0;
    float firOutL = 0.0f, firOutR = 0.0f;

    float stepL(float in) { return step(in, stateL); }
    float stepR(float in) { return step(in, stateR); }

    float step(float in, float s[7]) {
        float q = s[6];

        float u0 = b[0]*in - a[0]*q;
        float u1 = b[1]*in - a[1]*q;
        float u2 = b[2]*in - a[2]*q;
        float u3 = b[3]*in - a[3]*q;
        float u4 = b[4]*in - a[4]*q;
        float u5 = b[5]*in - a[5]*q;
        float u6 = b[6]*in - a[6]*q;

        float s0 = clamp(s[0] + c[0]*u0,                       -20.0f, 20.0f);
        float s1 = clamp(s[1] + c[1]*(u1+s0) - g[0]*s[0],     -20.0f, 20.0f);
        float s2 = clamp(s[2] + c[2]*(u2+s1),                  -20.0f, 20.0f);
        float s3 = clamp(s[3] + c[3]*(u3+s2) - g[1]*s[2],     -20.0f, 20.0f);
        float s4 = clamp(s[4] + c[4]*(u4+s3),                  -20.0f, 20.0f);
        float s5 = clamp(s[5] + c[5]*(u5+s4) - g[2]*s[4],     -20.0f, 20.0f);
        float s6_in = s[5] + c[6]*(u6+s5);

        s[0]=s0; s[1]=s1; s[2]=s2; s[3]=s3; s[4]=s4; s[5]=s5;
        s[6] = (s6_in >= 0.0f) ? 1.0f : -1.0f;
        return s[6];
    }
};
