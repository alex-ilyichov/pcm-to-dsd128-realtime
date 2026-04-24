#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <vector>
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

// DoP packing: 2 bytes DSD + 1 marker byte
void dsd_to_dop_frame(uint8_t dsd_byte1, uint8_t dsd_byte2, uint8_t* dop_frame, bool marker_toggle) {
    dop_frame[0] = dsd_byte1;
    dop_frame[1] = dsd_byte2;
    dop_frame[2] = marker_toggle ? 0xFA : 0x05;
}

// Global frame counter (not really needed now, but kept for possible future expansions)
uint64_t global_frame_counter = 0;

// CoreAudio Render Callback
OSStatus RenderCallback(void* inRefCon,
                        AudioUnitRenderActionFlags* ioActionFlags,
                        const AudioTimeStamp* inTimeStamp,
                        UInt32 inBusNumber,
                        UInt32 inNumberFrames,
                        AudioBufferList* ioData) {
    static float state_L[5] = {0};
    static float state_R[5] = {0};
    static bool marker_toggle = false;

    if (ioData == nullptr || ioData->mBuffers[0].mData == nullptr || ioData->mBuffers[1].mData == nullptr) {
        return noErr;
    }




    uint8_t* buffer = (uint8_t*)ioData->mBuffers[0].mData; // Single interleaved buffer

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
    
        // Write Left channel (first 3 bytes)
        buffer[frame * 6 + 0] = dop_frame_L[0];
        buffer[frame * 6 + 1] = dop_frame_L[1];
        buffer[frame * 6 + 2] = dop_frame_L[2];
    
        // Write Right channel (next 3 bytes)
        buffer[frame * 6 + 3] = dop_frame_R[0];
        buffer[frame * 6 + 4] = dop_frame_R[1];
        buffer[frame * 6 + 5] = dop_frame_R[2];
    
        marker_toggle = !marker_toggle;
    }
    

    return noErr;
}

int main() {
    AudioComponent comp;
    AudioComponentDescription desc;
    AudioUnit outputAU;

    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        std::cerr << "Unable to find default audio output device." << std::endl;
        return -1;
    }

    AudioComponentInstanceNew(comp, &outputAU);

    // Strictly define 24-bit PCM format
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate = OUTPUT_SAMPLE_RATE;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket = 1;
    asbd.mChannelsPerFrame = 2;
    asbd.mBitsPerChannel = 24;
    asbd.mBytesPerFrame = 6;  // 3 bytes per channel
    asbd.mBytesPerPacket = 6;
    asbd.mReserved = 0;

    OSStatus err = AudioUnitSetProperty(outputAU,
                                        kAudioUnitProperty_StreamFormat,
                                        kAudioUnitScope_Input,
                                        0,
                                        &asbd,
                                        sizeof(asbd));
    if (err != noErr) {
        std::cerr << "Failed to set audio stream format! Error code: " << err << std::endl;
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
        std::cerr << "Failed to set render callback! Error code: " << err << std::endl;
        return -3;
    }

    AudioUnitInitialize(outputAU);
    AudioOutputUnitStart(outputAU);

    std::cout << "Streaming proper DSD128 DoP @ 176.4kHz to default output (DAC)..." << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    AudioOutputUnitStop(outputAU);
    AudioUnitUninitialize(outputAU);
    AudioComponentInstanceDispose(outputAU);

    return 0;
}
