# pcm-to-dsd128-realtime

Real-time PCM → DSD128 converter for macOS. Takes a PCM audio stream from a DAW via BlackHole virtual audio device, converts it to DSD128 using a delta-sigma modulator, and streams it to a native DSD-capable USB DAC using DoP (DSD over PCM) framing.

## Status

Working proof of concept. Audio plays correctly at DSD128 with confirmed hardware rate. Known limitation: naive modulator produces audible high-frequency noise floor correlated with the signal — modulator quality improvements are next.

## Signal chain

```
DAW → BlackHole 2ch → [DSDHelper-v4] → iFi Zen Air DAC (DoP DSD128)
```

## How it works

1. CoreAudio HALOutputUnit captures the PCM stream from BlackHole at 44,100 Hz
2. A lock-free circular ringbuffer decouples the input and output callbacks
3. A phase accumulator resamples 44,100 Hz → 352,800 Hz (8× oversampling)
4. A 5th-order delta-sigma modulator with clamped integrators converts each sample to 16 DSD bits
5. DoP framing packs 16 DSD bits + marker byte (0x05/0xFA alternating) into 24-bit PCM words
6. The output HALOutputUnit sends the DoP stream to the DAC at 352,800 Hz / 24-bit integer

## Requirements

- macOS 13+ (arm64)
- [BlackHole 2ch](https://existential.audio/blackhole/) virtual audio driver
- USB DAC with native DSD / DoP support (tested with iFi Zen Air DAC)
- Xcode Command Line Tools

## Build

```bash
clang++ -O2 -std=c++17 -o DSDHelper-v4 src/DSDHelper-v4.cpp \
  -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
```

## Usage

1. In Audio MIDI Setup, set the DAC to **2 ch 24-bit Integer 352,8 kHz**
2. In your DAW, set the output device to **BlackHole 2ch**
3. Run the converter:
   ```bash
   ./DSDHelper-v4
   ```
4. Play audio in your DAW — the DAC LED should show DSD (cyan on iFi)
5. Press Ctrl+C to stop

The binary will print the confirmed DAC hardware rate on startup:
```
DAC rate set to 352800 Hz
DAC confirmed rate: 352800 Hz → DSD128
BlackHole format: 44100 Hz, 32-bit, 2ch
Oversample ratio: 8x
```

## Diagnostic utility

To find the exact CoreAudio device name (including any trailing spaces):
```bash
clang++ -O2 -std=c++17 -o list-devices src/list-devices.cpp \
  -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
./list-devices
```

## Sine tone test

To validate the DoP pipeline independently of the DAW/BlackHole chain:
```bash
clang++ -O2 -std=c++17 -o dsd-sine src/DSDH-rew3.cpp \
  -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
./dsd-sine
```
Plays a 1kHz sine wave at DSD128 for 10 seconds.

## Known issues / roadmap

- [ ] Modulator noise shaping coefficients — quantization noise is currently flat across the spectrum; proper NTF coefficients would push it above 20kHz
- [ ] Linear interpolation or FIR anti-imaging filter before the modulator
- [ ] Configurable input device and sample rate via command-line arguments
- [ ] Graceful sample rate detection (currently assumes 44,100 Hz input)
- [ ] macOS AudioServerPlugin wrapper for true virtual device integration (no BlackHole dependency)

## Archive

`archive/` contains earlier iterations documenting the development process — useful for understanding what failed and why (byte order bugs, missing hardware rate switching, unstable modulator without integrator clamping).

## References

- [DoP Standard v1.1](http://dsd-guide.com/dop-open-standard)
- [CoreAudio HALOutputUnit documentation](https://developer.apple.com/documentation/audiotoolbox)
- [BlackHole virtual audio driver](https://existential.audio/blackhole/)
