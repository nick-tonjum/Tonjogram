// Tonjogram — lightweight real-time spectrogram + waveform viewer.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#include "audio_capture.h"
#include "colormaps.h"
#include "dsp.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ---------------------------------------------------------------------------
// Settings (persisted as key=value in the user's config directory)
// ---------------------------------------------------------------------------
enum FreqScale { SCALE_LINEAR = 0, SCALE_LOG, SCALE_MEL, SCALE_COUNT };
static const char* kScaleNames[SCALE_COUNT] = {"Linear", "Logarithmic", "Mel"};
static const int kFftSizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const int kNumFftSizes = sizeof(kFftSizes) / sizeof(kFftSizes[0]);
static const char* kChannelNames[] = {"Mix (mono)", "Left", "Right", "Side (L-R)"};
enum Orientation { ORIENT_AUTO = 0, ORIENT_HORIZONTAL, ORIENT_WATERFALL, ORIENT_REVERSE_WATERFALL, ORIENT_COUNT };
static const char* kOrientNames[ORIENT_COUNT] = {"Auto", "Horizontal", "Waterfall", "Reverse waterfall"};

struct Settings {
    std::string sourceName;
    int fftSize = 4096;
    bool lens = true;           // multi-resolution "lens" around the cursor
    float lensRadius = 90.f;    // px
    int window = WIN_HANN;
    float colsPerSec = 120.f;
    int freqScale = SCALE_MEL;
    float fMin = 20.f, fMax = 20000.f;
    float dbMin = -110.f, dbMax = -10.f;
    int colormap = CMAP_MAGMA;
    int channelMode = CH_MIX;
    bool advanced = false;
    int orientation = ORIENT_AUTO;
    bool showPanel = true, showSpectrum = true, showWaveform = true;
    bool peakHold = true, trigger = false;
    bool showAverage = false;
    float avgSeconds = 3.f;
    float trackJumpHz = 100.f;   // hover tracking line stays within +/- this of the hovered peak
    float trackTightness = 0.6f; // 0 = loose (follows faint/wobbly ridges), 1 = strict
    bool magnifier = true;
    float magZoom = 4.f, magSize = 180.f;
    float waveMs = 40.f, waveGain = 1.f;
    float spectrumHeight = 150.f, waveHeight = 150.f;
    int winW = 1280, winH = 800;
};

static std::filesystem::path configPath() {
    namespace fs = std::filesystem;
    fs::path dir, legacy;  // legacy: folder used before the app was renamed to Tonjogram
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) { dir = fs::path(a) / "Tonjogram"; legacy = fs::path(a) / "RTSpectrogram"; }
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) {
        dir = fs::path(h) / "Library" / "Application Support" / "Tonjogram";
        legacy = fs::path(h) / "Library" / "Application Support" / "RTSpectrogram";
    }
#else
    fs::path base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME")) base = fs::path(h) / ".config";
    if (!base.empty()) { dir = base / "tonjogram"; legacy = base / "rtspectrogram"; }
#endif
    if (dir.empty()) return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "settings.ini";
    if (!fs::exists(file, ec) && !legacy.empty() && fs::exists(legacy / "settings.ini", ec))
        fs::copy_file(legacy / "settings.ini", file, ec);  // carry settings over from the old name
    return file;
}

static void loadSettings(Settings& s) {
    auto p = configPath();
    if (p.empty()) return;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto fl = [&] { return (float)std::atof(v.c_str()); };
        auto in = [&] { return std::atoi(v.c_str()); };
        if (k == "source") s.sourceName = v;
        else if (k == "fft") s.fftSize = in();
        else if (k == "lens") s.lens = in() != 0;
        else if (k == "lensr") s.lensRadius = std::clamp(fl(), 30.f, 400.f);
        else if (k == "window") s.window = in();
        else if (k == "speed") s.colsPerSec = fl();
        else if (k == "scale") s.freqScale = in();
        else if (k == "fmin") s.fMin = fl();
        else if (k == "fmax") s.fMax = fl();
        else if (k == "dbmin") s.dbMin = fl();
        else if (k == "dbmax") s.dbMax = fl();
        else if (k == "cmap") s.colormap = in();
        else if (k == "channel") s.channelMode = in();
        else if (k == "advanced") s.advanced = in() != 0;
        else if (k == "orient") s.orientation = std::clamp(in(), 0, ORIENT_COUNT - 1);
        else if (k == "panel") s.showPanel = in() != 0;
        else if (k == "spectrum") s.showSpectrum = in() != 0;
        else if (k == "waveform") s.showWaveform = in() != 0;
        else if (k == "peakhold") s.peakHold = in() != 0;
        else if (k == "trigger") s.trigger = in() != 0;
        else if (k == "avg") s.showAverage = in() != 0;
        else if (k == "trackjump") s.trackJumpHz = std::clamp(fl(), 1.f, 5000.f);
        else if (k == "tracktight") s.trackTightness = std::clamp(fl(), 0.f, 1.f);
        else if (k == "mag") s.magnifier = in() != 0;
        else if (k == "magzoom") s.magZoom = std::clamp(fl(), 2.f, 16.f);
        else if (k == "magsize") s.magSize = std::clamp(fl(), 100.f, 400.f);
        else if (k == "avgsec") s.avgSeconds = std::clamp(fl(), 0.1f, 60.f);
        else if (k == "wavems") s.waveMs = fl();
        else if (k == "wavegain") s.waveGain = fl();
        else if (k == "spech") s.spectrumHeight = fl();
        else if (k == "waveh") s.waveHeight = fl();
        else if (k == "winw") s.winW = in();
        else if (k == "winh") s.winH = in();
    }
    // Sanitize.
    bool okFft = false;
    for (int n : kFftSizes) okFft |= (n == s.fftSize);
    if (!okFft) s.fftSize = 4096;
    s.window = std::clamp(s.window, 0, WIN_COUNT - 1);
    s.freqScale = std::clamp(s.freqScale, 0, SCALE_COUNT - 1);
    s.colormap = std::clamp(s.colormap, 0, CMAP_COUNT - 1);
    s.channelMode = std::clamp(s.channelMode, 0, 3);
    s.colsPerSec = std::clamp(s.colsPerSec, 5.f, 2000.f);
    s.winW = std::clamp(s.winW, 400, 8000);
    s.winH = std::clamp(s.winH, 300, 8000);
}

static void saveSettings(const Settings& s) {
    auto p = configPath();
    if (p.empty()) return;
    std::ofstream f(p);
    f << "source=" << s.sourceName << "\nfft=" << s.fftSize << "\nlens=" << s.lens << "\nlensr=" << s.lensRadius  << "\nwindow=" << s.window << "\nspeed=" << s.colsPerSec
      << "\nscale=" << s.freqScale << "\nfmin=" << s.fMin << "\nfmax=" << s.fMax << "\ndbmin=" << s.dbMin
      << "\ndbmax=" << s.dbMax << "\ncmap=" << s.colormap << "\nchannel=" << s.channelMode
      << "\nadvanced=" << s.advanced << "\norient=" << s.orientation << "\npanel=" << s.showPanel << "\nspectrum=" << s.showSpectrum << "\nwaveform=" << s.showWaveform
      << "\npeakhold=" << s.peakHold << "\ntrigger=" << s.trigger << "\navg=" << s.showAverage << "\ntrackjump=" << s.trackJumpHz << "\ntracktight=" << s.trackTightness
      << "\nmag=" << s.magnifier << "\nmagzoom=" << s.magZoom << "\nmagsize=" << s.magSize
      << "\navgsec=" << s.avgSeconds  << "\nwavems=" << s.waveMs
      << "\nwavegain=" << s.waveGain << "\nspech=" << s.spectrumHeight << "\nwaveh=" << s.waveHeight
      << "\nwinw=" << s.winW << "\nwinh=" << s.winH << "\n";
}

// ---------------------------------------------------------------------------
// Frequency axis mapping
// ---------------------------------------------------------------------------
static double scaleFwd(int s, double f) {
    switch (s) {
        case SCALE_LOG: return std::log(std::max(f, 1e-3));
        case SCALE_MEL: return 2595.0 * std::log10(1.0 + f / 700.0);
        default: return f;
    }
}
static double scaleInv(int s, double v) {
    switch (s) {
        case SCALE_LOG: return std::exp(v);
        case SCALE_MEL: return 700.0 * (std::pow(10.0, v / 2595.0) - 1.0);
        default: return v;
    }
}

struct FreqAxis {
    int scale = SCALE_LOG;
    double fMin = 20, fMax = 20000;
    double a = 0, b = 1;
    bool valid = false;
    void set(int s, double lo, double hi) {
        scale = s;
        fMin = lo;
        fMax = hi;
        valid = true;
        a = scaleFwd(s, lo);
        b = scaleFwd(s, hi);
    }
    double freqAt(double t) const { return scaleInv(scale, a + t * (b - a)); }  // t: 0 = fMin, 1 = fMax
    double posOf(double f) const { return (scaleFwd(scale, f) - a) / (b - a); }
};

static std::vector<double> freqTicks(const FreqAxis& ax, float pixels, float minSpacing) {
    std::vector<double> cand, out;
    if (ax.scale == SCALE_LINEAR) {
        double span = ax.fMax - ax.fMin;
        double raw = span / std::max(2.0, (double)pixels / (minSpacing * 2.5));
        double mag = std::pow(10.0, std::floor(std::log10(raw)));
        double step = mag;
        for (double m : {1.0, 2.0, 5.0, 10.0})
            if (mag * m >= raw) { step = mag * m; break; }
        for (double f = std::ceil(ax.fMin / step) * step; f <= ax.fMax; f += step) cand.push_back(f);
    } else {
        for (double dec = 1; dec <= 100000; dec *= 10)
            for (double m : {1.0, 2.0, 5.0}) {
                double f = dec * m;
                if (f >= ax.fMin && f <= ax.fMax) cand.push_back(f);
            }
    }
    if (ax.scale != SCALE_LINEAR) std::reverse(cand.begin(), cand.end());  // keep big round numbers first
    double last = -1e9;
    for (double f : cand) {
        double px = ax.posOf(f) * pixels;
        if (std::fabs(px - last) >= minSpacing) {
            out.push_back(f);
            last = px;
        }
    }
    return out;
}

static void fmtFreq(char* buf, size_t n, double f) {
    if (f >= 1000) snprintf(buf, n, (std::fmod(f, 1000) == 0) ? "%.0fk" : "%.1fk", f / 1000);
    else snprintf(buf, n, "%.0f", f);
}

static void noteName(double f, char* buf, size_t n) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (f <= 0) { snprintf(buf, n, "-"); return; }
    double midi = 69.0 + 12.0 * std::log2(f / 440.0);
    int m = (int)std::lround(midi);
    int cents = (int)std::lround((midi - m) * 100);
    int octave = m / 12 - 1;
    snprintf(buf, n, "%s%d %+dc", names[((m % 12) + 12) % 12], octave, cents);
}

// ImGui draw callbacks to switch a texture's filtering mid-frame.
static void glSetNearest(const ImDrawList*, const ImDrawCmd* cmd) {
    glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->UserCallbackData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}
static void glSetLinear(const ImDrawList*, const ImDrawCmd* cmd) {
    glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->UserCallbackData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

// ---------------------------------------------------------------------------
// Spectrogram image: a ring of columns stored in a GL texture (x = time, y = freq)
// ---------------------------------------------------------------------------
struct Spectrogram {
    int W = 0, H = 0;
    int nextCol = 0;  // where the next column is written; oldest visible column
    GLuint tex = 0;
    std::vector<float> db;        // H*W, row-major, row 0 = highest frequency
    std::vector<uint32_t> rgba;   // H*W
    uint32_t lut[256];
    float dbMin = -110, dbMax = -10;
    int dirtyFirst = 0, dirtyCount = 0;
    bool fullUpload = true;

    struct RowMap { int b0, b1; float frac; };  // b1 < 0: interpolate b0..b0+1; b0 < 0: out of range
    std::vector<RowMap> rows;
    std::vector<float> latest, hold;  // latest column (H), peak-hold (H)
    std::vector<uint64_t> colEnd;     // per column: absolute sample index where its FFT frame ended (0 = empty)
    std::vector<double> avgPow;       // running (exponential) average in the power domain (H)
    std::vector<float> avgDb;         // same, in dB (H)
    bool avgInit = false;
    GLuint avgTex = 0;                // 1 x H overlay texture for the "burn"
    int avgTexH = 0;
    std::vector<uint32_t> avgRgba;

    void setColormap(int type) { buildColormap(type, lut); }

    inline uint32_t colorOf(float d) const {
        float t = (d - dbMin) / (dbMax - dbMin);
        int i = (int)(t * 255.f);
        i = i < 0 ? 0 : (i > 255 ? 255 : i);
        return lut[i];
    }

    void recolor() {
        for (size_t i = 0; i < db.size(); ++i) rgba[i] = colorOf(db[i]);
        fullUpload = true;
    }

    void clear() {
        std::fill(db.begin(), db.end(), -300.f);
        std::fill(colEnd.begin(), colEnd.end(), 0);
        std::fill(latest.begin(), latest.end(), -300.f);
        std::fill(hold.begin(), hold.end(), -300.f);
        resetAverage();
        recolor();
    }

    void resetAverage() {
        avgPow.assign(H, 0.0);
        avgDb.assign(H, -300.f);
        avgInit = false;
    }

    // Resize keeping the newest columns (right-aligned) and resampling rows.
    void resize(int newW, int newH) {
        if (newW == W && newH == H) return;
        std::vector<float> nd((size_t)newW * newH, -300.f);
        std::vector<uint64_t> ne(newW, 0);
        for (int j = 0; j < newW && W > 0 && H > 0; ++j) {
            int age = newW - 1 - j;
            if (age >= W) continue;
            int oc = ((nextCol - 1 - age) % W + W) % W;
            ne[j] = colEnd[oc];
            for (int r = 0; r < newH; ++r) {
                int orow = (int)((r + 0.5) * H / newH);
                nd[(size_t)r * newW + j] = db[(size_t)orow * W + oc];
            }
        }
        W = newW;
        H = newH;
        nextCol = 0;
        db.swap(nd);
        colEnd.swap(ne);
        rgba.assign((size_t)W * H, 0);
        latest.assign(H, -300.f);
        hold.assign(H, -300.f);
        resetAverage();
        recolor();
        if (!tex) {
            glGenTextures(1, &tex);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        fullUpload = false;
        dirtyCount = 0;
    }

    void buildRowMap(const FreqAxis& ax, double sampleRate, int fftSize) { buildRowMapFor(ax, sampleRate, fftSize, H, rows); }

    static void buildRowMapFor(const FreqAxis& ax, double sampleRate, int fftSize, int H, std::vector<RowMap>& rows) {
        rows.resize(H);
        const double df = sampleRate / fftSize;
        const int nb = fftSize / 2 + 1;
        const double nyq = sampleRate / 2;
        for (int r = 0; r < H; ++r) {
            double fHi = ax.freqAt(1.0 - (double)r / H);
            double fLo = ax.freqAt(1.0 - (double)(r + 1) / H);
            double fC = ax.freqAt(1.0 - (r + 0.5) / H);
            RowMap m{-1, -1, 0.f};
            if (fC <= nyq) {
                double bl = fLo / df, bh = std::min(fHi, nyq) / df;
                if (bh - bl <= 1.0) {
                    double c = fC / df;
                    int b0 = std::clamp((int)std::floor(c), 0, nb - 2);
                    m = {b0, -1, (float)std::clamp(c - b0, 0.0, 1.0)};
                } else {
                    int b0 = std::clamp((int)std::ceil(bl), 0, nb - 1);
                    int b1 = std::clamp((int)std::floor(bh), 0, nb - 1);
                    if (b1 < b0) b1 = b0;
                    m = {b0, b1, 0.f};
                }
            }
            rows[r] = m;
        }
    }

    // Value of one display row from an FFT's dB bins.
    static inline float rowValueFrom(const RowMap& m, const std::vector<float>& bins) {
        if (m.b0 < 0) return -300.f;
        if (m.b1 < 0) return bins[m.b0] + (bins[m.b0 + 1] - bins[m.b0]) * m.frac;
        float v = bins[m.b0];
        for (int b = m.b0 + 1; b <= m.b1; ++b) v = std::max(v, bins[b]);
        return v;
    }

    // Overwrite an existing column (used when re-rendering history).
    void renderColumn(int col, const float* vals) {
        for (int r = 0; r < H; ++r) {
            const float v = vals[r];
            const size_t i = (size_t)r * W + col;
            db[i] = v;
            rgba[i] = colorOf(v);
        }
    }

    void blankColumn(int col) {
        for (int r = 0; r < H; ++r) {
            const size_t i = (size_t)r * W + col;
            db[i] = -300.f;
            rgba[i] = colorOf(-300.f);
        }
        colEnd[col] = 0;
    }

    // vals: H row values in dB. avgAlpha: exponential-average coefficient per column (0 = disabled).
    void addColumn(const float* vals, bool peakHold, float holdDecay, double avgAlpha, uint64_t endSample) {
        const int col = nextCol;
        colEnd[col] = endSample;
        const bool doAvg = avgAlpha > 0;
        for (int r = 0; r < H; ++r) {
            const float v = vals[r];
            latest[r] = v;
            hold[r] = peakHold ? std::max(v, hold[r] - holdDecay) : v;
            if (doAvg) {
                const double p = std::pow(10.0, v * 0.1);
                avgPow[r] = avgInit ? avgPow[r] + avgAlpha * (p - avgPow[r]) : p;
                avgDb[r] = (float)(10.0 * std::log10(avgPow[r] + 1e-30));
            }
            const size_t i = (size_t)r * W + col;
            db[i] = v;
            rgba[i] = colorOf(v);
        }
        if (doAvg) avgInit = true;
        if (dirtyCount == 0) dirtyFirst = col;
        dirtyCount = std::min(dirtyCount + 1, W);
        nextCol = (nextCol + 1) % W;
    }

    // Rebuild the 1 x H average-strip texture (colormap colors, opaque).
    void uploadAverage() {
        if (H == 0) return;
        avgRgba.resize(H);
        for (int r = 0; r < H; ++r) {
            avgRgba[r] = colorOf(avgDb[r]);
        }
        if (!avgTex) glGenTextures(1, &avgTex);
        glBindTexture(GL_TEXTURE_2D, avgTex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (avgTexH != H) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, avgRgba.data());
            avgTexH = H;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, H, GL_RGBA, GL_UNSIGNED_BYTE, avgRgba.data());
        }
    }

    void uploadRange(int x0, int n) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, W);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, x0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, 0, n, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }

    void upload() {
        if (!tex || (!fullUpload && dirtyCount == 0)) return;
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (fullUpload || dirtyCount >= W) {
            uploadRange(0, W);
        } else {
            int first = dirtyFirst, n = dirtyCount;
            int n1 = std::min(n, W - first);
            uploadRange(first, n1);
            if (n > n1) uploadRange(0, n - n1);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        fullUpload = false;
        dirtyCount = 0;
    }
};

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------
struct App {
    Settings s;
    AudioCapture cap;
    Spectrogram sg;
    FreqAxis axis;

    static constexpr size_t HIST = 1u << 21;  // sample history (power of two)
    std::vector<float> hist = std::vector<float>(HIST, 0.f);
    uint64_t histTotal = 0;
    uint64_t nextColAt = 0;
    std::vector<float> frame;
    bool paused = false;
    unsigned sampleRate = 48000;
    bool rowMapDirty = true;
    int lastCmap = -1;
    float lastDbMin = 0, lastDbMax = 0;
    double peakFreq = 0, peakDb = -300;
    float uiScale = 1.f;
    float fbScale = 1.f;

    // ---- Analysis ----
    // The image uses one FFT of the chosen size. Around the cursor, a "lens" shows a
    // multi-resolution view: each display row uses the FFT size whose bin spacing matches the
    // row's height in Hz, combining up to three neighbouring sizes with a weighted soft-minimum
    // in dB (after Lukin & Todd) so it keeps the sharpest detail of each. The lens fades smoothly
    // into the normal image and is drawn as a separate layer, so the base image is untouched.
    static constexpr int P_MIN = 8, P_MAX = 16;  // FFT sizes 256 .. 65536
    static constexpr int MR_MIN_P = 9, MR_MAX_P = 14;
    static constexpr int MR_TAPS = 3;
    static constexpr float MR_HALFWIDTH = 1.25f;  // blend kernel half-width (octaves of FFT size)
    static constexpr float MR_TAU = 4.f;          // soft-min temperature (dB)
    float hoverTf = -1.f;  // normalized frequency under the cursor this frame (-1 = not hovering)
    int lastUserFft = 0, lastUserWin = -1;
    SpectrumAnalyzer an[P_MAX + 1];
    std::vector<Spectrogram::RowMap> maps[P_MAX + 1];
    int pBase = 12;
    std::vector<int> mrQ;    // H * MR_TAPS size exponents (-1 = unused)
    std::vector<float> mrW;  // H * MR_TAPS weights
    int mrPLo = 12, mrPHi = 12;
    std::vector<float> colVals;  // H row values for the column being computed

    static int log2i(int n) { int p = 0; while ((1 << p) < n) ++p; return p; }

    void rebuildMaps() {
        const int H = sg.H;
        pBase = std::clamp(log2i(s.fftSize), P_MIN, P_MAX);
        Spectrogram::buildRowMapFor(axis, sampleRate, 1 << pBase, H, maps[pBase]);
        // Per-row ideal sizes for the lens.
        mrQ.assign((size_t)H * MR_TAPS, -1);
        mrW.assign((size_t)H * MR_TAPS, 0.f);
        mrPLo = MR_MAX_P;
        mrPHi = MR_MIN_P;
        for (int r = 0; r < H; ++r) {
            const double fHi = axis.freqAt(1.0 - (double)r / H), fLo = axis.freqAt(1.0 - (double)(r + 1) / H);
            const double rowHz = std::max(1e-6, fHi - fLo);
            const double p = std::clamp(std::log2(sampleRate / (2.0 * rowHz)), (double)MR_MIN_P, (double)MR_MAX_P);
            const int qc = (int)std::lround(p);
            float wsum = 0;
            int n = 0;
            for (int q = qc - 1; q <= qc + 1; ++q) {
                if (q < MR_MIN_P || q > MR_MAX_P) continue;
                const float wq = std::max(0.f, 1.f - (float)std::fabs(q - p) / MR_HALFWIDTH);
                if (wq <= 0) continue;
                mrQ[(size_t)r * MR_TAPS + n] = q;
                mrW[(size_t)r * MR_TAPS + n] = wq;
                wsum += wq;
                ++n;
                mrPLo = std::min(mrPLo, q);
                mrPHi = std::max(mrPHi, q);
            }
            for (int i = 0; i < n; ++i) mrW[(size_t)r * MR_TAPS + i] /= wsum;
        }
        if (mrPHi < mrPLo) mrPHi = mrPLo;
        for (int q = mrPLo; q <= mrPHi; ++q)
            if (q != pBase) Spectrogram::buildRowMapFor(axis, sampleRate, 1 << q, H, maps[q]);
        rowMapDirty = false;
        ++lensGen;  // cached lens columns are now stale
    }

    // Samples needed before a column's end position.
    int analysisSpan() const { return 1 << pBase; }

    void fillFrame(int64_t start, int N, float* out) const {
        for (int i = 0; i < N; ++i) {
            const int64_t idx = start + i;
            out[i] = idx < 0 ? 0.f : hist[(uint64_t)idx & (HIST - 1)];
        }
    }

    // Base image column ending at sample 'end' -> colVals.
    void computeColumn(uint64_t end) {
        const int H = sg.H, N = 1 << pBase;
        colVals.resize(H);
        an[pBase].configure(N, s.window);
        frame.resize(N);
        fillFrame((int64_t)end - N, N, frame.data());
        an[pBase].process(frame.data());
        for (int r = 0; r < H; ++r) colVals[r] = Spectrogram::rowValueFrom(maps[pBase][r], an[pBase].db());
    }

    // Multi-resolution column, centred on the same instant as the base column. Returns false if
    // the audio it needs isn't available (yet).
    bool computeLensColumn(uint64_t end, std::vector<float>& out) {
        const int H = sg.H;
        const int64_t center = (int64_t)end - (1 << pBase) / 2;
        const int64_t half = (1 << mrPHi) / 2;
        if (center + half > (int64_t)histTotal) return false;
        if ((int64_t)histTotal - (center - half) > (int64_t)HIST) return false;
        frame.resize(1 << mrPHi);
        for (int q = mrPLo; q <= mrPHi; ++q) {
            const int N = 1 << q;
            an[q].configure(N, s.window);
            fillFrame(center - N / 2, N, frame.data());
            an[q].process(frame.data());
        }
        out.resize(H);
        for (int r = 0; r < H; ++r) {
            float vals[MR_TAPS], ws[MR_TAPS];
            int n = 0;
            float vmin = 1e9f;
            for (int i = 0; i < MR_TAPS; ++i) {
                const int q = mrQ[(size_t)r * MR_TAPS + i];
                if (q < 0) break;
                vals[n] = Spectrogram::rowValueFrom(maps[q][r], an[q].db());
                ws[n] = mrW[(size_t)r * MR_TAPS + i];
                vmin = std::min(vmin, vals[n]);
                ++n;
            }
            if (n <= 1) { out[r] = n ? vals[0] : -300.f; continue; }
            double acc = 0;
            for (int i = 0; i < n; ++i) acc += ws[i] * std::exp(-(double)(vals[i] - vmin) / MR_TAU);
            out[r] = vmin - MR_TAU * (float)std::log(acc);
        }
        return true;
    }

    // ---- Hover lens ----
    struct LensEntry { uint64_t end = 0; int gen = -1; std::vector<float> vals; };
    std::vector<LensEntry> lensCache;  // per physical spectrogram column
    int lensGen = 0;
    GLuint lensTex = 0;
    std::vector<uint32_t> lensRgba;
    float lensFade = 0.f;
    struct LensPos { bool active = false; int k = 0, row = 0; };
    LensPos lensHover, lensHoverNext;  // hover from the previous frame drives this frame's lens
    bool lensDrawn = false;
    ImVec2 lensQuad[4];  // screen corners of the lens layer (for the magnifier)

    // Screen point for texel-edge coordinates (k: 0..W along time, r: 0..H top=high freq).
    static ImVec2 texelToScreen(float k, float r, int W, int H, int orient, ImVec2 i0, ImVec2 i1) {
        const float w = i1.x - i0.x, h = i1.y - i0.y;
        const float tf = 1.f - r / H, age = 1.f - k / W;
        if (orient == ORIENT_HORIZONTAL) return ImVec2(i1.x - age * w, i1.y - tf * h);
        if (orient == ORIENT_WATERFALL) return ImVec2(i0.x + tf * w, i0.y + age * h);
        return ImVec2(i0.x + tf * w, i1.y - age * h);
    }

    void drawLens(ImDrawList* dl, ImVec2 i0, ImVec2 i1, int orient, float timePx, float freqPx) {
        lensDrawn = false;
        const float target = (s.lens && lensHover.active) ? 1.f : 0.f;
        const float dt = ImGui::GetIO().DeltaTime;
        lensFade += (target - lensFade) * std::min(1.f, dt * 10.f);
        if (target == 0.f && lensFade < 0.02f) { lensFade = 0.f; return; }
        if (sg.W == 0 || sg.H == 0 || mrQ.empty()) return;
        if ((int)lensCache.size() != sg.W) lensCache.assign(sg.W, LensEntry{});

        const float R = s.lensRadius * uiScale;
        const float colsPerPx = sg.W / timePx, rowsPerPx = sg.H / freqPx;
        const int kc = std::clamp(lensHover.k, 0, sg.W - 1), rc = std::clamp(lensHover.row, 0, sg.H - 1);
        const int k0 = std::max(0, kc - (int)std::ceil(R * colsPerPx)), k1 = std::min(sg.W - 1, kc + (int)std::ceil(R * colsPerPx));
        const int r0 = std::max(0, rc - (int)std::ceil(R * rowsPerPx)), r1 = std::min(sg.H - 1, rc + (int)std::ceil(R * rowsPerPx));
        if (k1 < k0 || r1 < r0) return;

        // Fill missing lens columns, nearest to the cursor first, within a time budget.
        const double t0 = glfwGetTime();
        for (int d = 0; d <= std::max(kc - k0, k1 - kc); ++d) {
            for (int sgn = 0; sgn < 2; ++sgn) {
                const int k = sgn ? kc + d : kc - d;
                if ((sgn && d == 0) || k < k0 || k > k1) continue;
                const int col = (sg.nextCol + k) % sg.W;
                const uint64_t end = sg.colEnd[col];
                LensEntry& e = lensCache[col];
                if (end == 0 || (e.gen == lensGen && e.end == end)) continue;
                if (glfwGetTime() - t0 > 0.005) goto budget_done;
                if (computeLensColumn(end, e.vals)) { e.end = end; e.gen = lensGen; }
            }
        }
    budget_done:

        // Build the overlay: lens colours with a smooth radial falloff in alpha.
        const int tw = k1 - k0 + 1, th = r1 - r0 + 1;
        lensRgba.assign((size_t)tw * th, 0);
        for (int k = k0; k <= k1; ++k) {
            const int col = (sg.nextCol + k) % sg.W;
            const LensEntry& e = lensCache[col];
            if (e.gen != lensGen || e.end != sg.colEnd[col] || e.end == 0) continue;
            const float dx = (k - kc) / colsPerPx;
            for (int r = r0; r <= r1; ++r) {
                const float dy = (r - rc) / rowsPerPx;
                const float dist = std::sqrt(dx * dx + dy * dy);
                float t = std::clamp((R - dist) / (R * 0.5f), 0.f, 1.f);
                t = t * t * (3.f - 2.f * t) * lensFade;  // smoothstep falloff
                if (t <= 0.f) continue;
                const uint32_t a = (uint32_t)(t * 255.f + 0.5f);
                lensRgba[(size_t)(r - r0) * tw + (k - k0)] = (sg.colorOf(e.vals[r]) & 0x00ffffffu) | (a << 24);
            }
        }
        if (!lensTex) glGenTextures(1, &lensTex);
        glBindTexture(GL_TEXTURE_2D, lensTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, lensRgba.data());

        lensQuad[0] = texelToScreen((float)k0, (float)r0, sg.W, sg.H, orient, i0, i1);
        lensQuad[1] = texelToScreen((float)k1 + 1, (float)r0, sg.W, sg.H, orient, i0, i1);
        lensQuad[2] = texelToScreen((float)k1 + 1, (float)r1 + 1, sg.W, sg.H, orient, i0, i1);
        lensQuad[3] = texelToScreen((float)k0, (float)r1 + 1, sg.W, sg.H, orient, i0, i1);
        dl->PushClipRect(i0, i1, true);
        dl->AddImageQuad((ImTextureID)(intptr_t)lensTex, lensQuad[0], lensQuad[1], lensQuad[2], lensQuad[3],
                         ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
        dl->PopClipRect();
        lensDrawn = true;
    }

    // Magnifier box origin: up-right of the cursor, flipped/clamped to stay in the window.
    ImVec2 magBoxOrigin(ImVec2 mpos) const {
        const float S = uiScale, box = s.magSize * S, off = 24 * S;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 b0(mpos.x + off, mpos.y - off - box);
        if (b0.x + box > vp->WorkPos.x + vp->WorkSize.x) b0.x = mpos.x - off - box;
        if (b0.y < vp->WorkPos.y) b0.y = mpos.y + off;
        b0.x = std::clamp(b0.x, vp->WorkPos.x, std::max(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - box));
        b0.y = std::clamp(b0.y, vp->WorkPos.y, std::max(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - box));
        return b0;
    }

    // Anchor the next tooltip to the magnifier so they never overlap: on the side of the box
    // away from the cursor if it fits, otherwise the other side.
    void placeTooltipByMagnifier(ImVec2 mpos, int lines) const {
        const float S = uiScale, box = s.magSize * S, gap = 6 * S;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 pad = ImGui::GetStyle().WindowPadding;
        const float estH = lines * ImGui::GetTextLineHeightWithSpacing() + 2 * pad.y;
        const ImVec2 b0 = magBoxOrigin(mpos);
        const float top = vp->WorkPos.y, bottom = vp->WorkPos.y + vp->WorkSize.y;
        const bool fitsAbove = b0.y - gap - estH >= top;
        const bool fitsBelow = b0.y + box + gap + estH <= bottom;
        const bool boxAboveCursor = b0.y + box * 0.5f < mpos.y;
        bool above = boxAboveCursor ? fitsAbove || !fitsBelow : !(fitsBelow || !fitsAbove);
        // Grow toward the middle of the window horizontally so the tooltip stays on screen.
        const bool rightHalf = b0.x + box * 0.5f > vp->WorkPos.x + vp->WorkSize.x * 0.5f;
        const float x = rightHalf ? b0.x + box : b0.x, px = rightHalf ? 1.f : 0.f;
        if (above) ImGui::SetNextWindowPos(ImVec2(x, b0.y - gap), ImGuiCond_Always, ImVec2(px, 1));
        else ImGui::SetNextWindowPos(ImVec2(x, b0.y + box + gap), ImGuiCond_Always, ImVec2(px, 0));
    }

    void endFrameHover() {
        hoverTf = -1.f;
        hoverK = -1;
        if (lensHoverNext.active) lensHover = lensHoverNext;
        else lensHover.active = false;  // keep last position for the fade-out
        lensHoverNext.active = false;
    }

    // ---- History re-render: recompute existing columns from stored audio ----
    std::vector<int> rrOrder;   // physical column indices, nearest-to-focus first
    size_t rrPos = 0;
    bool rrBlankMissing = false;  // blank columns we can't recompute (frequency mapping changed)
    int hoverK = -1;            // spectrogram column under the cursor this frame (0 = oldest)

    void requestRerender(bool blankMissing, int focusK = -1) {
        if (sg.W == 0) return;
        if (focusK < 0) focusK = sg.W - 1;
        std::vector<std::pair<int, int>> order(sg.W);
        for (int k = 0; k < sg.W; ++k) order[k] = {std::abs(k - focusK), k};
        std::sort(order.begin(), order.end());
        rrOrder.resize(sg.W);
        for (int i = 0; i < sg.W; ++i) rrOrder[i] = (sg.nextCol + order[i].second) % sg.W;
        rrPos = 0;
        rrBlankMissing = rrBlankMissing || blankMissing;
    }

    void stepRerender(double budgetSec) {
        if (rrPos >= rrOrder.size() || sg.W == 0) return;
        if (rowMapDirty) rebuildMaps();
        const int N = analysisSpan();
        const double t0 = glfwGetTime();
        bool changed = false;
        int n = 0;
        while (rrPos < rrOrder.size()) {
            const int col = rrOrder[rrPos++];
            if (col >= sg.W) continue;
            const uint64_t end = sg.colEnd[col];
            const bool available = end != 0 && end <= histTotal && histTotal - end + (uint64_t)N <= HIST;
            if (available) {
                computeColumn(end);
                sg.renderColumn(col, colVals.data());
                changed = true;
            } else if (rrBlankMissing && end != 0) {
                sg.blankColumn(col);
                changed = true;
            }
            if ((++n & 3) == 0 && glfwGetTime() - t0 > budgetSec) break;
        }
        if (rrPos >= rrOrder.size()) rrBlankMissing = false;
        if (changed) sg.fullUpload = true;
    }

    int hop() const { return std::max(1, (int)std::lround(sampleRate / s.colsPerSec)); }

    void startSource(int idx) {
        if (!cap.start(idx)) {
            sampleRate = 48000;
        } else {
            sampleRate = cap.sampleRate() ? cap.sampleRate() : 48000;
            s.sourceName = cap.sources()[idx].name;
        }
        histTotal = 0;
        std::fill(hist.begin(), hist.end(), 0.f);
        nextColAt = (uint64_t)hop();
        rowMapDirty = true;
        if (sg.W) sg.clear();
    }

    void startPreferredSource() {
        int idx = 0;
        for (int i = 0; i < (int)cap.sources().size(); ++i)
            if (cap.sources()[i].name == s.sourceName) idx = i;
        startSource(idx);
    }

    void pump() {
        static float buf[16384];
        size_t n;
        while ((n = cap.ring.read(buf, 16384)) > 0) {
            if (paused) continue;
            for (size_t i = 0; i < n; ++i) hist[(histTotal + i) & (HIST - 1)] = buf[i];
            histTotal += n;
        }
        if (paused || sg.W == 0) return;

        if (lastUserFft != s.fftSize || lastUserWin != s.window) {
            sg.resetAverage();
            lastUserFft = s.fftSize;
            lastUserWin = s.window;
            rowMapDirty = true;
            requestRerender(false);
        }
        if (rowMapDirty) rebuildMaps();
        const uint64_t h = (uint64_t)hop();
        // If we've fallen too far behind (e.g. window was hidden), skip ahead.
        const uint64_t maxCols = (uint64_t)sg.W;
        if (nextColAt + h * maxCols < histTotal) nextColAt = histTotal - h * (maxCols - 1);
        // Exponential average with time constant avgSeconds (per-column coefficient).
        const double avgAlpha = s.showAverage ? 1.0 - std::exp(-(double)h / sampleRate / std::max(0.05f, s.avgSeconds)) : 0.0;
        if (!s.showAverage) sg.avgInit = false;
        bool any = false;
        while (nextColAt <= histTotal) {
            const uint64_t end = nextColAt;
            computeColumn(end);
            sg.addColumn(colVals.data(), s.peakHold, 60.f / s.colsPerSec * 0.5f, avgAlpha, end);
            nextColAt += h;
            any = true;
        }
        if (any) findPeak();
    }

    void findPeak() {
        const SpectrumAnalyzer& pa = an[pBase];
        if (pa.fftSize() == 0) return;
        const auto& d = pa.db();
        const double df = (double)sampleRate / pa.fftSize();
        int b0 = std::max(1, (int)std::ceil(s.fMin / df));
        int b1 = std::min((int)d.size() - 2, (int)std::floor(s.fMax / df));
        int best = -1;
        float bv = -1e9f;
        for (int b = b0; b <= b1; ++b)
            if (d[b] > bv) { bv = d[b]; best = b; }
        if (best < 0) { peakDb = -300; peakFreq = 0; return; }
        // Parabolic interpolation on dB values.
        double a = d[best - 1], b = d[best], c = d[best + 1];
        double den = a - 2 * b + c;
        double p = den != 0 ? 0.5 * (a - c) / den : 0;
        p = std::clamp(p, -0.5, 0.5);
        peakFreq = (best + p) * df;
        peakDb = b - 0.25 * (a - c) * p;
    }

    void applyDisplaySettings() {
        double nyq = sampleRate / 2.0;
        s.fMax = std::min<float>(s.fMax, (float)nyq);
        if (s.freqScale == SCALE_LOG) s.fMin = std::max(s.fMin, 1.f);
        if (s.fMin >= s.fMax - 1) s.fMin = std::max(0.f, s.fMax - 10);
        if (!axis.valid || axis.scale != s.freqScale || axis.fMin != s.fMin || axis.fMax != s.fMax) {
            axis.set(s.freqScale, s.fMin, s.fMax);
            rowMapDirty = true;
            // Re-render history for the new frequency mapping (instead of wiping it).
            if (sg.W) {
                sg.resetAverage();
                std::fill(sg.latest.begin(), sg.latest.end(), -300.f);
                std::fill(sg.hold.begin(), sg.hold.end(), -300.f);
                requestRerender(true);
            }
        }
        if (s.dbMax < s.dbMin + 1) s.dbMax = s.dbMin + 1;
        if (lastCmap != s.colormap || lastDbMin != s.dbMin || lastDbMax != s.dbMax) {
            sg.setColormap(s.colormap);
            sg.dbMin = s.dbMin;
            sg.dbMax = s.dbMax;
            lastCmap = s.colormap;
            lastDbMin = s.dbMin;
            lastDbMax = s.dbMax;
            if (sg.W) sg.recolor();
        }
        cap.channelMode.store(s.channelMode);
    }

    // ---------------------------- UI ----------------------------
    void drawPanel() {
        const float S = uiScale;
        const bool adv = s.advanced;

        // Cog (closes panel) + Simple / Advanced switch
        {
            const float fh = ImGui::GetFrameHeight();
            if (cogButton("##cogpanel", ImGui::GetCursorScreenPos(), fh, true)) togglePanel();
            ImGui::SameLine();
            float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            for (int i = 0; i < 2; ++i) {
                bool sel = (i == 1) == adv;
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, on);
                if (ImGui::Button(i ? "Advanced" : "Simple", ImVec2(w, 0))) s.advanced = (i == 1);
                if (sel) ImGui::PopStyleColor();
                if (i == 0) ImGui::SameLine();
            }
        }
        ImGui::Spacing();

        // Source
        ImGui::TextDisabled("SOURCE");
        const auto& srcs = cap.sources();
        int cur = cap.currentSource();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##source", cur >= 0 && cur < (int)srcs.size() ? srcs[cur].name.c_str() : "(none)")) {
            if (ImGui::IsWindowAppearing()) cap.refreshSources();  // pick up newly plugged devices
            cur = cap.currentSource();
            for (int i = 0; i < (int)cap.sources().size(); ++i)
                if (ImGui::Selectable(cap.sources()[i].name.c_str(), i == cur)) startSource(i);
            ImGui::EndCombo();
        }
        bool showRestart = false;
        if (cap.running()) {
            if (adv) ImGui::TextDisabled("%u Hz · %u ch · %s", cap.sampleRate(), cap.channels(), cap.backendName());
#ifdef __APPLE__
            if (cur >= 0 && cur < (int)srcs.size() && srcs[cur].kind == SourceKind::SystemDefault && peakDb < -150) {
                ImGui::TextWrapped("No signal? Allow \"System Audio Recording\" in System Settings > Privacy & Security, then press Restart.");
                showRestart = true;
            }
#endif
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
            ImGui::TextWrapped("%s", cap.error().empty() ? "Not capturing" : cap.error().c_str());
            ImGui::PopStyleColor();
            showRestart = true;
        }
        if (showRestart && ImGui::Button("Restart")) startSource(std::max(0, cap.currentSource()));
        if (adv) {
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##channel", &s.channelMode, kChannelNames, 4);
        }

        // Analysis
        ImGui::Spacing();
        ImGui::TextDisabled("ANALYSIS");
        int fftIdx = 0;
        for (int i = 0; i < kNumFftSizes; ++i) if (kFftSizes[i] == s.fftSize) fftIdx = i;
        char fftLabel[32];
        snprintf(fftLabel, sizeof fftLabel, "%d", s.fftSize);
        if (ImGui::BeginCombo("FFT size", fftLabel)) {
            for (int i = 0; i < kNumFftSizes; ++i) {
                char l[32];
                snprintf(l, sizeof l, "%d", kFftSizes[i]);
                if (ImGui::Selectable(l, i == fftIdx)) {
                    s.fftSize = kFftSizes[i];
                    rowMapDirty = true;
                    requestRerender(false);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !adv) ImGui::SetTooltip("Bigger = sharper frequency detail, blurrier timing");
        ImGui::Checkbox("Hover lens", &s.lens);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Around the cursor, every frequency uses its ideal FFT size\n(long for lows, short for highs), fading smoothly into the normal view.");
        if (s.lens && adv) ImGui::SliderFloat("Lens radius", &s.lensRadius, 30.f, 400.f, "%.0f px");
        if (adv) {
            ImGui::TextDisabled("  %.2f Hz/bin · %.1f ms window", (double)sampleRate / s.fftSize, 1000.0 * s.fftSize / sampleRate);
            ImGui::Combo("Window", &s.window, kWindowNames, WIN_COUNT);
        }
        ImGui::SliderFloat("Speed", &s.colsPerSec, 5.f, 2000.f, adv ? "%.0f col/s" : "%.0f", ImGuiSliderFlags_Logarithmic);
        if (adv) {
            int h = hop();
            double overlap = 100.0 * (1.0 - (double)h / analysisSpan());
            ImGui::TextDisabled("  hop %d · overlap %.0f%% · %.1fs shown", h, std::max(0.0, overlap),
                                sg.W ? (double)sg.W * h / sampleRate : 0.0);
        }

        // Display
        ImGui::Spacing();
        ImGui::TextDisabled("DISPLAY");
        ImGui::Combo("Layout", &s.orientation, kOrientNames, ORIENT_COUNT);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto: horizontal when wide, reverse waterfall (newest at the bottom) when tall");
        ImGui::Combo("Scale", &s.freqScale, kScaleNames, SCALE_COUNT);
        float nyq = sampleRate / 2.f;
        if (adv) {
            ImGui::DragFloat("Min freq", &s.fMin, 1.f, 0.f, nyq, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
            ImGui::DragFloat("Max freq", &s.fMax, 10.f, 10.f, nyq, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
        }
        if (ImGui::SmallButton("20–20k")) { s.fMin = 20; s.fMax = 20000; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Speech")) { s.fMin = 50; s.fMax = 8000; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Full")) { s.fMin = s.freqScale == SCALE_LOG ? 10.f : 0.f; s.fMax = nyq; }
        ImGui::SliderFloat(adv ? "dB floor" : "Floor", &s.dbMin, -200.f, 0.f, "%.0f dB");
        if (ImGui::IsItemHovered() && !adv) ImGui::SetTooltip("Lower = show quieter detail");
        if (adv) {
            ImGui::SliderFloat("dB ceiling", &s.dbMax, -150.f, 20.f, "%.0f dB");
            ImGui::Combo("Colormap", &s.colormap, kColormapNames, CMAP_COUNT);
        }

        // Views
        ImGui::Spacing();
        ImGui::TextDisabled("VIEWS");
        ImGui::Checkbox("Spectrum", &s.showSpectrum);
        ImGui::SameLine(150 * S);
        ImGui::Checkbox("Waveform", &s.showWaveform);
        ImGui::Checkbox("Magnifier", &s.magnifier);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoomed box that follows the cursor over the spectrogram.\nScroll to change zoom.");
        if (s.magnifier && adv) {
            ImGui::SliderFloat("Mag zoom", &s.magZoom, 2.f, 16.f, "%.1fx", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Mag size", &s.magSize, 100.f, 400.f, "%.0f px");
        }
        ImGui::Checkbox("Average", &s.showAverage);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Average spectrum: a line on the spectrum view and a strip at the newest edge of the spectrogram");
        if (s.showAverage) {
            ImGui::SliderFloat("Avg time", &s.avgSeconds, 0.1f, 60.f, "%.1f s", ImGuiSliderFlags_Logarithmic);
        }
        if (adv) {
            ImGui::SliderFloat("Track range", &s.trackJumpHz, 1.f, 2000.f, "\xc2\xb1%.0f Hz", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hover tracking line: how far it may drift (in total)\nfrom the hovered peak's frequency.");
            ImGui::SliderFloat("Track tightness", &s.trackTightness, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Higher = stricter: smaller steps, stops sooner when the tone fades,\nno gap bridging. Lower = follows faint or wobbly lines further.");
            if (s.showSpectrum) ImGui::Checkbox("Peak hold", &s.peakHold);
            if (s.showWaveform) {
                if (s.showSpectrum) ImGui::SameLine(150 * S);
                ImGui::Checkbox("Trigger", &s.trigger);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock to rising zero-crossings so periodic signals stand still");
                ImGui::SliderFloat("Wave length", &s.waveMs, 1.f, 5000.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Wave gain", &s.waveGain, 0.25f, 100.f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(90 * S, 0))) paused = !paused;
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(90 * S, 0))) sg.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Space: pause/resume\nC: clear\nH: hide this panel\nDrag the gaps between views to resize them");
    }

    // ---- Controls panel visibility ----
    bool panelCanDock = true;   // window wide enough to dock the panel (set per frame)
    bool overlayOpen = false;   // floating panel used when it can't dock
    void togglePanel() {
        if (panelCanDock) s.showPanel = !s.showPanel;
        else overlayOpen = !overlayOpen;
    }

    // Settings cog drawn with the draw list (no icon font needed).
    bool cogButton(const char* id, ImVec2 pos, float size, bool active) {
        const float S = uiScale;
        ImGui::SetCursorScreenPos(pos);
        bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
        bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = hov ? IM_COL32(70, 70, 85, 230) : IM_COL32(20, 20, 26, 190);
        dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, 4 * S);
        const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
        const ImU32 col = active ? IM_COL32(242, 140, 90, 255) : IM_COL32(225, 225, 232, 255);
        const float R = size * 0.27f, tooth = size * 0.36f, tw = size * 0.075f;
        for (int i = 0; i < 8; ++i) {
            float a = i * 3.14159265f / 4.f, ca = std::cos(a), sa = std::sin(a);
            ImVec2 d(ca, sa), n(-sa, ca);
            auto P = [&](float r, float t) { return ImVec2(c.x + d.x * r + n.x * t, c.y + d.y * r + n.y * t); };
            dl->AddQuadFilled(P(R * 0.8f, -tw), P(tooth, -tw), P(tooth, tw), P(R * 0.8f, tw), col);
        }
        dl->AddCircleFilled(c, R, col, 24);
        dl->AddCircleFilled(c, R * 0.45f, bg, 16);
        if (hov) ImGui::SetTooltip("Controls (H)");
        return clicked;
    }

    // ---- Hover peak tracking ----
    struct PeakTrack { bool active = false; int k = 0, row = 0; };  // k: 0 = oldest column, W-1 = newest
    PeakTrack spectrumHoverTrack;  // set while hovering the spectrum view, drawn on the spectrogram next frame
    std::vector<std::pair<int, int>> ridge;

    float cellDb(int k, int row) const { return sg.db[(size_t)row * sg.W + (sg.nextCol + k) % sg.W]; }

    int localPeakRow(int k, int row, int radius) const {
        int best = row;
        for (int r = std::max(0, row - radius); r <= std::min(sg.H - 1, row + radius); ++r)
            if (cellDb(k, r) > cellDb(k, best)) best = r;
        for (;;) {  // climb to the true local maximum
            int nb = best;
            if (best > 0 && cellDb(k, best - 1) > cellDb(k, nb)) nb = best - 1;
            if (best < sg.H - 1 && cellDb(k, best + 1) > cellDb(k, nb)) nb = best + 1;
            if (nb == best) break;
            best = nb;
        }
        return best;
    }

    double peakRowFreq(int k, int row) const {
        double p = 0;
        if (row > 0 && row < sg.H - 1) {
            double a = cellDb(k, row - 1), b = cellDb(k, row), c = cellDb(k, row + 1);
            double den = a - 2 * b + c;
            if (den < 0) p = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
        return axis.freqAt(1.0 - (row + p + 0.5) / sg.H);
    }

    double rowCenterFreq(int row) const { return axis.freqAt(1.0 - (row + 0.5) / sg.H); }
    double rowOfFreq(double f) const {
        f = std::clamp(f, axis.fMin, axis.fMax);
        if (axis.scale == SCALE_LOG) f = std::max(f, 1e-3);
        return (1.0 - axis.posOf(f)) * sg.H - 0.5;
    }

    // Follow the peak's ridge through time in both directions from (k0, row0).
    // Strict: the whole line stays within +/- trackJumpHz of the starting peak, each column may
    // move only a pixel or two, every point must be a true local maximum, and the line ends as
    // soon as the ridge fades (> 20 dB below the start) or stops being a peak.
    void traceRidge(int k0, int row0) {
        ridge.clear();
        const float startDb = cellDb(k0, row0);
        // Tightness controls: allowed fade below the start level, per-column step, and gap tolerance.
        const float t = std::clamp(s.trackTightness, 0.f, 1.f);
        const float fadeDb = 40.f - 32.f * t;                                   // 40 dB .. 8 dB
        const int stepRows = std::max(1, (int)std::lround((3.5f - 2.5f * t) * fbScale));  // ~3.5 px .. 1 px
        const int maxGap = (int)std::lround(3.f * (1.f - t));                   // 3 .. 0 missed columns
        const float minDb = std::max(s.dbMin + 3.f, startDb - fadeDb);
        const double f0 = rowCenterFreq(row0);
        // Allowed band in rows (rows run high -> low frequency).
        const int bandLo = std::min(row0, std::clamp((int)std::floor(rowOfFreq(f0 + s.trackJumpHz)), 0, sg.H - 1));
        const int bandHi = std::max(row0, std::clamp((int)std::ceil(rowOfFreq(f0 - s.trackJumpHz)), 0, sg.H - 1));
        auto isPeak = [&](int k, int r) {
            const float v = cellDb(k, r);
            return (r == 0 || v >= cellDb(k, r - 1)) && (r == sg.H - 1 || v >= cellDb(k, r + 1));
        };
        // Returns the next row, or -1 when the ridge ends.
        auto step = [&](int k, int r) {
            const int lo = std::max(bandLo, r - stepRows), hi = std::min(bandHi, r + stepRows);
            int best = -1;
            for (int q = lo; q <= hi; ++q)
                if (best < 0 || cellDb(k, q) > cellDb(k, best)) best = q;
            if (best < 0 || cellDb(k, best) < minDb || !isPeak(k, best)) return -1;
            return best;
        };
        // Walk one direction, bridging up to maxGap consecutive missed columns.
        auto walk = [&](int dir) {
            int r = row0, gap = 0;
            for (int k = k0 + dir; k >= 0 && k < sg.W; k += dir) {
                int nr = step(k, r);
                if (nr < 0) {
                    if (++gap > maxGap) break;
                    continue;
                }
                gap = 0;
                r = nr;
                ridge.push_back({k, r});
            }
        };
        walk(-1);
        std::reverse(ridge.begin(), ridge.end());
        ridge.push_back({k0, row0});
        walk(+1);
    }

    int autoOrient = ORIENT_HORIZONTAL;  // decided per frame from the window's shape
    int effectiveOrientation() const { return s.orientation != ORIENT_AUTO ? s.orientation : autoOrient; }

    // Compact "style" dropdown shown when the side panel isn't visible.
    void drawStyleButton(ImVec2 pos) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0.55f));
        if (ImGui::SmallButton("Style    ")) ImGui::OpenPopup("##style");
        ImGui::PopStyleColor();
        {
            // Draw a small down-arrow (not every system font has the glyph).
            ImVec2 bmax = ImGui::GetItemRectMax(), bmin = ImGui::GetItemRectMin();
            float cy = (bmin.y + bmax.y) * 0.5f, cx = bmax.x - 10 * uiScale, r = 3.5f * uiScale;
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.6f), ImVec2(cx + r, cy - r * 0.6f),
                                                          ImVec2(cx, cy + r * 0.7f), IM_COL32(230, 230, 235, 255));
        }
        if (ImGui::BeginPopup("##style")) {
            ImGui::TextDisabled("COLORMAP");
            for (int i = 0; i < CMAP_COUNT; ++i)
                if (ImGui::Selectable(kColormapNames[i], s.colormap == i, ImGuiSelectableFlags_DontClosePopups)) s.colormap = i;
            ImGui::Separator();
            ImGui::TextDisabled("LAYOUT");
            for (int i = 0; i < ORIENT_COUNT; ++i)
                if (ImGui::Selectable(kOrientNames[i], s.orientation == i, ImGuiSelectableFlags_DontClosePopups)) s.orientation = i;
            ImGui::Separator();
            if (ImGui::Selectable("Magnifier", s.magnifier, ImGuiSelectableFlags_DontClosePopups)) s.magnifier = !s.magnifier;
            if (ImGui::Selectable(paused ? "Resume" : "Pause", false, ImGuiSelectableFlags_DontClosePopups)) paused = !paused;
            ImGui::EndPopup();
        }
    }

    // compact: no margins, labels drawn inside the image, no time axis.
    void drawSpectrogram(ImVec2 p0, ImVec2 p1, float axisW, float axisH, bool compact, bool styleButton) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float S = uiScale;
        const int orient = effectiveOrientation();
        const bool vertical = orient != ORIENT_HORIZONTAL;
        if (compact) axisW = axisH = 0;
        ImVec2 i0(p0.x + axisW, p0.y), i1(p1.x, p1.y - axisH);
        // Average strip sits at the "now" edge: right (horizontal), top (waterfall), bottom (reverse).
        ImVec2 a0, a1;
        const bool strip = s.showAverage;
        if (strip) {
            const float sw = 26 * S, sgap = 4 * S;
            if (orient == ORIENT_HORIZONTAL) {
                a0 = ImVec2(i1.x - sw, i0.y); a1 = i1;
                i1.x -= sw + sgap;
            } else if (orient == ORIENT_WATERFALL) {
                a0 = i0; a1 = ImVec2(i1.x, i0.y + sw);
                i0.y += sw + sgap;
            } else {
                a0 = ImVec2(i0.x, i1.y - sw); a1 = i1;
                i1.y -= sw + sgap;
            }
        }
        float w = i1.x - i0.x, h = i1.y - i0.y;
        if (w < 4 || h < 4) return;
        const float timePx = vertical ? h : w, freqPx = vertical ? w : h;

        // Match texture resolution to on-screen pixels (capped). Texture: x = time, y = freq (top = high).
        int tw = std::clamp((int)(timePx * fbScale), 64, 2048);
        int th = std::clamp((int)(freqPx * fbScale), 64, 2048);
        if (tw != sg.W || th != sg.H) {
            sg.resize(tw, th);
            rowMapDirty = true;
            requestRerender(false);
        }
        sg.upload();

        // Map texture onto the screen rect for the chosen orientation.
        // Texture u: oldest (u0) -> newest (u0+1); v: 0 = highest freq, 1 = lowest.
        const float u0 = (float)sg.nextCol / sg.W, uN = u0 + 1.f;
        ImVec2 tl(i0.x, i0.y), tr(i1.x, i0.y), br(i1.x, i1.y), bl(i0.x, i1.y);
        ImVec2 uvTL, uvTR, uvBR, uvBL;
        if (orient == ORIENT_HORIZONTAL) {
            uvTL = {u0, 0}; uvTR = {uN, 0}; uvBR = {uN, 1}; uvBL = {u0, 1};
        } else if (orient == ORIENT_WATERFALL) {  // newest at top, low freq on the left
            uvTL = {uN, 1}; uvTR = {uN, 0}; uvBR = {u0, 0}; uvBL = {u0, 1};
        } else {                                  // newest at bottom
            uvTL = {u0, 1}; uvTR = {u0, 0}; uvBR = {uN, 0}; uvBL = {uN, 1};
        }
        dl->AddImageQuad((ImTextureID)(intptr_t)sg.tex, tl, tr, br, bl, uvTL, uvTR, uvBR, uvBL);
        drawLens(dl, i0, i1, orient, timePx, freqPx);
        if (strip) {
            dl->AddRectFilled(a0, a1, sg.lut[0]);
            if (sg.avgInit) {
                sg.uploadAverage();
                const ImTextureID at = (ImTextureID)(intptr_t)sg.avgTex;
                if (vertical)  // low frequency on the left
                    dl->AddImageQuad(at, a0, ImVec2(a1.x, a0.y), a1, ImVec2(a0.x, a1.y), ImVec2(0, 1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1));
                else
                    dl->AddImage(at, a0, a1, ImVec2(0, 0), ImVec2(1, 1));
            }
            if (!compact) dl->AddRect(a0, a1, IM_COL32(80, 80, 90, 255));
            // Hover: average level at that frequency.
            ImGui::SetCursorScreenPos(a0);
            ImGui::InvisibleButton("##avgstrip", ImVec2(std::max(1.f, a1.x - a0.x), std::max(1.f, a1.y - a0.y)));
            if (ImGui::IsItemHovered() && sg.avgInit && sg.H > 0) {
                ImVec2 m = ImGui::GetIO().MousePos;
                float tf = vertical ? (m.x - a0.x) / (a1.x - a0.x) : 1.f - (m.y - a0.y) / (a1.y - a0.y);
                tf = std::clamp(tf, 0.f, 1.f);
                int row = std::clamp((int)((1 - tf) * sg.H), 0, sg.H - 1);
                double f = axis.freqAt(tf);
                char nb[32];
                noteName(f, nb, sizeof nb);
                ImGui::SetTooltip("Average (%.1f s)\n%.1f Hz  (%s)\n%.1f dB", s.avgSeconds, f, nb, sg.avgDb[row]);
            }
        }
        if (!compact) dl->AddRect(i0, i1, IM_COL32(80, 80, 90, 255));

        // Position helpers: tf in [0,1] low->high frequency, age in [0,1] newest->oldest.
        auto freqPos = [&](float tf) { return vertical ? ImVec2(i0.x + tf * w, 0) : ImVec2(0, i1.y - tf * h); };
        auto timePos = [&](float age) {
            if (orient == ORIENT_HORIZONTAL) return ImVec2(i1.x - age * w, 0);
            if (orient == ORIENT_WATERFALL) return ImVec2(0, i0.y + age * h);
            return ImVec2(0, i1.y - age * h);
        };
        const ImU32 txt = IM_COL32(190, 190, 200, 255), grid = IM_COL32(255, 255, 255, 28);
        const ImU32 shadowBg = IM_COL32(0, 0, 0, 140);
        auto label = [&](ImVec2 pos, const char* t) {
            if (compact) {
                ImVec2 ts = ImGui::CalcTextSize(t);
                dl->AddRectFilled(ImVec2(pos.x - 2 * S, pos.y), ImVec2(pos.x + ts.x + 2 * S, pos.y + ts.y), shadowBg, 2 * S);
            }
            dl->AddText(pos, txt, t);
        };

        // Frequency axis
        for (double f : freqTicks(axis, freqPx, (vertical ? 48 : 22) * S)) {
            const float tf = (float)axis.posOf(f);
            char b[16];
            fmtFreq(b, sizeof b, f);
            ImVec2 ts = ImGui::CalcTextSize(b);
            if (vertical) {
                float x = freqPos(tf).x;
                dl->AddLine(ImVec2(x, i0.y), ImVec2(x, i1.y), grid);
                if (compact) label(ImVec2(std::clamp(x - ts.x * 0.5f, i0.x + 2 * S, i1.x - ts.x - 2 * S), i1.y - ts.y - 2 * S), b);
                else {
                    const float fy = (strip && orient == ORIENT_REVERSE_WATERFALL) ? a1.y : i1.y;
                    dl->AddLine(ImVec2(x, fy), ImVec2(x, fy + 4 * S), txt);
                    label(ImVec2(std::clamp(x - ts.x * 0.5f, i0.x, i1.x - ts.x), fy + 4 * S), b);
                }
            } else {
                float y = freqPos(tf).y;
                dl->AddLine(ImVec2(i0.x, y), ImVec2(i1.x, y), grid);
                if (compact) {
                    if (styleButton && y < i0.y + 34 * S) continue;  // keep clear of the cog / Style buttons
                    label(ImVec2(i0.x + 4 * S, std::clamp(y - ts.y * 0.5f, i0.y, i1.y - ts.y)), b);
                }
                else {
                    dl->AddLine(ImVec2(i0.x - 4 * S, y), ImVec2(i0.x, y), txt);
                    label(ImVec2(i0.x - 6 * S - ts.x, std::clamp(y - ts.y * 0.5f, p0.y, i1.y - ts.y)), b);
                }
            }
        }

        // Time axis (hidden in compact mode)
        const double span = (double)sg.W * hop() / sampleRate;
        if (!compact) {
            double raw = span / std::max(2.0, timePx / ((vertical ? 40.0 : 90.0) * S));
            double mag = std::pow(10.0, std::floor(std::log10(raw))), step = mag;
            for (double m : {1.0, 2.0, 5.0, 10.0}) if (mag * m >= raw) { step = mag * m; break; }
            for (double t = 0; t <= span + 1e-9; t += step) {
                char b[32];
                if (t == 0) snprintf(b, sizeof b, "now");
                else if (step < 1) snprintf(b, sizeof b, "-%.*fs", step < 0.1 ? 2 : 1, t);
                else snprintf(b, sizeof b, "-%.0fs", t);
                ImVec2 ts = ImGui::CalcTextSize(b);
                ImVec2 tp = timePos((float)(t / span));
                if (vertical) {
                    dl->AddLine(ImVec2(i0.x - 4 * S, tp.y), ImVec2(i0.x, tp.y), txt);
                    label(ImVec2(i0.x - 6 * S - ts.x, std::clamp(tp.y - ts.y * 0.5f, i0.y, i1.y - ts.y)), b);
                } else {
                    dl->AddLine(ImVec2(tp.x, i1.y), ImVec2(tp.x, i1.y + 4 * S), txt);
                    label(ImVec2(std::clamp(tp.x - ts.x * 0.5f, i0.x, i1.x - ts.x), i1.y + 4 * S), b);
                }
            }
        }

        // Peak readout (top-right)
        if (peakDb > -150) {
            char nb[32], b[96];
            noteName(peakFreq, nb, sizeof nb);
            snprintf(b, sizeof b, "Peak %.1f Hz  %s  %.1f dB", peakFreq, nb, peakDb);
            ImVec2 ts = ImGui::CalcTextSize(b);
            if (ts.x + 20 * S < w - (styleButton ? 70 * S : 0)) {
                ImVec2 bp(i1.x - ts.x - 10 * S, i0.y + 6 * S);
                dl->AddRectFilled(ImVec2(bp.x - 5 * S, bp.y - 3 * S), ImVec2(bp.x + ts.x + 5 * S, bp.y + ts.y + 3 * S), IM_COL32(0, 0, 0, 150), 4 * S);
                dl->AddText(bp, IM_COL32(255, 255, 255, 230), b);
            }
        }
        if (paused)
            dl->AddText(ImVec2(i0.x + 8 * S, i0.y + (styleButton ? 30 : 6) * S), IM_COL32(255, 200, 80, 255), "PAUSED");

        // Screen position for (frequency 0..1 low->high, age 0..1 newest->oldest).
        auto screenAt = [&](float tf, float age) {
            if (orient == ORIENT_HORIZONTAL) return ImVec2(i1.x - age * w, i1.y - tf * h);
            if (orient == ORIENT_WATERFALL) return ImVec2(i0.x + tf * w, i0.y + age * h);
            return ImVec2(i0.x + tf * w, i1.y - age * h);
        };

        // Hover readout + peak tracking
        PeakTrack track = spectrumHoverTrack;  // from hovering the spectrum view (previous frame)
        bool sgHover = false;
        ImVec2 mpos;
        std::vector<ImVec2> ridgePts;
        ImVec2 trackPeakPt;
        bool hasTrackPeak = false;
        double trackPeakFreq = 0;
        spectrumHoverTrack.active = false;
        ImGui::SetCursorScreenPos(i0);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##sg", ImVec2(w, h));
        if (ImGui::IsItemHovered()) {
            ImVec2 m = ImGui::GetIO().MousePos;
            sgHover = true;
            mpos = m;
            if (s.magnifier && ImGui::GetIO().MouseWheel != 0)
                s.magZoom = std::clamp(s.magZoom * std::pow(1.25f, ImGui::GetIO().MouseWheel), 2.f, 16.f);
            float tx = std::clamp((m.x - i0.x) / w, 0.f, 1.f), ty = std::clamp((m.y - i0.y) / h, 0.f, 1.f);
            float tf, age;  // tf: 0 low -> 1 high; age: 0 newest -> 1 oldest
            if (orient == ORIENT_HORIZONTAL) { tf = 1 - ty; age = 1 - tx; }
            else if (orient == ORIENT_WATERFALL) { tf = tx; age = ty; }
            else { tf = tx; age = 1 - ty; }
            hoverTf = tf;
            int row = std::clamp((int)((1 - tf) * sg.H), 0, sg.H - 1);
            int k = std::clamp((int)((1 - age) * sg.W), 0, sg.W - 1);
            hoverK = k;
            lensHoverNext = {true, k, row};
            double f = axis.freqAt(tf);
            float d = cellDb(k, row);
            dl->AddLine(ImVec2(i0.x, m.y), ImVec2(i1.x, m.y), IM_COL32(255, 255, 255, 50));
            dl->AddLine(ImVec2(m.x, i0.y), ImVec2(m.x, i1.y), IM_COL32(255, 255, 255, 50));
            char nb[32], db[32];
            noteName(f, nb, sizeof nb);
            if (d > -250) snprintf(db, sizeof db, "%.0f dB", d);
            else snprintf(db, sizeof db, "-");
            const int pr = localPeakRow(k, row, std::max(2, (int)(6 * S * fbScale)));
            const float pd = cellDb(k, pr);
            if (s.magnifier) placeTooltipByMagnifier(m, 3);
            if (pd > s.dbMin + 3) {
                track = {true, k, pr};
                double pf = peakRowFreq(k, pr);
                char pn[32];
                noteName(pf, pn, sizeof pn);
                ImGui::SetTooltip("Dominant (Blue)  %.1f Hz  (%s)  %.1f dB\nCursor  %.1f Hz  %s\n-%.2f s", pf, pn, pd, f, db, age * span);
            } else {
                ImGui::SetTooltip("%.1f Hz  (%s)\n%s\n-%.2f s", f, nb, db, age * span);
            }
        }
        if (track.active && track.k < sg.W && track.row < sg.H) {
            traceRidge(track.k, track.row);
            std::vector<ImVec2>& pts = ridgePts;
            pts.reserve(ridge.size());
            for (auto& [k, r] : ridge) pts.push_back(screenAt(1.f - (r + 0.5f) / sg.H, 1.f - (k + 0.5f) / sg.W));
            trackPeakPt = screenAt(1.f - (track.row + 0.5f) / sg.H, 1.f - (track.k + 0.5f) / sg.W);
            hasTrackPeak = true;
            trackPeakFreq = peakRowFreq(track.k, track.row);
            // The tracking line is drawn only inside the magnifier (below).
        }
        if (sgHover && s.magnifier) {
            // Map a screen point inside the image to texture UV (u: time, wraps; v: 0 = high freq).
            auto uvAt = [&](ImVec2 p) {
                float tx = (p.x - i0.x) / w, ty = (p.y - i0.y) / h, tf, age;
                if (orient == ORIENT_HORIZONTAL) { tf = 1 - ty; age = 1 - tx; }
                else if (orient == ORIENT_WATERFALL) { tf = tx; age = ty; }
                else { tf = tx; age = 1 - ty; }
                return ImVec2(u0 + (1.f - age), 1.f - tf);
            };
            const float box = s.magSize * S, zoom = s.magZoom;
            const float half = box / (2 * zoom);
            // Source region centered on the cursor, kept inside the image.
            ImVec2 c(std::clamp(mpos.x, i0.x + std::min(half, w * 0.5f), i1.x - std::min(half, w * 0.5f)),
                     std::clamp(mpos.y, i0.y + std::min(half, h * 0.5f), i1.y - std::min(half, h * 0.5f)));
            const ImVec2 b0 = magBoxOrigin(mpos);
            const ImVec2 b1(b0.x + box, b0.y + box), bc(b0.x + box * 0.5f, b0.y + box * 0.5f);
            auto mag = [&](ImVec2 p) { return ImVec2(bc.x + (p.x - c.x) * zoom, bc.y + (p.y - c.y) * zoom); };

            ImDrawList* fl = ImGui::GetForegroundDrawList();
            fl->AddRectFilled(ImVec2(b0.x - 3 * S, b0.y - 3 * S), ImVec2(b1.x + 3 * S, b1.y + 3 * S), IM_COL32(0, 0, 0, 160), 4 * S);
            void* texData = (void*)(intptr_t)sg.tex;
            fl->AddCallback(glSetNearest, texData);
            fl->AddImageQuad((ImTextureID)(intptr_t)sg.tex, b0, ImVec2(b1.x, b0.y), b1, ImVec2(b0.x, b1.y),
                             uvAt(ImVec2(c.x - half, c.y - half)), uvAt(ImVec2(c.x + half, c.y - half)),
                             uvAt(ImVec2(c.x + half, c.y + half)), uvAt(ImVec2(c.x - half, c.y + half)));
            fl->AddCallback(glSetLinear, texData);
            fl->PushClipRect(b0, b1, true);
            if (lensDrawn)
                fl->AddImageQuad((ImTextureID)(intptr_t)lensTex, mag(lensQuad[0]), mag(lensQuad[1]), mag(lensQuad[2]), mag(lensQuad[3]),
                                 ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
            if (ridgePts.size() > 1) {
                std::vector<ImVec2> mp;
                mp.reserve(ridgePts.size());
                for (const ImVec2& p : ridgePts) mp.push_back(mag(p));
                fl->AddPolyline(mp.data(), (int)mp.size(), IM_COL32(0, 0, 0, 64), 0, 4.5f * S);
                fl->AddPolyline(mp.data(), (int)mp.size(), IM_COL32(90, 230, 255, 128), 0, 2.f * S);
            }
            if (hasTrackPeak) {
                const ImVec2 pc = mag(trackPeakPt);
                fl->AddCircle(pc, 6 * S, IM_COL32(0, 0, 0, 64), 0, 3.5f * S);
                fl->AddCircle(pc, 6 * S, IM_COL32(90, 230, 255, 128), 0, 1.8f * S);
            }
            // Mini frequency scale (left edge; bottom edge in vertical layouts) + dominant-frequency line.
            {
                const bool vert = orient != ORIENT_HORIZONTAL;
                auto tfAt = [&](ImVec2 p) { return vert ? (p.x - i0.x) / w : 1.f - (p.y - i0.y) / h; };
                const float ta = std::clamp(tfAt(ImVec2(c.x - half, c.y + half)), 0.f, 1.f);
                const float tb = std::clamp(tfAt(ImVec2(c.x + half, c.y - half)), 0.f, 1.f);
                const double fa = axis.freqAt(std::min(ta, tb)), fb = axis.freqAt(std::max(ta, tb));
                const float th = ImGui::GetTextLineHeight();
                const float bandW = 48 * S, bandH = th + 6 * S;
                const ImVec2 band0 = vert ? ImVec2(b0.x, b1.y - bandH) : b0;
                const ImVec2 band1 = vert ? b1 : ImVec2(b0.x + bandW, b1.y);
                // Where a frequency lands inside the magnifier.
                auto magOfFreq = [&](double f) {
                    const float tf = (float)axis.posOf(f);
                    return mag(vert ? ImVec2(i0.x + tf * w, c.y) : ImVec2(c.x, i1.y - tf * h));
                };
                auto fmtTick = [&](char* buf, size_t n, double f, double step) {
                    const int dec = std::max(0, -(int)std::floor(std::log10(step) + 1e-9));
                    if (f >= 1000) snprintf(buf, n, "%.*fk", std::clamp(3 - (int)std::floor(std::log10(step) + 1e-9), 0, 3), f / 1000);
                    else snprintf(buf, n, "%.*f", dec, f);
                };
                fl->AddRectFilled(band0, band1, IM_COL32(0, 0, 0, 140));
                const double range = fb - fa;
                double step = 1;
                if (range > 0) {
                    const double raw = range / (vert ? 3.0 : 4.0);
                    const double m10 = std::pow(10.0, std::floor(std::log10(raw)));
                    step = m10 * 10;
                    for (double mlt : {1.0, 2.0, 5.0, 10.0}) if (m10 * mlt >= raw) { step = m10 * mlt; break; }
                    const ImU32 tc = IM_COL32(215, 215, 222, 235);
                    for (double f = std::ceil(fa / step) * step; f <= fb + 1e-9; f += step) {
                        const ImVec2 q = magOfFreq(f);
                        char lb[24];
                        fmtTick(lb, sizeof lb, f, step);
                        const ImVec2 ts = ImGui::CalcTextSize(lb);
                        if (vert) {
                            if (q.x < b0.x || q.x > b1.x) continue;
                            fl->AddLine(ImVec2(q.x, band0.y), ImVec2(q.x, band0.y + 3 * S), tc, 1.f);
                            fl->AddText(ImVec2(std::clamp(q.x - ts.x * 0.5f, b0.x + 2 * S, b1.x - ts.x - 2 * S), band0.y + 3 * S), tc, lb);
                        } else {
                            if (q.y < b0.y || q.y > b1.y) continue;
                            fl->AddLine(ImVec2(band1.x - 4 * S, q.y), ImVec2(band1.x, q.y), tc, 1.f);
                            fl->AddText(ImVec2(band1.x - 6 * S - ts.x, std::clamp(q.y - ts.y * 0.5f, b0.y, b1.y - ts.y)), tc, lb);
                        }
                    }
                }
                if (hasTrackPeak) {
                    const ImVec2 q = mag(trackPeakPt);
                    const ImU32 blue = IM_COL32(90, 230, 255, 128);
                    char lb[24];
                    fmtTick(lb, sizeof lb, trackPeakFreq, std::max(0.1, step / 10));
                    const ImVec2 ts = ImGui::CalcTextSize(lb);
                    if (vert && q.x >= b0.x && q.x <= b1.x) {
                        fl->AddLine(ImVec2(q.x, b0.y), ImVec2(q.x, band0.y), blue, 1.5f * S);
                        const float lx = std::clamp(q.x - ts.x * 0.5f, b0.x + 2 * S, b1.x - ts.x - 2 * S);
                        fl->AddRectFilled(ImVec2(lx - 3 * S, band0.y + 1 * S), ImVec2(lx + ts.x + 3 * S, b1.y - 1 * S), IM_COL32(20, 70, 90, 235), 3 * S);
                        fl->AddText(ImVec2(lx, band0.y + 3 * S), IM_COL32(150, 240, 255, 255), lb);
                    } else if (!vert && q.y >= b0.y && q.y <= b1.y) {
                        fl->AddLine(ImVec2(band1.x, q.y), ImVec2(b1.x, q.y), blue, 1.5f * S);
                        const float ly = std::clamp(q.y - ts.y * 0.5f, b0.y + 1 * S, b1.y - ts.y - 1 * S);
                        fl->AddRectFilled(ImVec2(b0.x + 1 * S, ly - 1 * S), ImVec2(band1.x - 1 * S, ly + ts.y + 1 * S), IM_COL32(20, 70, 90, 235), 3 * S);
                        fl->AddText(ImVec2(band1.x - 6 * S - ts.x, ly), IM_COL32(150, 240, 255, 255), lb);
                    }
                }
            }
            ImVec2 mc = mag(mpos);
            const ImU32 ch = IM_COL32(255, 255, 255, 170);
            fl->AddLine(ImVec2(mc.x - 10 * S, mc.y), ImVec2(mc.x - 3 * S, mc.y), ch, 1.5f * S);
            fl->AddLine(ImVec2(mc.x + 3 * S, mc.y), ImVec2(mc.x + 10 * S, mc.y), ch, 1.5f * S);
            fl->AddLine(ImVec2(mc.x, mc.y - 10 * S), ImVec2(mc.x, mc.y - 3 * S), ch, 1.5f * S);
            fl->AddLine(ImVec2(mc.x, mc.y + 3 * S), ImVec2(mc.x, mc.y + 10 * S), ch, 1.5f * S);
            char zt[16];
            snprintf(zt, sizeof zt, "%.1f\xc3\x97", zoom);
            ImVec2 zs = ImGui::CalcTextSize(zt);
            fl->AddRectFilled(ImVec2(b1.x - zs.x - 8 * S, b0.y), ImVec2(b1.x, b0.y + zs.y + 4 * S), IM_COL32(0, 0, 0, 150));
            fl->AddText(ImVec2(b1.x - zs.x - 4 * S, b0.y + 2 * S), IM_COL32(230, 230, 235, 255), zt);
            fl->PopClipRect();
            fl->AddRect(b0, b1, IM_COL32(220, 220, 230, 200), 0, 0, 1.5f * S);
        }
        if (styleButton) {
            const float cs = ImGui::GetFrameHeight();
            if (cogButton("##cogview", ImVec2(i0.x + 6 * S, i0.y + 6 * S), cs, false)) togglePanel();
            drawStyleButton(ImVec2(i0.x + 12 * S + cs, i0.y + 6 * S + (cs - ImGui::GetFontSize()) * 0.5f));
        }
    }

    void drawSpectrum(ImVec2 p0, ImVec2 p1, float axisW) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float S = uiScale;
        ImVec2 i0(p0.x + axisW, p0.y), i1(p1.x, p1.y - 16 * S);
        float w = i1.x - i0.x, h = i1.y - i0.y;
        if (w < 4 || h < 4 || sg.H == 0) return;
        dl->AddRectFilled(i0, i1, IM_COL32(14, 14, 20, 255));
        const ImU32 txt = IM_COL32(190, 190, 200, 255), grid = IM_COL32(255, 255, 255, 28);
        auto yOf = [&](float d) { return i1.y - std::clamp((d - s.dbMin) / (s.dbMax - s.dbMin), 0.f, 1.f) * h; };
        // dB grid
        float range = s.dbMax - s.dbMin;
        float step = range > 120 ? 30.f : (range > 60 ? 20.f : 10.f);
        for (float d = std::ceil(s.dbMin / step) * step; d <= s.dbMax; d += step) {
            float y = yOf(d);
            dl->AddLine(ImVec2(i0.x, y), ImVec2(i1.x, y), grid);
            char b[16];
            snprintf(b, sizeof b, "%.0f", d);
            ImVec2 ts = ImGui::CalcTextSize(b);
            dl->AddText(ImVec2(i0.x - 6 * S - ts.x, std::clamp(y - ts.y * 0.5f, i0.y, i1.y - ts.y)), txt, b);
        }
        for (double f : freqTicks(axis, w, 48 * S)) {
            float x = i0.x + (float)axis.posOf(f) * w;
            dl->AddLine(ImVec2(x, i0.y), ImVec2(x, i1.y), grid);
            char b[16];
            fmtFreq(b, sizeof b, f);
            ImVec2 ts = ImGui::CalcTextSize(b);
            dl->AddText(ImVec2(std::clamp(x - ts.x * 0.5f, i0.x, i1.x - ts.x), i1.y + 2 * S), txt, b);
        }
        // Curves: rows run top (high f) to bottom (low f); x = 1 - row/H.
        const int n = sg.H;
        const int stride = std::max(1, n / std::max(1, (int)(w * fbScale)));
        std::vector<ImVec2> pts;
        pts.reserve(n / stride + 2);
        dl->PushClipRect(i0, i1, true);
        if (s.peakHold) {
            for (int r = n - 1; r >= 0; r -= stride)
                pts.push_back(ImVec2(i0.x + (1.f - (r + 0.5f) / n) * w, yOf(sg.hold[r])));
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 180, 60, 150), 0, 1.0f * S);
            pts.clear();
        }
        uint32_t accent = sg.lut[200];
        for (int r = n - 1; r >= 0; r -= stride) {
            float x = i0.x + (1.f - (r + 0.5f) / n) * w;
            float y = yOf(sg.latest[r]);
            pts.push_back(ImVec2(x, y));
            dl->AddLine(ImVec2(x, i1.y), ImVec2(x, y), (accent & 0x00ffffff) | 0x30000000);
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), accent, 0, 1.4f * S);
        if (s.showAverage && sg.avgInit) {
            pts.clear();
            for (int r = n - 1; r >= 0; r -= stride)
                pts.push_back(ImVec2(i0.x + (1.f - (r + 0.5f) / n) * w, yOf(sg.avgDb[r])));
            dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(110, 220, 255, 235), 0, 1.8f * S);
        }
        dl->PopClipRect();
        dl->AddRect(i0, i1, IM_COL32(80, 80, 90, 255));

        ImGui::SetCursorScreenPos(i0);
        ImGui::InvisibleButton("##spec", ImVec2(w, h));
        if (ImGui::IsItemHovered()) {
            ImVec2 m = ImGui::GetIO().MousePos;
            float tx = std::clamp((m.x - i0.x) / w, 0.f, 1.f);
            hoverTf = tx;
            hoverK = -1;
            int row = std::clamp((int)((1.f - tx) * n), 0, n - 1);
            double f = axis.freqAt(tx);
            char nb[32];
            noteName(f, nb, sizeof nb);
            dl->AddLine(ImVec2(m.x, i0.y), ImVec2(m.x, i1.y), IM_COL32(255, 255, 255, 50));
            const int kNew = sg.W - 1;
            const int pr = localPeakRow(kNew, row, std::max(2, (int)(6 * S * fbScale)));
            const float pd = sg.latest[pr];
            char avg[48] = "";
            if (s.showAverage && sg.avgInit) snprintf(avg, sizeof avg, "\navg %.1f dB", sg.avgDb[row]);
            if (pd > s.dbMin + 3) {
                spectrumHoverTrack = {true, kNew, pr};
                double pf = peakRowFreq(kNew, pr);
                char pn[32];
                noteName(pf, pn, sizeof pn);
                ImGui::SetTooltip("Dominant  %.1f Hz  (%s)  %.1f dB\nCursor  %.1f Hz  %.1f dB%s", pf, pn, pd, f, sg.latest[row], avg);
            } else {
                ImGui::SetTooltip("%.1f Hz  (%s)\n%.1f dB%s", f, nb, sg.latest[row], avg);
            }
        }
    }

    void drawWaveform(ImVec2 p0, ImVec2 p1, float axisW) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float S = uiScale;
        ImVec2 i0(p0.x + axisW, p0.y), i1 = p1;
        float w = i1.x - i0.x, h = i1.y - i0.y;
        if (w < 4 || h < 4) return;
        dl->AddRectFilled(i0, i1, IM_COL32(14, 14, 20, 255));
        const ImU32 txt = IM_COL32(190, 190, 200, 255), grid = IM_COL32(255, 255, 255, 28);
        const float mid = (i0.y + i1.y) * 0.5f, amp = h * 0.5f * s.waveGain;
        dl->AddLine(ImVec2(i0.x, mid), ImVec2(i1.x, mid), grid);
        {
            char b[16];
            float full = 1.f / s.waveGain;
            snprintf(b, sizeof b, full >= 0.1f ? "%+.2f" : "%+.3f", full);
            dl->AddText(ImVec2(i0.x - 6 * S - ImGui::CalcTextSize(b).x, i0.y), txt, b);
            snprintf(b, sizeof b, full >= 0.1f ? "%+.2f" : "%+.3f", -full);
            dl->AddText(ImVec2(i0.x - 6 * S - ImGui::CalcTextSize(b).x, i1.y - ImGui::GetTextLineHeight()), txt, b);
        }

        uint64_t N = (uint64_t)std::max(2.0, s.waveMs * 0.001 * sampleRate);
        N = std::min<uint64_t>(N, HIST / 4);
        uint64_t end = histTotal;
        if (s.trigger && histTotal > 3 * N) {
            // Find the most recent rising zero crossing such that a full window fits before 'now'.
            for (uint64_t i = histTotal - N; i > histTotal - 2 * N; --i) {
                float a = hist[(i - 1) & (HIST - 1)], b = hist[i & (HIST - 1)];
                if (a < 0.f && b >= 0.f) { end = i + N / 2; break; }
            }
        }
        const uint64_t start = end > N ? end - N : 0;
        const uint64_t count = end - start;
        uint32_t col = sg.lut[210];
        dl->PushClipRect(i0, i1, true);
        if (count > 1) {
            const int px = std::max(1, (int)w);
            if (count > (uint64_t)px * 2) {
                for (int x = 0; x < px; ++x) {
                    uint64_t a = start + count * x / px, b = start + count * (x + 1) / px;
                    float mn = 1e9f, mx = -1e9f;
                    for (uint64_t i = a; i < b; ++i) {
                        float v = hist[i & (HIST - 1)];
                        mn = std::min(mn, v);
                        mx = std::max(mx, v);
                    }
                    float y0 = mid - mx * amp, y1 = mid - mn * amp;
                    if (y1 - y0 < 1) y1 = y0 + 1;
                    dl->AddLine(ImVec2(i0.x + x + 0.5f, y0), ImVec2(i0.x + x + 0.5f, y1), col);
                }
            } else {
                std::vector<ImVec2> pts(count);
                for (uint64_t i = 0; i < count; ++i)
                    pts[i] = ImVec2(i0.x + (float)i / (count - 1) * w, mid - hist[(start + i) & (HIST - 1)] * amp);
                dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.3f * S);
            }
        }
        dl->PopClipRect();
        dl->AddRect(i0, i1, IM_COL32(80, 80, 90, 255));

        // Level meter readout over the last 50 ms.
        uint64_t mN = std::min<uint64_t>((uint64_t)(0.05 * sampleRate), histTotal);
        double sum = 0;
        float pk = 0;
        for (uint64_t i = histTotal - mN; i < histTotal; ++i) {
            float v = hist[i & (HIST - 1)];
            sum += (double)v * v;
            pk = std::max(pk, std::fabs(v));
        }
        double rmsDb = mN ? 10 * std::log10(sum / mN + 1e-20) : -200;
        double pkDb = 20 * std::log10(pk + 1e-10);
        char b[96];
        snprintf(b, sizeof b, "RMS %.1f dBFS   Peak %.1f dBFS   %.0f ms", std::max(rmsDb, -120.0), std::max(pkDb, -120.0), s.waveMs);
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(i1.x - ts.x - 8 * S, i0.y + 4 * S), pkDb > -0.1 ? IM_COL32(255, 90, 80, 255) : IM_COL32(200, 200, 210, 220), b);
    }

    void drawUI() {
        const float S = uiScale;
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) paused = !paused;
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) togglePanel();
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) sg.clear();
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 ws = vp->WorkSize;
        // Small windows: spectrogram only, edge to edge, with a Style dropdown.
        const bool compact = ws.y < 340 * S || ws.x < 340 * S;
        // The side panel needs room; hide it automatically in narrow windows.
        panelCanDock = !compact && ws.x >= 700 * S;
        const bool panel = s.showPanel && panelCanDock;
        if (panelCanDock) overlayOpen = false;

        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(ws);
        if (compact) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
        if (compact) ImGui::PopStyleVar();
        if (panel) {
            ImGui::BeginChild("panel", ImVec2(300 * S, 0), ImGuiChildFlags_Borders);
            ImGui::PushItemWidth(-90 * S);
            drawPanel();
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::SameLine();
        }
        ImGui::BeginChild("view", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        applyDisplaySettings();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float gap = 10 * S, axisW = 48 * S, axisH = 18 * S;
        // Auto layout follows the spectrogram panel's own shape: taller -> vertical flow,
        // wider -> horizontal. A small dead zone stops it flickering while resizing near square.
        auto chooseOrient = [&](float sw, float sh) {
            if (autoOrient == ORIENT_HORIZONTAL && sh > sw * 1.03f) autoOrient = ORIENT_REVERSE_WATERFALL;
            else if (autoOrient != ORIENT_HORIZONTAL && sw > sh * 1.03f) autoOrient = ORIENT_HORIZONTAL;
        };

        if (compact) {
            chooseOrient(avail.x, avail.y);
            drawSpectrogram(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), axisW, axisH, true, true);
            ImGui::EndChild();
            ImGui::End();
            drawOverlayPanel();
            endFrameHover();
            return;
        }

        float specH = s.showSpectrum ? s.spectrumHeight * S : 0;
        float waveH = s.showWaveform ? s.waveHeight * S : 0;
        float sgH = avail.y - specH - waveH - (s.showSpectrum ? gap : 0) - (s.showWaveform ? gap : 0);
        const float minSg = 120 * S;
        if (sgH < minSg) {
            // Shrink the lower panels if the window is too short.
            float excess = minSg - sgH;
            float tot = specH + waveH;
            if (tot > 0) {
                specH = std::max(30 * S, specH - excess * specH / tot);
                waveH = std::max(30 * S, waveH - excess * waveH / tot);
            }
            sgH = avail.y - specH - waveH - (s.showSpectrum ? gap : 0) - (s.showWaveform ? gap : 0);
        }
        // Draggable divider in the gap above a lower panel; dragging down shrinks that panel.
        auto splitter = [&](const char* id, float y, float& heightSetting, float* aboveSetting) {
            ImGui::SetCursorScreenPos(ImVec2(p0.x, y));
            ImGui::InvisibleButton(id, ImVec2(avail.x, gap));
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0) {
                float before = heightSetting;
                heightSetting = std::clamp(heightSetting - ImGui::GetIO().MouseDelta.y / S, 50.f, 1200.f);
                if (aboveSetting) *aboveSetting = std::clamp(*aboveSetting + (before - heightSetting), 50.f, 1200.f);
            }
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x + axisW + avail.x * 0.45f, y + gap * 0.5f - 1.5f * S),
                                                      ImVec2(p0.x + avail.x * 0.55f + axisW * 0.5f, y + gap * 0.5f + 1.5f * S),
                                                      hot ? IM_COL32(255, 255, 255, 120) : IM_COL32(255, 255, 255, 30), 2 * S);
        };
        chooseOrient(avail.x, sgH);
        float y = p0.y;
        drawSpectrogram(ImVec2(p0.x, y), ImVec2(p0.x + avail.x, y + sgH), axisW, axisH, false, !panel);
        y += sgH;
        if (s.showSpectrum) {
            splitter("##split1", y, s.spectrumHeight, nullptr);
            y += gap;
            drawSpectrum(ImVec2(p0.x, y), ImVec2(p0.x + avail.x, y + specH), axisW);
            y += specH;
        }
        if (s.showWaveform) {
            splitter("##split2", y, s.waveHeight, s.showSpectrum ? &s.spectrumHeight : nullptr);
            y += gap;
            drawWaveform(ImVec2(p0.x, y), ImVec2(p0.x + avail.x, y + waveH), axisW);
        }
        ImGui::EndChild();
        ImGui::End();
        drawOverlayPanel();
        endFrameHover();
    }

    // Floating controls panel for windows too small to dock it.
    void drawOverlayPanel() {
        if (panelCanDock || !overlayOpen) return;
        const float S = uiScale;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float m = 6 * S;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + m, vp->WorkPos.y + m));
        ImGui::SetNextWindowSize(ImVec2(std::min(300 * S, vp->WorkSize.x - 2 * m), vp->WorkSize.y - 2 * m));
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::Begin("##overlaypanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushItemWidth(-90 * S);
        drawPanel();
        ImGui::PopItemWidth();
        ImGui::End();
    }
};

// ---------------------------------------------------------------------------
static void glfwError(int code, const char* desc) { fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

static void setupStyle(float S) {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.ChildRounding = 6;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.PopupRounding = 4;
    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.09f, 0.11f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.13f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.19f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.26f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.24f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.24f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.34f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.32f, 0.42f, 1);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.30f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.38f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.95f, 0.55f, 0.35f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.65f, 0.45f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.55f, 0.35f, 1);
    st.ScaleAllSizes(S);
}

static void loadFont(float pixelSize) {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2026, 0x2026, 0x2190, 0x2193, 0};
    for (const char* path : candidates) {
        std::ifstream test(path, std::ios::binary);
        if (!test) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        if (io.Fonts->AddFontFromFileTTF(path, pixelSize, &cfg, ranges)) return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.f * pixelSize / 15.f);
    io.Fonts->AddFontDefault(&cfg);
}

int main(int, char**) {
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    App* app = new App();
    loadSettings(app->s);

    GLFWwindow* win = glfwCreateWindow(app->s.winW, app->s.winH, "Tonjogram", nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // DPI handling: macOS scales via the framebuffer; elsewhere scale UI by the monitor content scale.
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(win, &xs, &ys);
    int fbw, fbh, ww, wh;
    glfwGetFramebufferSize(win, &fbw, &fbh);
    glfwGetWindowSize(win, &ww, &wh);
    app->fbScale = ww > 0 ? (float)fbw / ww : 1.f;
#ifdef __APPLE__
    app->uiScale = 1.f;
    float rasterScale = std::max(1.f, app->fbScale);
#else
    app->uiScale = std::max(1.f, xs);
    float rasterScale = 1.f;
#endif
    setupStyle(app->uiScale);
    loadFont(15.f * app->uiScale * rasterScale);
    io.FontGlobalScale = 1.f / rasterScale;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    app->sg.setColormap(app->s.colormap);
    app->startPreferredSource();

    while (!glfwWindowShouldClose(win)) {
        if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.05);
            app->pump();
            continue;
        }
        glfwPollEvents();
        glfwGetFramebufferSize(win, &fbw, &fbh);
        glfwGetWindowSize(win, &ww, &wh);
        if (ww > 0) app->fbScale = (float)fbw / ww;

        app->pump();
        app->stepRerender(0.006);  // ~6 ms per frame for re-rendering history

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app->drawUI();
        ImGui::Render();

        glViewport(0, 0, fbw, fbh);
        glClearColor(0.06f, 0.06f, 0.08f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    glfwGetWindowSize(win, &app->s.winW, &app->s.winH);
    saveSettings(app->s);
    app->cap.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (app->sg.tex) glDeleteTextures(1, &app->sg.tex);
    if (app->sg.avgTex) glDeleteTextures(1, &app->sg.avgTex);
    glfwDestroyWindow(win);
    glfwTerminate();
    delete app;
    return 0;
}
