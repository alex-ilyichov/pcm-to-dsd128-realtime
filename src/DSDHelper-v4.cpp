#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include "modulator.h"
#include "modulator_naive.h"
#include "modulator_shaped.h"
#include "modulator_fir7.h"

// --- Constants ---
#define OUTPUT_SAMPLE_RATE  352800.0   // DSD128 DoP carrier
#define TARGET_DSD_RATE     5644800.0  // DSD128 bit rate
#define DSD_BITS_PER_FRAME  16         // DSD bits packed per DoP frame

// Defaults — override via command-line arguments
const char* DEFAULT_INPUT_DEVICE  = "BlackHole 2ch";
const char* DEFAULT_OUTPUT_DEVICE = "iFi (by AMR) HD USB Audio ";

// --- Lock-free circular ringbuffer ---
// Power-of-2 size so index wrapping is a bitmask (no modulo, no locks needed)
static const int RING_SIZE = 65536; // must be power of 2
static const int RING_MASK = RING_SIZE - 1;

struct RingBuffer {
    float data[RING_SIZE];
    std::atomic<int> writePos{0};
    std::atomic<int> readPos{0};

    int available() const {
        return (writePos.load(std::memory_order_acquire) -
                readPos.load(std::memory_order_acquire)) & RING_MASK;
    }

    void push(float v) {
        int w = writePos.load(std::memory_order_relaxed);
        data[w & RING_MASK] = v;
        writePos.store((w + 1) & RING_MASK, std::memory_order_release);
    }

    float pop() {
        int r = readPos.load(std::memory_order_relaxed);
        float v = data[r & RING_MASK];
        readPos.store((r + 1) & RING_MASK, std::memory_order_release);
        return v;
    }
};

static RingBuffer ringL;
static RingBuffer ringR;


// --- DoP frame packer (marker in MSB per DoP spec) ---
void pack_dop(uint16_t dsd16, uint8_t* out, bool marker) {
    out[0] = dsd16 & 0xFF;
    out[1] = (dsd16 >> 8) & 0xFF;
    out[2] = marker ? 0xFA : 0x05;  // MSB = marker byte
}

// --- Device lookup ---
AudioDeviceID getDeviceByName(const char* name) {
    UInt32 size = 0;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size);
    UInt32 count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> ids(count);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data());

    for (UInt32 i = 0; i < count; ++i) {
        CFStringRef nameRef;
        size = sizeof(nameRef);
        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(ids[i], &nameAddr, 0, nullptr, &size, &nameRef) == noErr) {
            char buf[256];
            CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(nameRef);
            if (strcmp(buf, name) == 0) return ids[i];
        }
    }
    return kAudioDeviceUnknown;
}

// --- Global state ---
static AudioUnit  inputUnit;
static AudioUnit  outputUnit;
static Modulator* modulator     = nullptr;
static bool       marker_toggle = false;
static double     inputSampleRate = 44100.0;

// --- Input callback: BlackHole PCM → ringbuffer ---
OSStatus InputCallback(void*,
                       AudioUnitRenderActionFlags* ioActionFlags,
                       const AudioTimeStamp* inTimeStamp,
                       UInt32 inBusNumber,
                       UInt32 inNumberFrames,
                       AudioBufferList*)
{
    // Allocate on stack for zero-alloc in audio thread
    const int MAX_FRAMES = 4096;
    float interleavedBuf[MAX_FRAMES * 2];

    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 2;
    abl.mBuffers[0].mDataByteSize   = inNumberFrames * sizeof(float) * 2;
    abl.mBuffers[0].mData           = interleavedBuf;

    AudioUnitRender(inputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &abl);

    float* samples = (float*)abl.mBuffers[0].mData;
    for (UInt32 f = 0; f < inNumberFrames; ++f) {
        ringL.push(samples[f * 2]);
        ringR.push(samples[f * 2 + 1]);
    }
    return noErr;
}

// --- Output callback: ringbuffer → DSD modulator → DoP frames ---
OSStatus OutputCallback(void*,
                        AudioUnitRenderActionFlags*,
                        const AudioTimeStamp*,
                        UInt32,
                        UInt32 inFrames,
                        AudioBufferList* ioData)
{
    uint8_t* buffer = (uint8_t*)ioData->mBuffers[0].mData;

    static float  last_L = 0.0f, last_R = 0.0f;
    static double inputPhase = 0.0;
    const  double phaseInc = inputSampleRate / OUTPUT_SAMPLE_RATE;

    for (UInt32 frame = 0; frame < inFrames; ++frame) {
        inputPhase += phaseInc;
        if (inputPhase >= 1.0) {
            inputPhase -= 1.0;
            if (ringL.available() >= 1 && ringR.available() >= 1) {
                last_L = ringL.pop();
                last_R = ringR.pop();
            }
        }

        uint16_t dsd_L = 0, dsd_R = 0;
        modulator->process(last_L, last_R, dsd_L, dsd_R);

        uint8_t dop_L[3], dop_R[3];
        pack_dop(dsd_L, dop_L, marker_toggle);
        pack_dop(dsd_R, dop_R, marker_toggle);
        marker_toggle = !marker_toggle;

        size_t idx = frame * 6;
        buffer[idx + 0] = dop_L[0];
        buffer[idx + 1] = dop_L[1];
        buffer[idx + 2] = dop_L[2];
        buffer[idx + 3] = dop_R[0];
        buffer[idx + 4] = dop_R[1];
        buffer[idx + 5] = dop_R[2];
    }
    return noErr;
}

int main(int argc, char* argv[]) {
    const char* inputDeviceName  = DEFAULT_INPUT_DEVICE;
    const char* outputDeviceName = DEFAULT_OUTPUT_DEVICE;
    const char* modulatorName    = "shaped"; // default

    if (argc >= 2) inputDeviceName  = argv[1];
    if (argc >= 3) outputDeviceName = argv[2];
    if (argc >= 4) modulatorName    = argv[3];

    // Select modulator
    NaiveModulator  naiveMod;
    ShapedModulator shapedMod;
    Fir7Modulator   fir7Mod;

    if      (strcmp(modulatorName, "naive")  == 0) modulator = &naiveMod;
    else if (strcmp(modulatorName, "shaped") == 0) modulator = &shapedMod;
    else if (strcmp(modulatorName, "fir7")   == 0) modulator = &fir7Mod;
    else {
        std::cerr << "Unknown modulator: " << modulatorName << std::endl;
        std::cerr << "Available: naive, shaped, fir7" << std::endl;
        return 1;
    }
    modulator->reset();

    std::cout << "Input device:  " << inputDeviceName  << std::endl;
    std::cout << "Output device: " << outputDeviceName << std::endl;
    std::cout << "Modulator:     [" << modulator->name() << "] "
              << modulator->description() << std::endl;

    // --- Find devices ---
    AudioDeviceID inputDevice = getDeviceByName(inputDeviceName);
    if (inputDevice == kAudioDeviceUnknown) {
        std::cerr << "Input device not found: " << inputDeviceName << std::endl;
        std::cerr << "Run list-devices to see available device names." << std::endl;
        return 1;
    }

    AudioDeviceID dacDevice = getDeviceByName(outputDeviceName);
    if (dacDevice == kAudioDeviceUnknown) {
        std::cerr << "Output device not found: " << outputDeviceName << std::endl;
        std::cerr << "Run list-devices to see available device names." << std::endl;
        return 1;
    }

    // --- Set DAC hardware rate to 352.8kHz ---
    Float64 targetRate = OUTPUT_SAMPLE_RATE;
    AudioObjectPropertyAddress rateAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus rateErr = AudioObjectSetPropertyData(dacDevice, &rateAddr, 0, nullptr, sizeof(Float64), &targetRate);
    if (rateErr != noErr) {
        std::cerr << "Warning: could not set DAC rate: " << rateErr << std::endl;
    } else {
        std::cout << "DAC rate set to " << targetRate << " Hz" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Readback: confirm what rate the DAC actually accepted
    Float64 confirmedRate = 0.0;
    UInt32 rateSize = sizeof(Float64);
    AudioObjectGetPropertyData(dacDevice, &rateAddr, 0, nullptr, &rateSize, &confirmedRate);
    std::cout << "DAC confirmed rate: " << confirmedRate << " Hz → "
              << (confirmedRate >= 352799.0 ? "DSD128" :
                  confirmedRate >= 176399.0 ? "DSD64"  : "PCM (not DSD)")
              << std::endl;

    // --- Set up input unit (BlackHole) ---
    AudioComponentDescription desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple
    };
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioComponentInstanceNew(comp, &inputUnit);

    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,  1, &one,  sizeof(one));
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));

    // Get BlackHole's native format
    AudioStreamBasicDescription asbd_in = {};
    UInt32 size = sizeof(asbd_in);
    AudioUnitGetProperty(inputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &asbd_in, &size);
    inputSampleRate = asbd_in.mSampleRate;
    std::cout << "BlackHole format: " << inputSampleRate << " Hz, "
              << asbd_in.mBitsPerChannel << "-bit, "
              << asbd_in.mChannelsPerFrame << "ch" << std::endl;
    std::cout << "Oversample ratio: " << (OUTPUT_SAMPLE_RATE / inputSampleRate) << "x" << std::endl;

    // Request interleaved float stereo from BlackHole
    AudioStreamBasicDescription asbd_float = {};
    asbd_float.mSampleRate       = asbd_in.mSampleRate;
    asbd_float.mFormatID         = kAudioFormatLinearPCM;
    asbd_float.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd_float.mChannelsPerFrame = 2;
    asbd_float.mFramesPerPacket  = 1;
    asbd_float.mBitsPerChannel   = 32;
    asbd_float.mBytesPerFrame    = 8;
    asbd_float.mBytesPerPacket   = 8;
    AudioUnitSetProperty(inputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &asbd_float, sizeof(asbd_float));

    AURenderCallbackStruct cb_in = {InputCallback, nullptr};
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb_in, sizeof(cb_in));
    AudioUnitInitialize(inputUnit);
    AudioOutputUnitStart(inputUnit);
    std::cout << "BlackHole input started." << std::endl;

    // --- Set up output unit (iFi DAC) ---
    AudioComponentInstanceNew(comp, &outputUnit);
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one,  sizeof(one));
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,  1, &zero, sizeof(zero));
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dacDevice, sizeof(dacDevice));

    // 24-bit integer interleaved stereo @ 352.8kHz for DoP
    AudioStreamBasicDescription asbd_out = {};
    asbd_out.mSampleRate       = OUTPUT_SAMPLE_RATE;
    asbd_out.mFormatID         = kAudioFormatLinearPCM;
    asbd_out.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd_out.mChannelsPerFrame = 2;
    asbd_out.mFramesPerPacket  = 1;
    asbd_out.mBitsPerChannel   = 24;
    asbd_out.mBytesPerFrame    = 6;
    asbd_out.mBytesPerPacket   = 6;
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd_out, sizeof(asbd_out));

    AURenderCallbackStruct cb_out = {OutputCallback, nullptr};
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb_out, sizeof(cb_out));
    AudioUnitInitialize(outputUnit);
    AudioOutputUnitStart(outputUnit);
    std::cout << "iFi DAC output started at " << OUTPUT_SAMPLE_RATE << " Hz (DoP DSD128)." << std::endl;
    std::cout << "Route your DAW to \"" << inputDeviceName << "\". Press Ctrl+C to stop." << std::endl;
    std::cout << "Usage: DSDHelper-v4 [input-device] [output-device] [naive|shaped|fir7]" << std::endl;

    // Run until killed
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    AudioOutputUnitStop(inputUnit);
    AudioOutputUnitStop(outputUnit);
    AudioUnitUninitialize(inputUnit);
    AudioUnitUninitialize(outputUnit);
    AudioComponentInstanceDispose(inputUnit);
    AudioComponentInstanceDispose(outputUnit);
    return 0;
}
