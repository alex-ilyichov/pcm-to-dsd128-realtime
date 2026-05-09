#include <cstdio>
#include <cmath>
#include <cstdint>
#include "modulator.h"
#include "modulator_naive.h"
#include "modulator_shaped.h"
#include "modulator_fir7.h"
#include "modulator_order3.h"
#include "modulator_cifb7.h"
#include "naive_params.h"

// Run the modulator against a 440 Hz sine, print density and bit patterns.
//
// Each call to process() expects 16 samples at DSD128 rate (5644800 Hz) and
// returns a packed 16-bit DSD word.  The original test_modulators.cpp passed
// a scalar float where const float* was required — a pre-existing compile
// error fixed here.
static void test(Modulator* m, const char* label, int periods = 50) {
    m->reset();

    static constexpr double DSD_RATE = 5644800.0;  // DSD128 sample rate
    static constexpr double FREQ     = 440.0;
    static constexpr double AMPL     = 0.9;

    long ones = 0, total = 0;
    long total_samples = (long)(DSD_RATE / FREQ * periods);
    long total_frames  = total_samples / 16;  // one call per 16 DSD samples

    printf("\n=== %s ===\n%s\n", label, m->description());

    int printed = 0;
    long sample_idx = 0;

    for (long f = 0; f < total_frames; ++f) {
        float xL[16], xR[16];
        for (int b = 0; b < 16; ++b) {
            float s = (float)(AMPL * sin(2.0 * M_PI * FREQ / DSD_RATE * (double)sample_idx));
            xL[b] = xR[b] = s;
            ++sample_idx;
        }

        uint16_t outL = 0, outR = 0;
        m->process(xL, xR, outL, outR);

        for (int b = 0; b < 16; ++b) {
            if (outL & (1u << b)) ones++;
        }
        total += 16;

        if (printed < 32) {
            printf("frame %4ld  in=%+.5f  outL=0x%04X  outR=0x%04X\n",
                   f, xL[0], outL, outR);
            ++printed;
        }
    }

    double density = (double)ones / (double)total;
    printf("... density over %ld frames (%ld DSD bits): %.5f  (ideal 0.50000 for 440 Hz)\n",
           total_frames, total, density);
}

// Fade test: sine at AMPL decays exponentially to ~0 over `fade_frames` frames,
// then runs `tail_frames` frames of silence.
//
// Prints the last 32 near-silence frames, then scans the full silence tail for
// periodic attractors (limit cycles) of period 1..MAX_PERIOD.  A cycle of
// period P is confirmed if outL[f] == outL[f-P] holds for MIN_CONFIRM
// consecutive frames — ruling out accidental coincidences.
static void test_fade(Modulator* m, const char* label,
                      int fade_frames = 500, int tail_frames = 300) {
    m->reset();

    static constexpr double DSD_RATE   = 5644800.0;
    static constexpr double FREQ       = 440.0;
    static constexpr double AMPL       = 0.9;
    static constexpr int    MAX_PERIOD = 32;
    static constexpr int    MIN_CONFIRM = 8;  // consecutive matches to declare a cycle

    printf("\n=== %s  [FADE TEST] ===\n%s\n", label, m->description());

    int total_frames = fade_frames + tail_frames;
    auto* histL = new uint16_t[total_frames]();
    long sample_idx = 0;

    for (int f = 0; f < total_frames; ++f) {
        double env = (f < fade_frames)
            ? AMPL * std::exp(-4.0 * (double)f / (double)fade_frames)
            : 0.0;

        float xL[16], xR[16];
        for (int b = 0; b < 16; ++b) {
            float s = (float)(env * sin(2.0 * M_PI * FREQ / DSD_RATE * (double)sample_idx));
            xL[b] = xR[b] = s;
            ++sample_idx;
        }

        uint16_t outL = 0, outR = 0;
        m->process(xL, xR, outL, outR);
        histL[f] = outL;

        if (f >= total_frames - 32) {
            printf("frame %4d  env=%.4f  outL=0x%04X  outR=0x%04X\n",
                   f, env, outL, outR);
        }
    }

    // Scan silence tail for cycles of period 1..MAX_PERIOD.
    int found_period = 0;
    int found_at     = -1;
    for (int period = 1; period <= MAX_PERIOD && found_period == 0; ++period) {
        for (int f = fade_frames + period; f <= total_frames - MIN_CONFIRM; ++f) {
            int run = 0;
            while (f + run < total_frames && histL[f + run] == histL[f + run - period])
                ++run;
            if (run >= MIN_CONFIRM) {
                found_period = period;
                found_at     = f;
                break;
            }
        }
    }

    if (found_period == 0) {
        printf("Limit-cycle scan (period 1..%d, %d-frame confirm): NONE — good\n",
               MAX_PERIOD, MIN_CONFIRM);
    } else {
        printf("Limit-cycle scan: *** CYCLE DETECTED  period=%d  first seen at frame %d ***\n",
               found_period, found_at);
        printf("  pattern: ");
        for (int i = 0; i < found_period; ++i)
            printf("0x%04X ", histL[found_at + i]);
        printf("\n");
    }

    delete[] histL;
}

int main() {
    naive_params::loadFromEnvironment();

    NaiveModulator  naive;
    ShapedModulator shaped;
    Fir7Modulator   fir7;
    Order3Modulator order3;
    Cifb7Modulator  cifb7;

    test(&naive,  "naive");
    test(&shaped, "shaped");
    test(&fir7,   "fir7");
    test(&order3, "order3");
    test(&cifb7,  "cifb7");

    printf("\n\n--- Fade / limit-cycle tests ---\n");
    printf("SHAPED_DITHER_GAIN = %.2e\n", (double)naive_params::SHAPED_DITHER_GAIN);

    test_fade(&naive,  "naive");
    test_fade(&shaped, "shaped");
    test_fade(&fir7,   "fir7");
    test_fade(&order3, "order3");
    test_fade(&cifb7,  "cifb7");

    return 0;
}
