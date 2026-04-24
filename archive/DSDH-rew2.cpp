// DSDHelper-DoP.cpp
// Generates DSD128 via DoP (352.8kHz) with 1kHz sine wave
// Uses HALOutputUnit to stream to iFi DAC or default device

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

// --- Constants ---
#define OUTPUT_SAMPLE_RATE 352800.0
#define TARGET_DSD_RATE    5644800.0
#define DSD_BITS_PER_FRAME 16

// --- Delta-Sigma Modulator (5th-order, no FIR) ---
float noise_shaper_5th_order(float input, float state[5]) {
    state[0] += input - state[4];
    state[1] += state[0] - state[4];
    state[2] += state[1] - state[4];
    state[3] += state[2] - state[4];
    state[4] = (state[3] >= 0.0f) ? 1.0f : -1.0f;
    return state[4];
}

// --- DoP Frame Pack (MSB = Marker, LSB = DSD16) ---
void pack_dop_frame(uint16_t dsd16, uint8_t* dop, bool marker_toggle) {
    dop[0] = marker_toggle ? 0xFA : 0x05;
    dop[1] = (dsd16 >> 8) & 0xFF;
    dop[2] = dsd16 & 0xFF;
}

// --- DAC Device Lookup by Name ---
AudioDeviceID getDeviceByName(const char* name) {
    AudioDeviceID result = kAudioDeviceUnknown;
    UInt32 size = 0;

    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
        return result;

    UInt32 count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> ids(count);

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return result;

    for (UInt32 i = 0; i < count; ++i) {
        CFStringRef nameRef = nullptr;
        UInt32 nameSize = sizeof(nameRef);

        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        if (AudioObjectGetPropertyData(ids[i], &nameAddr, 0, nullptr, &nameSize, &nameRef) == noErr && nameRef) {
            char nameBuf[256];
            CFStringGetCString(nameRef, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
            CFRelease(nameRef);

            if (strcmp(nameBuf, name) == 0) {
                return ids[i];
            }
        }
    }

    return result;
}


// --- Global State ---
static AudioUnit outputUnit;
static float state_L[5] = {0};
static float state_R[5] = {0};
static bool marker_toggle = false;
static uint64_t dsd_counter = 0;

// --- Output Callback ---
OSStatus OutputCallback(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 inFrames, AudioBufferList* ioData) {
    uint8_t* buffer = (uint8_t*)ioData->mBuffers[0].mData;
    for (UInt32 frame = 0; frame < inFrames; ++frame) {
        // --- Hold sine sample constant for 16 DSD ticks ---
        float amplitude = 0.5f;
        float freq = 1000.0f;
        float sample = amplitude * sinf(2.0f * M_PI * freq * (float)dsd_counter / TARGET_DSD_RATE);
        dsd_counter += DSD_BITS_PER_FRAME;

        uint16_t dsd_L = 0, dsd_R = 0;
        for (int i = 0; i < DSD_BITS_PER_FRAME; ++i) {
            dsd_L = (dsd_L << 1) | (noise_shaper_5th_order(sample, state_L) > 0 ? 1 : 0);
            dsd_R = (dsd_R << 1) | (noise_shaper_5th_order(sample, state_R) > 0 ? 1 : 0);
        }

        uint8_t dop_L[3], dop_R[3];
        pack_dop_frame(dsd_L, dop_L, marker_toggle);
        pack_dop_frame(dsd_R, dop_R, marker_toggle);
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

// --- Main Setup ---
int main() {
    AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &outputUnit)) return 1;

    UInt32 enableIO = 1;
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));

    AudioDeviceID dev = getDeviceByName("iFi (by AMR) HD USB Audio ");
    if (dev != kAudioDeviceUnknown) {
        AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev));
    }

    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate       = OUTPUT_SAMPLE_RATE;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = 2;
    asbd.mFramesPerPacket  = 1;
    asbd.mBitsPerChannel   = 24;
    asbd.mBytesPerFrame    = 6;
    asbd.mBytesPerPacket   = 6;

    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
    AURenderCallbackStruct cb = {OutputCallback, nullptr};
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    AudioUnitInitialize(outputUnit);
    AudioOutputUnitStart(outputUnit);

    std::cout << "Streaming 1kHz DSD128 sine wave over DoP at 352.8kHz..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    AudioOutputUnitStop(outputUnit);
    AudioUnitUninitialize(outputUnit);
    AudioComponentInstanceDispose(outputUnit);
    return 0;
}
