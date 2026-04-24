#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>

#define OUTPUT_SAMPLE_RATE 176400
#define BUFFER_SIZE 512

// 5th-order Delta-Sigma Modulator
float noise_shaper_5th_order(float pcm_sample, float state[5]) {
    float input = pcm_sample;
    state[0] += input - state[4];
    state[1] += state[0] - state[4];
    state[2] += state[1] - state[4];
    state[3] += state[2] - state[4];
    state[4] = state[3] >= 0.0f ? 1.0f : -1.0f;
    return state[4];
}

// DoP packing
void dsd_to_dop_frame(uint8_t dsd_byte1, uint8_t dsd_byte2, uint8_t* dop_frame, bool marker_toggle) {
    dop_frame[0] = dsd_byte1;
    dop_frame[1] = dsd_byte2;
    dop_frame[2] = marker_toggle ? 0xFA : 0x05;
}

// Global frame counter
uint64_t global_frame_counter = 0;

// HAL Output Device
AudioDeviceID outputDevice = kAudioObjectUnknown;

// Render callback
OSStatus RenderCallback(void* inRefCon,
                        AudioUnitRenderActionFlags* ioActionFlags,
                        const AudioTimeStamp* inTimeStamp,
                        UInt32 inBusNumber,
                        UInt32 inNumberFrames,
                        AudioBufferList* ioData) {
    static float state_L[5] = {0};
    static float state_R[5] = {0};
    static bool marker_toggle = false;

    if (!ioData || !ioData->mBuffers[0].mData) {
        return noErr;
    }

    uint8_t* buffer = (uint8_t*)ioData->mBuffers[0].mData; // interleaved

    for (UInt32 frame = 0; frame < inNumberFrames; ++frame, ++global_frame_counter) {
        float sample_L = 0.0f;
        float sample_R = 0.0f;

        float dsd_L = noise_shaper_5th_order(sample_L, state_L);
        float dsd_R = noise_shaper_5th_order(sample_R, state_R);

        uint8_t dsd_bytes_L[2] = { (uint8_t)(dsd_L > 0 ? 0xFF : 0x00), (uint8_t)(dsd_L > 0 ? 0xFF : 0x00) };
        uint8_t dsd_bytes_R[2] = { (uint8_t)(dsd_R > 0 ? 0xFF : 0x00), (uint8_t)(dsd_R > 0 ? 0xFF : 0x00) };

        uint8_t dop_frame_L[3];
        uint8_t dop_frame_R[3];

        dsd_to_dop_frame(dsd_bytes_L[0], dsd_bytes_L[1], dop_frame_L, marker_toggle);
        dsd_to_dop_frame(dsd_bytes_R[0], dsd_bytes_R[1], dop_frame_R, marker_toggle);

        buffer[frame * 6 + 0] = dop_frame_L[0];
        buffer[frame * 6 + 1] = dop_frame_L[1];
        buffer[frame * 6 + 2] = dop_frame_L[2];
        buffer[frame * 6 + 3] = dop_frame_R[0];
        buffer[frame * 6 + 4] = dop_frame_R[1];
        buffer[frame * 6 + 5] = dop_frame_R[2];

        marker_toggle = !marker_toggle;
    }

    return noErr;
}

int main() {
    // Find default output device ID
    UInt32 size = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMaster
    };

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, nullptr, &size, &outputDevice) != noErr) {
        std::cerr << "Unable to get default output device!" << std::endl;
        return -1;
    }

    // Setup HAL Output AudioUnit
    AudioComponent comp;
    AudioComponentDescription desc;
    AudioUnit outputAU;

    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    comp = AudioComponentFindNext(NULL, &desc);
    AudioComponentInstanceNew(comp, &outputAU);

    UInt32 enableIO = 1;
    AudioUnitSetProperty(outputAU,
                         kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output,
                         0,
                         &enableIO,
                         sizeof(enableIO));

    AudioUnitSetProperty(outputAU,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0,
                         &outputDevice,
                         sizeof(outputDevice));

    // Define strict 24-bit 176.4kHz PCM format
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate = OUTPUT_SAMPLE_RATE;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket = 1;
    asbd.mChannelsPerFrame = 2;
    asbd.mBitsPerChannel = 24;
    asbd.mBytesPerFrame = 6;
    asbd.mBytesPerPacket = 6;
    asbd.mReserved = 0;

    OSStatus err = AudioUnitSetProperty(outputAU,
                                        kAudioUnitProperty_StreamFormat,
                                        kAudioUnitScope_Input,
                                        0,
                                        &asbd,
                                        sizeof(asbd));
    if (err != noErr) {
        std::cerr << "Failed to set audio stream format!" << std::endl;
        return -2;
    }

    AURenderCallbackStruct cb;
    cb.inputProc = RenderCallback;
    cb.inputProcRefCon = NULL;

    err = AudioUnitSetProperty(outputAU,
                                kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input,
                                0,
                                &cb,
                                sizeof(cb));
    if (err != noErr) {
        std::cerr << "Failed to set render callback!" << std::endl;
        return -3;
    }

    AudioUnitInitialize(outputAU);
    AudioOutputUnitStart(outputAU);

    std::cout << "Streaming DSD128 DoP via HALOutputUnit @ 176.4kHz directly to DAC..." << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    AudioOutputUnitStop(outputAU);
    AudioUnitUninitialize(outputAU);
    AudioComponentInstanceDispose(outputAU);

    return 0;
}
