#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <vector>

int main() {
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
            // Print with quotes and byte length so we can see any trailing spaces/chars
            std::cout << "ID " << ids[i] << ": \"" << buf << "\" (len=" << strlen(buf) << ")" << std::endl;
        }
    }
    return 0;
}
