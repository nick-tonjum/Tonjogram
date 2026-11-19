#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"

#include "audio_capture.h"
#include <cstring>

#ifdef __APPLE__
#include "mac_system_tap.h"
#endif

struct AudioCapture::Impl {
    ma_context ctx{};
    bool ctxOk = false;
    ma_device dev{};
    bool devOk = false;
    std::vector<ma_device_info> playback, capture;
#ifdef __APPLE__
    MacSystemTap* tap = nullptr;
#endif
};

static void maDataCallback(ma_device* d, void*, const void* input, ma_uint32 frames) {
    if (!input) return;
    auto* self = (AudioCapture*)d->pUserData;
    const int nch = (int)d->capture.channels;
    const float* ptrs[32];
    const int n = nch > 32 ? 32 : nch;
    for (int c = 0; c < n; ++c) ptrs[c] = (const float*)input + c;
    self->push(ptrs, n, nch, (int)frames);
}

#ifdef __APPLE__
static void macTapCb(void* user, const float* const* chans, int numChannels, int stride, int frames) {
    ((AudioCapture*)user)->push(chans, numChannels, stride, frames);
}
#endif

AudioCapture::AudioCapture() : impl_(new Impl) {
    impl_->ctxOk = ma_context_init(nullptr, 0, nullptr, &impl_->ctx) == MA_SUCCESS;
    refreshSources();
}

AudioCapture::~AudioCapture() {
    stop();
    if (impl_->ctxOk) ma_context_uninit(&impl_->ctx);
}

const char* AudioCapture::backendName() const {
    return impl_->ctxOk ? ma_get_backend_name(impl_->ctx.backend) : "none";
}

void AudioCapture::push(const float* const* chans, int numChannels, int stride, int frames) {
    if (numChannels <= 0) return;
    const int mode = channelMode.load(std::memory_order_relaxed);
    float tmp[1024];
    int done = 0;
    while (done < frames) {
        const int n = frames - done < 1024 ? frames - done : 1024;
        for (int i = 0; i < n; ++i) {
            const size_t idx = (size_t)(done + i) * stride;
            float v;
            if (numChannels == 1) {
                v = chans[0][idx];
            } else if (mode == CH_LEFT) {
                v = chans[0][idx];
            } else if (mode == CH_RIGHT) {
                v = chans[1][idx];
            } else if (mode == CH_SIDE) {
                v = 0.5f * (chans[0][idx] - chans[1][idx]);
            } else {
                float s = 0;
                for (int c = 0; c < numChannels; ++c) s += chans[c][idx];
                v = s / numChannels;
            }
            tmp[i] = v;
        }
        ring.write(tmp, (size_t)n);
        done += n;
    }
}

void AudioCapture::refreshSources() {
    // Keep pointing at the source we're capturing from, even if the list order changes.
    const std::string currentName = current_ >= 0 && current_ < (int)sources_.size() ? sources_[current_].name : "";
    enumerate();
    current_ = -1;
    for (int i = 0; i < (int)sources_.size(); ++i)
        if (!currentName.empty() && sources_[i].name == currentName) current_ = i;
}

void AudioCapture::enumerate() {
    sources_.clear();
    impl_->playback.clear();
    impl_->capture.clear();

#if defined(__APPLE__)
    sources_.push_back({macTapSupported() ? "System audio (all apps)" : "System audio (needs macOS 14.2+)",
                        SourceKind::SystemDefault, -1});
#elif defined(_WIN32)
    sources_.push_back({"System audio (default output)", SourceKind::SystemDefault, -1});
#else
    sources_.push_back({"System audio (default output monitor)", SourceKind::SystemDefault, -1});
#endif

    if (!impl_->ctxOk) return;
    ma_device_info *pInfos = nullptr, *cInfos = nullptr;
    ma_uint32 pCount = 0, cCount = 0;
    if (ma_context_get_devices(&impl_->ctx, &pInfos, &pCount, &cInfos, &cCount) != MA_SUCCESS) return;
    impl_->playback.assign(pInfos, pInfos + pCount);
    impl_->capture.assign(cInfos, cInfos + cCount);

#ifdef _WIN32
    if (impl_->ctx.backend == ma_backend_wasapi) {
        for (ma_uint32 i = 0; i < pCount; ++i)
            sources_.push_back({std::string("Loopback: ") + impl_->playback[i].name, SourceKind::LoopbackDevice, (int)i});
    }
#endif
    for (ma_uint32 i = 0; i < cCount; ++i)
        sources_.push_back({std::string("Input: ") + impl_->capture[i].name, SourceKind::Input, (int)i});
}

bool AudioCapture::start(int sourceIndex) {
    stop();
    error_.clear();
    if (sourceIndex < 0 || sourceIndex >= (int)sources_.size()) {
        error_ = "Invalid source";
        return false;
    }
    const AudioSource& src = sources_[sourceIndex];
    ring.clear();

#ifdef __APPLE__
    if (src.kind == SourceKind::SystemDefault) {
        char err[256] = {0};
        impl_->tap = macTapCreate(macTapCb, this, err, sizeof(err));
        if (!impl_->tap) {
            error_ = err;
            return false;
        }
        sampleRate_ = (unsigned)macTapSampleRate(impl_->tap);
        channels_ = (unsigned)macTapChannels(impl_->tap);
        running_ = true;
        current_ = sourceIndex;
        return true;
    }
#endif

    if (!impl_->ctxOk) {
        error_ = "No audio backend available";
        return false;
    }

    auto tryInit = [&](ma_device_type type, const ma_device_id* id) -> bool {
        ma_device_config cfg = ma_device_config_init(type);
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 0;  // native
        cfg.capture.pDeviceID = id;
        cfg.sampleRate = 0;        // native
        cfg.periodSizeInMilliseconds = 10;
        cfg.dataCallback = maDataCallback;
        cfg.pUserData = this;
        if (ma_device_init(&impl_->ctx, &cfg, &impl_->dev) != MA_SUCCESS) return false;
        if (ma_device_start(&impl_->dev) != MA_SUCCESS) {
            ma_device_uninit(&impl_->dev);
            return false;
        }
        return true;
    };

    bool ok = false;
    switch (src.kind) {
        case SourceKind::Input:
            ok = tryInit(ma_device_type_capture, &impl_->capture[src.deviceIndex].id);
            break;
        case SourceKind::LoopbackDevice:
            ok = tryInit(ma_device_type_loopback, &impl_->playback[src.deviceIndex].id);
            break;
        case SourceKind::SystemDefault:
#if defined(_WIN32)
            ok = tryInit(ma_device_type_loopback, nullptr);
#elif defined(__APPLE__)
            ok = false;
#else
            if (impl_->ctx.backend == ma_backend_pulseaudio) {
                ma_device_id id;
                memset(&id, 0, sizeof(id));
                strncpy(id.pulse, "@DEFAULT_MONITOR@", sizeof(id.pulse) - 1);
                ok = tryInit(ma_device_type_capture, &id);
                if (!ok) {
                    // Fall back to the first monitor source we can find.
                    for (auto& c : impl_->capture) {
                        const size_t len = strlen(c.id.pulse);
                        if (len > 8 && strcmp(c.id.pulse + len - 8, ".monitor") == 0) {
                            if ((ok = tryInit(ma_device_type_capture, &c.id))) break;
                        }
                    }
                }
            } else {
                error_ = "System audio capture needs PulseAudio or PipeWire (pipewire-pulse). Pick an input instead.";
                return false;
            }
#endif
            break;
    }
    if (!ok) {
        if (error_.empty()) error_ = "Failed to open audio device";
        return false;
    }
    impl_->devOk = true;
    sampleRate_ = impl_->dev.sampleRate;
    channels_ = impl_->dev.capture.channels;
    running_ = true;
    current_ = sourceIndex;
    return true;
}

void AudioCapture::stop() {
#ifdef __APPLE__
    if (impl_->tap) {
        macTapDestroy(impl_->tap);
        impl_->tap = nullptr;
    }
#endif
    if (impl_->devOk) {
        ma_device_uninit(&impl_->dev);
        impl_->devOk = false;
    }
    running_ = false;
}
