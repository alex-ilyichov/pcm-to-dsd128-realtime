#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

// Configuration constants
#define OUTPUT_SAMPLE_RATE 176400.0    // PCM sample rate for DoP output (Hz)
#define TARGET_DSD_RATE    5644800.0   // Target DSD rate (DSD128 = 5.6448 MHz)

// Delta-sigma modulator (simple 5th-order)
float noise_shaper_5th_order(float pcm_input, float state[5]) {
    // 5th-order feedback loop: integrators accumulate error, final state outputs 1-bit
    state[0] += pcm_input - state[4];
    state[1] += state[0] - state[4];
    state[2] += state[1] - state[4];
    state[3] += state[2] - state[4];
    // Quantize: output +1 or -1 based on sign of 4th integrator (state[3])
    state[4] = (state[3] >= 0.0f ? 1.0f : -1.0f);
    return state[4];
}

// Pack two bytes of DSD into a 3-byte DoP frame (with marker in the MSB)
void dsd_to_dop_frame(uint8_t byte1, uint8_t byte2, uint8_t *dop_frame, bool marker_toggle) {
    // Least significant 16 bits carry DSD data, MSB carries DoP marker (0x05 or 0xFA)
    dop_frame[0] = byte1;
    dop_frame[1] = byte2;
    dop_frame[2] = (marker_toggle ? 0xFA : 0x05);  // toggle marker each frame
}

// Utility: find an audio device by name (exact match)
AudioDeviceID getDeviceByName(const char* deviceName) {
    AudioDeviceID result = kAudioDeviceUnknown;
    UInt32 size = 0;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    // Get all device IDs
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) == noErr) {
        UInt32 deviceCount = size / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> deviceIDs(deviceCount);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, deviceIDs.data()) == noErr) {
            // Iterate devices and check their names
            for (AudioDeviceID dev : deviceIDs) {
                CFStringRef nameRef = nullptr;
                UInt32 nameSize = sizeof(nameRef);
                AudioObjectPropertyAddress nameAddr = {
                    kAudioObjectPropertyName,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(dev, &nameAddr, 0, nullptr, &nameSize, &nameRef) == noErr && nameRef) {
                    char nameBuf[256];
                    CFStringGetCString(nameRef, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
                    CFRelease(nameRef);
                    if (std::strcmp(nameBuf, deviceName) == 0) {
                        result = dev;
                        break;
                    }
                }
            }
        }
    }
    return result;
}

// Globals for audio state
static AudioUnit outputUnit;          // HAL Output AudioUnit
static float state_L[5] = {0.0f};     // Modulator state (left channel)
static float state_R[5] = {0.0f};     // Modulator state (right channel)
static bool  marker_toggle = false;   // DoP marker toggle state (alternates each frame)
static uint64_t frame_counter = 0;    // Frame counter for tone generation timing

// Audio output callback – fills the buffer with DoP-encapsulated DSD data
OSStatus OutputCallback(void *inRefCon,
                        AudioUnitRenderActionFlags *ioActionFlags,
                        const AudioTimeStamp *inTimeStamp,
                        UInt32 inBusNumber,
                        UInt32 inNumberFrames,
                        AudioBufferList *ioData) {
    // We assume one interleaved buffer (stereo) with 24-bit PCM frames (packed in 3 bytes per channel).
    UInt8 *outBuffer = reinterpret_cast<UInt8*>(ioData->mBuffers[0].mData);
    static const float amplitude = 0.5f;   // -6 dBFS amplitude for the sine wave
    static const float freq = 1000.0f;     // 1 kHz test tone frequency

    for (UInt32 frame = 0; frame < inNumberFrames; ++frame, ++frame_counter) {
        float pcm_L, pcm_R;
        // Generate 1 kHz sine wave for the first 1 second, then silence
        if (frame_counter < static_cast<uint64_t>(OUTPUT_SAMPLE_RATE * 1.0)) {
            // Compute sine sample (using frame_counter as sample index at 176.4 kHz)
            double theta = 2.0 * M_PI * freq * (double)frame_counter / OUTPUT_SAMPLE_RATE;
            pcm_L = amplitude * std::sin(theta);
            pcm_R = pcm_L;
        } else {
            // After 1 second, output silence
            pcm_L = 0.0f;
            pcm_R = 0.0f;
        }

        // Run the 5th-order delta-sigma modulator to get 1-bit DSD output for each channel
        float dsd_bit_L = noise_shaper_5th_order(pcm_L, state_L);
        float dsd_bit_R = noise_shaper_5th_order(pcm_R, state_R);

        // Convert 1-bit output to byte values: 0xFF for '1', 0x00 for '0' (DSD high or low)
        uint8_t dsd_byte_L = (dsd_bit_L > 0.0f ? 0xFF : 0x00);
        uint8_t dsd_byte_R = (dsd_bit_R > 0.0f ? 0xFF : 0x00);

        // Pack two identical bytes (since we have only 1-bit per frame here) into DoP frames for L & R
        uint8_t dop_frame_L[3];
        uint8_t dop_frame_R[3];
        dsd_to_dop_frame(dsd_byte_L, dsd_byte_L, dop_frame_L, marker_toggle);
        dsd_to_dop_frame(dsd_byte_R, dsd_byte_R, dop_frame_R, marker_toggle);

        // Interleave L/R DoP frame bytes into the output buffer (24 bits per channel = 3 bytes)
        // Each frame contributes 6 bytes total: L(3 bytes) + R(3 bytes)
        size_t byteOffset = frame * 6;
        outBuffer[byteOffset + 0] = dop_frame_L[0];
        outBuffer[byteOffset + 1] = dop_frame_L[1];
        outBuffer[byteOffset + 2] = dop_frame_L[2];
        outBuffer[byteOffset + 3] = dop_frame_R[0];
        outBuffer[byteOffset + 4] = dop_frame_R[1];
        outBuffer[byteOffset + 5] = dop_frame_R[2];

        // Toggle the DoP marker for next frame (alternates 0x05 <-> 0xFA)
        marker_toggle = !marker_toggle;
    }
    return noErr;
}

int main() {
    // Initialize CoreAudio HAL output unit
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent defaultOutput = AudioComponentFindNext(nullptr, &desc);
    if (defaultOutput == nullptr) {
        std::cerr << "ERROR: Cannot find HALOutput AudioComponent" << std::endl;
        return -1;
    }
    OSStatus err = AudioComponentInstanceNew(defaultOutput, &outputUnit);
    if (err != noErr) {
        std::cerr << "ERROR: AudioComponentInstanceNew failed (err=" << err << ")" << std::endl;
        return -1;
    }

    // Enable output scope (speaker output) on the HAL unit
    UInt32 enableIO = 1;
    err = AudioUnitSetProperty(outputUnit,
                               kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output,
                               0,             // element 0 = output bus
                               &enableIO,
                               sizeof(enableIO));
    if (err != noErr) {
        std::cerr << "ERROR: Enabling HAL output IO failed (err=" << err << ")" << std::endl;
        return -1;
    }

    // Attempt to bind to a specific output device (e.g., iFi DAC) by name
    AudioDeviceID outputDevice = getDeviceByName("iFi (by AMR) HD USB Audio ");
    if (outputDevice != kAudioDeviceUnknown) {
        // Found the iFi DAC, set it as the current device for the HAL output unit
        err = AudioUnitSetProperty(outputUnit,
                                   kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global,
                                   0,
                                   &outputDevice,
                                   sizeof(outputDevice));
        if (err != noErr) {
            std::cerr << "WARNING: Failed to set iFi DAC as output device (err=" << err << "). Using default output." << std::endl;
        } else {
            std::cout << "Output device set to iFi DAC." << std::endl;
        }
    } else {
        std::cout << "iFi DAC not found. Using default output device." << std::endl;
    }

    // Configure the stream format for the output (176.4 kHz, 24-bit int, 2 channels, interleaved)
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = OUTPUT_SAMPLE_RATE;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = 2;
    asbd.mFramesPerPacket  = 1;
    asbd.mBitsPerChannel   = 24;
    asbd.mBytesPerFrame    = (asbd.mBitsPerChannel / 8) * asbd.mChannelsPerFrame;  // 3 bytes * 2 = 6
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame * asbd.mFramesPerPacket;         // also 6 bytes
    // Note: 24-bit packed means 3 bytes per sample per channel.

    err = AudioUnitSetProperty(outputUnit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input,
                               0,  // element 0 (input scope of output unit receives our audio)
                               &asbd,
                               sizeof(asbd));
    if (err != noErr) {
        std::cerr << "ERROR: Setting stream format failed (err=" << err << ")" << std::endl;
        return -1;
    }
    std::cout << "Output stream configured: 176.4 kHz, 24-bit PCM (DoP frames)" << std::endl;

    // Set the render callback for the output unit
    AURenderCallbackStruct cb;
    cb.inputProc = OutputCallback;
    cb.inputProcRefCon = nullptr;
    err = AudioUnitSetProperty(outputUnit,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input,
                               0,
                               &cb,
                               sizeof(cb));
    if (err != noErr) {
        std::cerr << "ERROR: Setting render callback failed (err=" << err << ")" << std::endl;
        return -1;
    }

    // Initialize and start the audio output unit
    err = AudioUnitInitialize(outputUnit);
    if (err == noErr) {
        err = AudioOutputUnitStart(outputUnit);
    }
    if (err != noErr) {
        std::cerr << "ERROR: Starting audio output failed (err=" << err << ")" << std::endl;
        return -1;
    }
    std::cout << "Playing 1 kHz DSD128 tone over DoP..." << std::endl;

    // Keep the main thread alive while audio is playing
    std::this_thread::sleep_for(std::chrono::seconds(5));
    // (In a real application, you might run indefinitely or until user stops, but we sleep 5s for demo.)

    // Teardown: stop and cleanup (not strictly necessary if program exits)
    AudioOutputUnitStop(outputUnit);
    AudioUnitUninitialize(outputUnit);
    AudioComponentInstanceDispose(outputUnit);
    return 0;
}
