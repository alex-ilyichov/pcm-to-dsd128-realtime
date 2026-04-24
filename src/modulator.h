#pragma once
#include <cstdint>

// Abstract interface for all delta-sigma modulators.
// process() converts one stereo PCM sample into two 16-bit DSD words (LSB-first).
class Modulator {
public:
    virtual void reset() = 0;
    virtual void process(float inL, float inR, uint16_t& outL, uint16_t& outR) = 0;
    virtual const char* name() const = 0;
    virtual const char* description() const = 0;
    virtual ~Modulator() = default;

protected:
    // Shared clamp utility
    static inline float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Pack 16 modulator bits (LSB-first) into a uint16_t
    static inline uint16_t packBits(float samples[16]) {
        uint16_t word = 0;
        for (int i = 0; i < 16; ++i)
            word |= (samples[i] > 0.0f ? 1u : 0u) << i;
        return word;
    }
};
