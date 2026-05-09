# pcm-to-dsd128-realtime

Real-time PCM to DSD128 converter for macOS. Captures a PCM audio stream via BlackHole virtual audio device, upsamples through a 7-stage halfband FIR interpolator, converts to 1-bit DSD128 using high-order sigma-delta noise shaping, and streams to a native DSD-capable USB DAC using DoP (DSD over PCM) framing.

## Signal chain

```
DAW/Player --> BlackHole 2ch (44.1 kHz)
  --> DC blocker (1 Hz highpass)
  --> 7-stage halfband FIR interpolator (44100 --> 5644800 Hz, x128, >120 dB stopband)
  --> Delta-sigma modulator (1-bit, selectable topology)
  --> DoP framing (16 DSD bits + marker per 24-bit word)
  --> USB DAC at 352800 Hz / 24-bit integer (DSD128)
```

## Modulators

Five delta-sigma modulator variants, selectable at runtime:

| Name | Order | Topology | Noise floor (0-20 kHz) | Notes |
|------|-------|----------|----------------------|-------|
| **cifb7** | 7th | CIFB | ~-110 dB, flat to 20 kHz | Best in-band performance. Sequential state update, resonator zeros, H_inf=1.25. |
| **fir7** | 7th | CRFB | ~-115 dB low, rises above 5 kHz | SoX CLANS-7 coefficients. HP-filtered RPDF dither at d[1]/d[2]/d[3]. |
| **shaped** | 5th | CRFB | ~-120 dB low, rises above 3 kHz | SoX CLANS-5 coefficients. HP-filtered RPDF dither at d[1]/d[2]. |
| **order3** | 3rd | Error feedback | ~-95 dB | Simple reference. RPDF at quantizer input. |
| **naive** | 5th | Distributed feedback | ~-90 dB | Baseline. Known limit cycle at silence. |

All modulators use leaky integrators, state clamping, and MSB-first bit packing per the DoP specification.

## GUI control

DSDHelperControl provides a macOS GUI with a modulator dropdown and runtime parameter tuning. It relaunches the engine process with updated settings.

## Requirements

- macOS 13+ (arm64)
- [BlackHole 2ch](https://existential.audio/blackhole/) virtual audio driver
- USB DAC with native DSD / DoP support (tested with iFi USB DACs)
- Xcode Command Line Tools

## Build

```bash
cd src

# Main engine
clang++ -std=c++17 -O2 -o DSDHelper-v4 DSDHelper-v4.cpp \
  -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

# GUI control app
clang++ -std=c++17 -O2 -fobjc-arc -o DSDHelperControl DSDHelperControl.mm \
  -framework Cocoa -framework CoreFoundation

# Test harness (sine density + fade/limit-cycle detection)
clang++ -std=c++17 -O2 -o test_modulators test_modulators.cpp -lm

# Device listing utility
clang++ -std=c++17 -O2 -o list-devices list-devices.cpp \
  -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
```

## Usage

1. In Audio MIDI Setup, set the DAC to **2 ch 24-bit Integer 352.8 kHz**
2. In your DAW or player, set the output device to **BlackHole 2ch**
3. Run the converter:
   ```bash
   # Default devices, shaped modulator
   ./DSDHelper-v4

   # Specify devices and modulator
   ./DSDHelper-v4 "BlackHole 2ch" "iFi (by AMR) HD USB Audio " cifb7

   # Use list-devices to find exact CoreAudio device names
   ./list-devices
   ```
4. Play audio -- the DAC should indicate DSD mode
5. Press Ctrl+C to stop

Startup output:
```
Input device:  BlackHole 2ch
Output device: iFi (by AMR) HD USB Audio
Modulator:     [cifb7] 7th-order CIFB (H_inf=1.25 OSR=64), RPDF at s[2]/s[3]
DAC rate set to 352800 Hz
DAC confirmed rate: 352800 Hz -> DSD128
```

## Testing

```bash
./test_modulators
```

Runs all five modulators against a 440 Hz sine wave (density check) and a fade-to-silence test (limit-cycle detection). Expected: density ~0.50000 for all, no limit cycles detected for shaped/fir7/order3/cifb7.

## Project structure

```
src/
  DSDHelper-v4.cpp       Main streaming engine
  DSDHelperControl.mm    macOS GUI control app
  modulator.h            Abstract base class + utilities (bit packing, dither, DC block)
  modulator_shaped.h     5th-order CRFB (SoX CLANS-5)
  modulator_fir7.h       7th-order CRFB (SoX CLANS-7)
  modulator_cifb7.h      7th-order CIFB (python-deltasigma coefficients)
  modulator_order3.h     3rd-order error feedback
  modulator_naive.h      5th-order distributed feedback baseline
  interpolator.h         7-stage halfband FIR cascade (x128, Kaiser window)
  naive_params.h         Runtime-tunable parameters via environment variables
  test_modulators.cpp    Unit test harness
  list-devices.cpp       CoreAudio device enumeration utility
  DSDH-rew3.cpp          Standalone sine tone test
scripts/
  gen_halfband_coeffs.py FIR coefficient generator (Kaiser window, beta=12.27)
```

## References

- Lipshitz & Vanderkooy, "Why 1-Bit Sigma-Delta Conversion is Unsuitable for High-Quality Applications", AES Convention Paper 5395, 2001
- Reefman & Janssen, "One-bit Audio: An Overview", Philips Research, 2003
- Risbo, "Sigma-Delta Modulators -- Stability Analysis and Optimization" (CLANS method), 1994
- [DoP Open Standard v1.1](http://dsd-guide.com/dop-open-standard)
- [BlackHole virtual audio driver](https://existential.audio/blackhole/)

## License

CC BY-NC 4.0. See [LICENSE](LICENSE) for details. Commercial use requires prior written agreement.
