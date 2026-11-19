#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "ring_buffer.h"

enum class SourceKind {
    SystemDefault,   // "everything you hear" on the default output
    LoopbackDevice,  // Windows: loopback of a specific output device
    Input,           // microphone / line-in / Linux monitor source
};

struct AudioSource {
    std::string name;
    SourceKind kind;
    int deviceIndex = -1;  // index into the backend's device list
};

enum ChannelMode { CH_MIX = 0, CH_LEFT, CH_RIGHT, CH_SIDE };

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    void refreshSources();
    const std::vector<AudioSource>& sources() const { return sources_; }

    bool start(int sourceIndex);
    void stop();
    bool running() const { return running_; }
    int currentSource() const { return current_; }
    unsigned sampleRate() const { return sampleRate_; }
    unsigned channels() const { return channels_; }
    const std::string& error() const { return error_; }
    const char* backendName() const;

    std::atomic<int> channelMode{CH_MIX};
    SpscRing ring{1u << 21};

    // Called from audio threads.
    void push(const float* const* chans, int numChannels, int stride, int frames);

private:
    void enumerate();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<AudioSource> sources_;
    bool running_ = false;
    int current_ = -1;
    unsigned sampleRate_ = 0, channels_ = 0;
    std::string error_;
};
