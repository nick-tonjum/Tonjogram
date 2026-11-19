// macOS system-audio capture via Core Audio process taps (macOS 14.2+).
#pragma once
#include <cstddef>

// chans[c] points at the first sample of channel c; consecutive samples are `stride` floats apart.
typedef void (*MacTapCallback)(void* user, const float* const* chans, int numChannels, int stride, int frames);

struct MacSystemTap;

bool macTapSupported();
// Returns nullptr on failure and writes a message into err.
MacSystemTap* macTapCreate(MacTapCallback cb, void* user, char* err, size_t errLen);
double macTapSampleRate(MacSystemTap* t);
int macTapChannels(MacSystemTap* t);
void macTapDestroy(MacSystemTap* t);
