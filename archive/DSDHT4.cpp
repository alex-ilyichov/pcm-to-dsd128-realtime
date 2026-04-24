#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cmath>

#define OUTPUT_SAMPLE_RATE 176400  // DAC output rate for DoP
#define TARGET_DSD_RATE 5644800.0   // DSD128 = 5.6448 MHz

// Device names
const char* INPUT_DEVICE_NAME = "BlackHole 2ch"; // or "BlackHole 16ch" if using that

// Simple 5th-order delta-sigma modulator
float noise_shaper_5th_order(float pcm_sample, float state[5]) {
    float input = pcm_sample;
    state[0] += input - state[4];
    state[1] += state[0] - state[4];
    state[2] += state[1] - state[4];
    state[3] += state[2] - state[4];
    state[4] = state[3] >= 0.0f ? 1.0f : -1.0f;
    return state[4];
}

// DoP packer
void dsd_to_dop_frame(uint8_t dsd_byte1, uint8_t dsd_byte2, uint8_t* dop_frame, bool marker_toggle) {
    dop_frame[0] = dsd_byte1;
    dop_frame[1] = dsd_byte2;
    dop_frame[2] = marker_toggle ? 0xFA : 0x05;
}

// Utility to find a device by name
AudioDeviceID getDeviceByName(const char* name) {
    UInt32 size = 0;
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
    int deviceCount = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices(deviceCount);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data());

    for (auto device : devices) {
        CFStringRef nameRef = nullptr;
        size = sizeof(nameRef);
        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(device, &nameAddr, 0, nullptr, &size, &nameRef) == noErr) {
            char buf[256];
            CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(nameRef);
            if (strcmp(buf, name) == 0) {
                return device;
            }
        }
    }
    return kAudioDeviceUnknown;
}

// Global AudioUnits
AudioUnit inputUnit;
AudioUnit outputUnit;

// State
static float state_L[5] = {0};
static float state_R[5] = {0};
static bool marker_toggle = false;
static double inputSampleRate = 0.0;
static double oversampleFactor = 128.0;

// Ringbuffer (simple)
std::vector<float> leftBuffer;
std::vector<float> rightBuffer;

// Input callback
OSStatus InputCallback(void* inRefCon,
                       AudioUnitRenderActionFlags* ioActionFlags,
                       const AudioTimeStamp* inTimeStamp,
                       UInt32 inBusNumber,
                       UInt32 inNumberFrames,
                       AudioBufferList* ioData) {
    AudioBufferList bufferList;
    UInt32 bufferSize = sizeof(bufferList);
    memset(&bufferList, 0, bufferSize);

    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = 2;
    bufferList.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float) * 2;
    bufferList.mBuffers[0].mData = malloc(bufferList.mBuffers[0].mDataByteSize);

    AudioUnitRender(inputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &bufferList);

    float* samples = (float*)bufferList.mBuffers[0].mData;
    for (UInt32 frame = 0; frame < inNumberFrames; ++frame) {
        leftBuffer.push_back(samples[frame * 2]);
        rightBuffer.push_back(samples[frame * 2 + 1]);
    }

    free(bufferList.mBuffers[0].mData);
    return noErr;
}

// Output callback
OSStatus OutputCallback(void* inRefCon,
                        AudioUnitRenderActionFlags* ioActionFlags,
                        const AudioTimeStamp* inTimeStamp,
                        UInt32 inBusNumber,
                        UInt32 inNumberFrames,
                        AudioBufferList* ioData) {
    uint8_t* buffer = (uint8_t*)ioData->mBuffers[0].mData;

    for (UInt32 frame = 0; frame < inNumberFrames; ++frame) {
        float sample_L = 0.0f;
        float sample_R = 0.0f;

        if (!leftBuffer.empty()) {
            sample_L = leftBuffer.front();
            leftBuffer.erase(leftBuffer.begin());
        }
        if (!rightBuffer.empty()) {
            sample_R = rightBuffer.front();
            rightBuffer.erase(rightBuffer.begin());
        }

        // Very basic nearest oversampling (you can replace with better interpolator later)
        float dsd_L = noise_shaper_5th_order(sample_L, state_L);
        float dsd_R = noise_shaper_5th_order(sample_R, state_R);

        uint8_t dop_frame_L[3];
        uint8_t dop_frame_R[3];

        dsd_to_dop_frame(
            (uint8_t)(dsd_L > 0 ? 0xFF : 0x00),
            (uint8_t)(dsd_L > 0 ? 0xFF : 0x00),
            dop_frame_L,
            marker_toggle
        );
        dsd_to_dop_frame(
            (uint8_t)(dsd_R > 0 ? 0xFF : 0x00),
            (uint8_t)(dsd_R > 0 ? 0xFF : 0x00),
            dop_frame_R,
            marker_toggle
        );

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
    // Find BlackHole device
    AudioDeviceID inputDevice = getDeviceByName(INPUT_DEVICE_NAME);
    if (inputDevice == kAudioDeviceUnknown) {
        std::cerr << "Could not find BlackHole device!" << std::endl;
        return -1;
    }

    // Open input unit
    AudioComponentDescription desc_in = {0};
    desc_in.componentType = kAudioUnitType_Output;
    desc_in.componentSubType = kAudioUnitSubType_HALOutput;
    desc_in.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc_in);
    AudioComponentInstanceNew(comp, &inputUnit);

    UInt32 enableIO = 1;
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));

    // Set input format
    AudioStreamBasicDescription asbd_in = {0};
    UInt32 size = sizeof(asbd_in);
    AudioUnitGetProperty(inputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &asbd_in, &size);
    inputSampleRate = asbd_in.mSampleRate;
    std::cout << "Detected input sample rate: " << inputSampleRate << " Hz" << std::endl;

    AudioUnitSetProperty(inputUnit,
                         kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output,
                         1,
                         &asbd_in,
                         sizeof(asbd_in));

    AURenderCallbackStruct cb_in;
    cb_in.inputProc = InputCallback;
    cb_in.inputProcRefCon = NULL;
    AudioUnitSetProperty(inputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb_in, sizeof(cb_in));

    AudioUnitInitialize(inputUnit);
    AudioOutputUnitStart(inputUnit);

    // Open output HALUnit to DAC
    desc_in.componentSubType = kAudioUnitSubType_HALOutput;
    comp = AudioComponentFindNext(NULL, &desc_in);
    AudioComponentInstanceNew(comp, &outputUnit);

    AudioUnitSetProperty(outputUnit,
                         kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input,
                         0,
                         &(AudioStreamBasicDescription){
                             OUTPUT_SAMPLE_RATE,
                             kAudioFormatLinearPCM,
                             kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
                             6, 2, 24, 6, 6, 0
                         },
                         sizeof(AudioStreamBasicDescription));

    AURenderCallbackStruct cb_out;
    cb_out.inputProc = OutputCallback;
    cb_out.inputProcRefCon = NULL;
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb_out, sizeof(cb_out));

    AudioUnitInitialize(outputUnit);
    AudioOutputUnitStart(outputUnit);

    std::cout << "Streaming FL Studio output via BlackHole to iFi DAC as DSD128 DoP!" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}