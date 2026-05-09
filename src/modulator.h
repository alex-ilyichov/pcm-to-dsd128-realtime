#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

// Abstract interface for all delta-sigma modulators.
// process() converts 16 pre-interpolated DSD-rate samples (per channel)
// into two packed 16-bit DSD words (LSB-first).
//
// The caller (DSDHelper) is responsible for FIR interpolation; xL[16] and
// xR[16] are already at the DSD step rate (5644800 Hz).
class Modulator {
public:
    virtual void reset() = 0;
    virtual void process(const float* xL, const float* xR,
                         uint16_t& outL, uint16_t& outR) = 0;
    virtual const char* name() const = 0;
    virtual const char* description() const = 0;
    virtual ~Modulator() = default;

protected:
    static inline float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static inline double clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static inline double softClip(double x, double drive = 1.2) {
        return std::tanh(x * drive);
    }

    static inline uint16_t packBitsLSBFirst(float samples[16]) {
        uint16_t word = 0;
        for (int i = 0; i < 16; ++i)
            word |= (samples[i] > 0.0f ? 1u : 0u) << i;
        return word;
    }

    static inline uint16_t packBitsMSBFirst(float samples[16]) {
        uint16_t word = 0;
        for (int i = 0; i < 16; ++i)
            word |= (samples[i] > 0.0f ? 1u : 0u) << (15 - i);
        return word;
    }

    // RPDF dither (float): single-sample uniform noise for inner-state injection.
    //
    // WHY RPDF INSTEAD OF TPDF:
    // TPDF (sum of two uniform RVs) is optimal for *multi-bit* PCM quantizers —
    // it whitens the error and spans exactly 2 LSBs.  For a 1-bit quantizer with
    // step size 2Δ the TPDF span *guarantees* overload on every sample, which is
    // precisely the L&V (2001) "imperfectibility" result.  RPDF with peak < Δ
    // can break limit cycles and suppress non-linearity without forcing the
    // quantizer into saturation.
    // (Lipshitz & Vanderkooy AES 2001 §1; Reefman & Janssen 2003 §4.2.2)
    //
    // WHY INNER-STATE INJECTION:
    // Injecting dither directly before the quantizer comparator (amplitude-domain
    // dither) is the weakest approach for limit-cycle suppression.  Perturbing an
    // inner integrator state disrupts the periodic state trajectory that sustains
    // a limit cycle, requiring a much smaller amplitude to achieve the same effect.
    // (Reefman & Janssen 2003 §4.3; see also limit-cycle analysis in §4.3.1)
    //
    // Independent per-channel RNG prevents correlated dither between L and R,
    // which would cause synchronous attractor transitions manifesting as slow
    // channel-balance drift.
    static inline float rpdfDither(uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        // Peak amplitude ≈ ±2.5e-4 — for float inner-state injection.
        static constexpr float SCALE = 2.5e-4f / 2147483648.0f;
        return (float)(int32_t)s * SCALE;
    }

    // RPDF dither (double): calibrated for inner-integrator injection in CRFB loops.
    // Peak amplitude ≈ ±0.05 against STATE_CLAMP = 4.0 (≈1.25% of clamp range).
    // At DSD128 oversampling ratios the NTF pushes this residual noise well above
    // 20 kHz; in-band SNR impact is negligible while limit-cycle suppression is
    // robust.
    static inline double rpdfDither64(uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        static constexpr double SCALE = 0.05 / 2147483648.0;
        return (double)(int32_t)s * SCALE;
    }

    // Gain-parameterised variant — used by shaped/fir7 modulators so that
    // SHAPED_DITHER_GAIN can be tuned at runtime via environment variable.
    static inline double rpdfDither64(uint32_t& s, double gain) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (double)(int32_t)s * (gain / 2147483648.0);
    }

    struct DcBlock {
        double x1 = 0.0;
        double y1 = 0.0;

        inline void reset() {
            x1 = 0.0;
            y1 = 0.0;
        }

        inline double process(double x) {
            // Very low-cut one-pole DC blocker. At DSD128 this removes bias and
            // slow drift while keeping the audio band effectively untouched.
            static constexpr double R = 0.999995;
            const double y = x - x1 + R * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };
};
