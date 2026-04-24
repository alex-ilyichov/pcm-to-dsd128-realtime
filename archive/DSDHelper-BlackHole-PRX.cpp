#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cmath>
#include <iostream>
#include <cstring>

#define OUTPUT_SAMPLE_RATE 176400.0 // DoP output rate (PCM, 24-bit, 2ch)
#define TARGET_DSD_RATE    5644800.0 // DSD128 = 5.6448 MHz

const char* INPUT_DEVICE_NAME = "BlackHole 2ch";
const char* OUTPUT_DEVICE_NAME = "iFi (by AMR) HD USB Audio "; // Use exact name from Audio MIDI Setup

// ====== 5th-order delta-sigma modulator ======
float noise_shaper_5th_order(float pcm_sample, float state[5]) {
    state[0] += pcm_sample - state[4];
    state[1] += state[0] - state[4];
    state[2] += state[1] - state[4];
    state[3] += state[2] - state[4];
    state[4] = state[3] >= 0.0f ? 1.0f : -1.0f;
    return state[4];
}

// ====== DoP frame packer (big endian) ======
void dsd_to_dop_frame(uint8_t dsd_byte1, uint8_t dsd_byte2, uint8_t* dop_frame, bool marker_toggle) {
    dop_frame[0] = marker_toggle ? 0x05 : 0xFA; // Marker in MSB
    dop_frame[1] = dsd_byte1;
    dop_frame[2] = dsd_byte2;
}

// ====== Utility: Find audio device by name ======
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
        if (AudioObjectGetPropertyData(device, &nameAddr, 0, nullptr, &size, &nameRef) == noErr && nameRef) {
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

// ====== Global AudioUnits and State ======
AudioUnit inputUnit;
AudioUnit outputUnit;

static float state_L[5] = {0};
static float state_R[5] = {0};
static bool marker_toggle = false;
static double inputSampleRate = 0.0;
static double oversampleFactor = 1.0;

// ====== Ring Buffer for PCM samples ======
constexpr size_t PCM_RING_BUFFER_SIZE = 262144;
float pcm_ring_L[PCM_RING_BUFFER_SIZE];
float pcm_ring_R[PCM_RING_BUFFER_SIZE];
std::atomic<size_t> pcm_write_idx{0}, pcm_read_idx{0};

// ====== Input callback: fill ring buffer ======
OSStatus InputCallback(void* inRefCon, AudioUnitRenderActionFlags* ioActionFlags,
                      const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber,
                      UInt32 inNumberFrames, AudioBufferList* ioData) {
    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = 2;
    bufferList.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float) * 2;
    bufferList.mBuffers[0].mData = malloc(bufferList.mBuffers[0].mDataByteSize);

    AudioUnitRender(inputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &bufferList);

    float* samples = (float*)bufferList.mBuffers[0].mData;
    for (UInt32 frame = 0; frame < inNumberFrames; ++frame) {
        float left = samples[frame * 2];
        float right = samples[frame * 2 + 1];
        size_t idx = pcm_write_idx.fetch_add(1) % PCM_RING_BUFFER_SIZE;
        pcm_ring_L[idx] = left;
        pcm_ring_R[idx] = right;
    }
    free(bufferList.mBuffers[0].mData);
    return noErr;
}

// ====== Output callback: PCM->DSD128->DoP ======
OSStatus OutputCallback(void* inRefCon, AudioUnitRenderActionFlags* ioActionFlags,
                       const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber,
                       UInt32 inNumberFrames, AudioBufferList* ioData) {
    static float last_L = 0.0f, last_R = 0.0f;

    uint8_t* buffer = (uint8_t*)ioData->mBuffers[0].mData;

    // For each output PCM frame (at 176.4kHz), we must generate 16 DSD bits per channel
    for (UInt32 frame = 0; frame < inNumberFrames; ++frame) {
        // 1. Get the next PCM sample from the ring buffer (or hold last if underrun)
        float pcm_L = last_L, pcm_R = last_R;
        if (pcm_read_idx.load() != pcm_write_idx.load()) {
            size_t idx = pcm_read_idx.fetch_add(1) % PCM_RING_BUFFER_SIZE;
            pcm_L = pcm_ring_L[idx];
            pcm_R = pcm_ring_R[idx];
            last_L = pcm_L;
            last_R = pcm_R;
        }

        // 2. For each DoP frame, generate 16 DSD bits per channel using the held PCM value
        uint8_t dsd_bytes_L[2] = {0}, dsd_bytes_R[2] = {0};
        for (int bit = 0; bit < 16; ++bit) {
            float dsd_bit_L = noise_shaper_5th_order(pcm_L, state_L);
            float dsd_bit_R = noise_shaper_5th_order(pcm_R, state_R);
            if (dsd_bit_L > 0) dsd_bytes_L[bit / 8] |= (1 << (7 - (bit % 8)));
            if (dsd_bit_R > 0) dsd_bytes_R[bit / 8] |= (1 << (7 - (bit % 8)));
        }
        uint8_t dop_frame_L[3], dop_frame_R[3];
        dsd_to_dop_frame(dsd_bytes_L[0], dsd_bytes_L[1], dop_frame_L, marker_toggle);
        dsd_to_dop_frame(dsd_bytes_R[0], dsd_bytes_R[1], dop_frame_R, marker_toggle);

        // 3. Interleave L/R into output buffer (6 bytes per frame)
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

// ====== Main: AudioUnit setup and run loop ======
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

    std::cout << "BlackHole format: "
              << asbd_in.mSampleRate << " Hz, "
              << asbd_in.mBitsPerChannel << " bits, "
              << ((asbd_in.mFormatFlags & kAudioFormatFlagIsFloat) ? "Float" : "Int")
              << std::endl;

    inputSampleRate = asbd_in.mSampleRate;
    oversampleFactor = TARGET_DSD_RATE / inputSampleRate;
    std::cout << "Detected input sample rate: " << inputSampleRate << " Hz" << std::endl;
    std::cout << "Oversample factor: " << oversampleFactor << "x" << std::endl;

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

    enableIO = 1;
    AudioUnitSetProperty(outputUnit,
        kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Output,
        0,
        &enableIO,
        sizeof(enableIO));

    // Bind HALOutputUnit directly to iFi DAC by name
    AudioDeviceID dacDevice = getDeviceByName(OUTPUT_DEVICE_NAME);
    if (dacDevice == kAudioDeviceUnknown) {
        std::cerr << "iFi DAC not found!" << std::endl;
        return -1;
    }
    OSStatus setDeviceErr = AudioUnitSetProperty(outputUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &dacDevice,
        sizeof(AudioDeviceID));
    if (setDeviceErr != noErr) {
        std::cerr << "Failed to bind outputUnit to DAC. Error: " << setDeviceErr << std::endl;
        return -1;
    }

    AudioStreamBasicDescription asbd_out = {0};
    asbd_out.mSampleRate = OUTPUT_SAMPLE_RATE;
    asbd_out.mFormatID = kAudioFormatLinearPCM;
    asbd_out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd_out.mFramesPerPacket = 1;
    asbd_out.mChannelsPerFrame = 2;
    asbd_out.mBitsPerChannel = 24;
    asbd_out.mBytesPerFrame = 6;
    asbd_out.mBytesPerPacket = 6;

    AudioUnitSetProperty(outputUnit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,
        0,
        &asbd_out,
        sizeof(asbd_out));

    std::cout << "Output stream set to " << OUTPUT_SAMPLE_RATE << " Hz, 24-bit integer PCM (for DoP)" << std::endl;

    AURenderCallbackStruct cb_out;
    cb_out.inputProc = OutputCallback;
    cb_out.inputProcRefCon = NULL;
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb_out, sizeof(cb_out));

    AudioUnitInitialize(outputUnit);
    AudioOutputUnitStart(outputUnit);

    std::cout << "Streaming DAW output via BlackHole to iFi DAC as " << TARGET_DSD_RATE << " Hz DoP!" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
