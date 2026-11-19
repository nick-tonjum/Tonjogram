// Captures everything the system is playing using a global Core Audio process tap
// wrapped in a private aggregate device. Requires macOS 14.2+ and the
// "System Audio Recording" permission (NSAudioCaptureUsageDescription).
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <cstdio>
#include "mac_system_tap.h"

struct MacSystemTap {
    AudioObjectID tapID = kAudioObjectUnknown;
    AudioObjectID aggID = kAudioObjectUnknown;
    AudioDeviceIOProcID procID = nullptr;
    MacTapCallback cb = nullptr;
    void* user = nullptr;
    double sampleRate = 48000;
    int channels = 2;
    bool interleaved = true;
};

static AudioObjectPropertyAddress addrOf(AudioObjectPropertySelector sel) {
    return {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

static NSString* stringProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = addrOf(sel);
    CFStringRef s = nullptr;
    UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &s) != noErr || !s) return nil;
    return (__bridge_transfer NSString*)s;
}

static void fail(char* err, size_t len, const char* what, OSStatus st) {
    if (err && len) snprintf(err, len, "%s (OSStatus %d)", what, (int)st);
}

bool macTapSupported() {
    if (@available(macOS 14.2, *)) return true;
    return false;
}

MacSystemTap* macTapCreate(MacTapCallback cb, void* user, char* err, size_t errLen) {
    if (@available(macOS 14.2, *)) {
        MacSystemTap* t = new MacSystemTap();
        t->cb = cb;
        t->user = user;

        CATapDescription* desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        desc.name = @"Tonjogram system tap";
        desc.UUID = [NSUUID UUID];
        desc.privateTap = YES;
        desc.muteBehavior = CATapUnmuted;

        OSStatus st = AudioHardwareCreateProcessTap(desc, &t->tapID);
        if (st != noErr) {
            fail(err, errLen, "Could not create system audio tap", st);
            delete t;
            return nullptr;
        }

        NSString* tapUID = stringProp(t->tapID, kAudioTapPropertyUID);
        if (!tapUID) tapUID = desc.UUID.UUIDString;

        AudioStreamBasicDescription fmt = {};
        {
            AudioObjectPropertyAddress a = addrOf(kAudioTapPropertyFormat);
            UInt32 size = sizeof(fmt);
            if (AudioObjectGetPropertyData(t->tapID, &a, 0, nullptr, &size, &fmt) == noErr) {
                if (fmt.mChannelsPerFrame > 0) t->channels = (int)fmt.mChannelsPerFrame;
                if (fmt.mSampleRate > 0) t->sampleRate = fmt.mSampleRate;
                t->interleaved = !(fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
            }
        }

        // Use the current default output device as the clock source.
        AudioObjectID outDev = kAudioObjectUnknown;
        {
            AudioObjectPropertyAddress a = addrOf(kAudioHardwarePropertyDefaultOutputDevice);
            UInt32 size = sizeof(outDev);
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &outDev);
        }
        NSString* outUID = outDev != kAudioObjectUnknown ? stringProp(outDev, kAudioDevicePropertyDeviceUID) : nil;

        NSMutableDictionary* agg = [@{
            @kAudioAggregateDeviceNameKey : @"Tonjogram Tap",
            @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceIsStackedKey : @NO,
            @kAudioAggregateDeviceTapAutoStartKey : @YES,
            @kAudioAggregateDeviceTapListKey : @[ @{
                @kAudioSubTapDriftCompensationKey : @YES,
                @kAudioSubTapUIDKey : tapUID,
            } ],
        } mutableCopy];
        if (outUID) {
            agg[@kAudioAggregateDeviceMainSubDeviceKey] = outUID;
            agg[@kAudioAggregateDeviceSubDeviceListKey] = @[ @{@kAudioSubDeviceUIDKey : outUID} ];
        }

        st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)agg, &t->aggID);
        if (st != noErr) {
            fail(err, errLen, "Could not create aggregate device for tap", st);
            macTapDestroy(t);
            return nullptr;
        }

        {
            Float64 sr = 0;
            AudioObjectPropertyAddress a = addrOf(kAudioDevicePropertyNominalSampleRate);
            UInt32 size = sizeof(sr);
            if (AudioObjectGetPropertyData(t->aggID, &a, 0, nullptr, &size, &sr) == noErr && sr > 0) t->sampleRate = sr;
        }

        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        dispatch_queue_t queue = dispatch_queue_create("tonjogram.tap", attr);
        const int tapCh = t->channels;
        const bool tapInterleaved = t->interleaved;
        MacSystemTap* self = t;
        st = AudioDeviceCreateIOProcIDWithBlock(
            &t->procID, t->aggID, queue,
            ^(const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*) {
                if (!in || in->mNumberBuffers == 0) return;
                // The tap's stream(s) come after any input streams of the clock sub-device.
                const float* ptrs[16];
                int nch = 0, stride = 1, frames = 0;
                const UInt32 nb = in->mNumberBuffers;
                if (tapInterleaved || nb == 1) {
                    const AudioBuffer& b = in->mBuffers[nb - 1];
                    nch = (int)b.mNumberChannels;
                    if (nch <= 0 || !b.mData) return;
                    if (nch > 16) nch = 16;
                    stride = (int)b.mNumberChannels;
                    frames = (int)(b.mDataByteSize / (sizeof(float) * b.mNumberChannels));
                    for (int c = 0; c < nch; ++c) ptrs[c] = (const float*)b.mData + c;
                } else {
                    nch = tapCh < (int)nb ? tapCh : (int)nb;
                    if (nch > 16) nch = 16;
                    const UInt32 first = nb - (UInt32)nch;
                    frames = (int)(in->mBuffers[first].mDataByteSize / sizeof(float));
                    for (int c = 0; c < nch; ++c) {
                        if (!in->mBuffers[first + c].mData) return;
                        ptrs[c] = (const float*)in->mBuffers[first + c].mData;
                    }
                }
                if (frames > 0) self->cb(self->user, ptrs, nch, stride, frames);
            });
        if (st != noErr) {
            fail(err, errLen, "Could not create IO proc", st);
            macTapDestroy(t);
            return nullptr;
        }
        st = AudioDeviceStart(t->aggID, t->procID);
        if (st != noErr) {
            fail(err, errLen, "Could not start system audio capture", st);
            macTapDestroy(t);
            return nullptr;
        }
        return t;
    }
    if (err && errLen) snprintf(err, errLen, "System audio capture requires macOS 14.2 or newer");
    return nullptr;
}

double macTapSampleRate(MacSystemTap* t) { return t ? t->sampleRate : 0; }
int macTapChannels(MacSystemTap* t) { return t ? t->channels : 0; }

void macTapDestroy(MacSystemTap* t) {
    if (!t) return;
    if (@available(macOS 14.2, *)) {
        if (t->aggID != kAudioObjectUnknown) {
            if (t->procID) {
                AudioDeviceStop(t->aggID, t->procID);
                AudioDeviceDestroyIOProcID(t->aggID, t->procID);
            }
            AudioHardwareDestroyAggregateDevice(t->aggID);
        }
        if (t->tapID != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(t->tapID);
    }
    delete t;
}
