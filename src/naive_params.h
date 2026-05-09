#pragma once
#include <cstdlib>
#include <cstring>

// Tunable parameters for NaiveModulator and Order3Modulator.
//
// Values can be overridden at runtime via environment variables (see
// loadFromEnvironment() below) so the control app can relaunch DSDHelper-v4
// with different values without recompiling.
//
// NOTE ON DITHER GAIN DEFAULTS (v2 changes):
//   v1 used TPDF with gain 1e-3, giving a peak amplitude of ~0.002 — so small
//   as to be essentially inert at any signal level.  v2 switches to RPDF
//   (single-sample uniform noise) and sets defaults that are actually effective:
//
//   DITHER_GAIN (NaiveModulator, injected at inner state s[1]):
//     5e-2  →  peak amplitude ±0.05  (≈0.2% of STATE_CLAMP = 26.7)
//
//   ORDER3_DITHER_GAIN (Order3Modulator, injected at quantizer comparator):
//     0.3   →  peak amplitude ±0.30  (<< 1-bit half-step ±1.0, no overload)
//
// These values are starting points.  Raise DITHER_GAIN if periodic artifacts
// (limit cycles) are audible at low levels.  Lower ORDER3_DITHER_GAIN if noise
// floor rises unacceptably (SNR budget is generous at DSD128 oversampling).

namespace naive_params {

// Overall input trim before entering the naive loop.
inline float INPUT_SCALE = 0.9f;

// RPDF dither gain for NaiveModulator, injected at inner state s[1].
// Previous default was 1e-3f (essentially inert).  5e-2f is the minimum
// that reliably disperses limit cycles in practice.
inline float DITHER_GAIN = 5.0e-2f;

// State leak applied each step.  Values slightly below 1.0 help the loop
// forget old error and reduce sticky low-level behaviour.
inline float LEAK = 0.9994f;

// Safety clamp on internal integrator states.
// Does NOT limit output level; only limits internal accumulator excursion.
inline float STATE_CLAMP = 26.7f;

// Optional soft clipping before the loop (for hot transients).
inline bool  ENABLE_SOFT_CLIP = false;
inline float SOFT_CLIP_DRIVE = 1.2f;

// Distributed-feedback coefficients for NaiveModulator.
// WARNING: these were derived from an unverified simulation and do not
// correspond to a formally designed NTF.  Use ShapedModulator or Fir7Modulator
// for any serious measurement or listening evaluation.
inline float CK0 = 1.773647f;
inline float CK1 = 0.898778f;
inline float CK2 = 1.129467f;
inline float CK3 = 1.563315f;

// ===============================
// Order3Modulator parameters
// ===============================

inline float ORDER3_INPUT_SCALE = 0.9f;

// Main shaping strength.  Try: 0.3 → 0.8.  1.0 is mathematically stronger
// but may become unstable under large inputs.
inline float ORDER3_SHAPE = 0.55f;

// RPDF dither gain for Order3Modulator, injected at the quantizer comparator.
// Peak amplitude = ORDER3_DITHER_GAIN.  Must stay below 1.0 (the 1-bit
// half-step) to avoid overload.  0.3 is a practical starting point.
// Previous default was 1e-3f (essentially inert).
inline float ORDER3_DITHER_GAIN = 3.0e-1f;

// Error history clamp.
inline float ORDER3_ERR_CLAMP = 26.7f;

// Error-feedback coefficients (ABC).  These were optimised by simulation
// for a 3rd-order noise shaper at DSD128.
inline float ORDER3_A =  1.395458f;
inline float ORDER3_B =  0.223776f;
inline float ORDER3_C = -0.518374f;

// ===============================
// Shaped / Fir7 modulator parameters
// ===============================

// Base RPDF dither gain for ShapedModulator and Fir7Modulator.
// Used at two injection points:
//   1. Quantizer input v: gain = SHAPED_DITHER_GAIN (baseline linearisation)
//   2. Inner states d[1..3]: gain = SHAPED_DITHER_GAIN × multiplier (4×/2×/1×)
//      for efficient limit-cycle suppression (Reefman 2003 §4.3)
// Tune via SHAPED_DITHER_GAIN env var.  Range: 2e-2 .. 1.5e-1.
inline float SHAPED_DITHER_GAIN = 5.0e-2f;

// ===============================
// Runtime environment loading
// ===============================

inline float parseFloatEnv(const char* name, float currentValue) {
    const char* s = std::getenv(name);
    if (!s || !*s) return currentValue;
    char* end = nullptr;
    float v = std::strtof(s, &end);
    return (end != s) ? v : currentValue;
}

inline bool parseBoolEnv(const char* name, bool currentValue) {
    const char* s = std::getenv(name);
    if (!s || !*s) return currentValue;
    if (std::strcmp(s, "1") == 0 || std::strcmp(s, "true") == 0 || std::strcmp(s, "TRUE") == 0 ||
        std::strcmp(s, "yes") == 0 || std::strcmp(s, "YES") == 0 || std::strcmp(s, "on") == 0 ||
        std::strcmp(s, "ON") == 0) {
        return true;
    }
    if (std::strcmp(s, "0") == 0 || std::strcmp(s, "false") == 0 || std::strcmp(s, "FALSE") == 0 ||
        std::strcmp(s, "no") == 0 || std::strcmp(s, "NO") == 0 || std::strcmp(s, "off") == 0 ||
        std::strcmp(s, "OFF") == 0) {
        return false;
    }
    return currentValue;
}

inline void loadFromEnvironment() {
    INPUT_SCALE      = parseFloatEnv("NAIVE_INPUT_SCALE",      INPUT_SCALE);
    DITHER_GAIN      = parseFloatEnv("NAIVE_DITHER_GAIN",      DITHER_GAIN);
    LEAK             = parseFloatEnv("NAIVE_LEAK",             LEAK);
    STATE_CLAMP      = parseFloatEnv("NAIVE_STATE_CLAMP",      STATE_CLAMP);
    ENABLE_SOFT_CLIP = parseBoolEnv ("NAIVE_ENABLE_SOFT_CLIP", ENABLE_SOFT_CLIP);
    SOFT_CLIP_DRIVE  = parseFloatEnv("NAIVE_SOFT_CLIP_DRIVE",  SOFT_CLIP_DRIVE);

    ORDER3_INPUT_SCALE = parseFloatEnv("ORDER3_INPUT_SCALE",   ORDER3_INPUT_SCALE);
    ORDER3_SHAPE       = parseFloatEnv("ORDER3_SHAPE",         ORDER3_SHAPE);
    ORDER3_DITHER_GAIN = parseFloatEnv("ORDER3_DITHER_GAIN",   ORDER3_DITHER_GAIN);
    ORDER3_ERR_CLAMP   = parseFloatEnv("ORDER3_ERR_CLAMP",     ORDER3_ERR_CLAMP);

    SHAPED_DITHER_GAIN = parseFloatEnv("SHAPED_DITHER_GAIN",   SHAPED_DITHER_GAIN);
}

} // namespace naive_params
