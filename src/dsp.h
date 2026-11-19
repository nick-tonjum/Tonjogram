#pragma once
#include <complex>
#include <vector>

enum WindowType { WIN_RECT = 0, WIN_HANN, WIN_HAMMING, WIN_BLACKMAN, WIN_BLACKMAN_HARRIS, WIN_FLATTOP, WIN_COUNT };
extern const char* kWindowNames[WIN_COUNT];

// Real-input FFT of size N (power of two) implemented as an N/2 complex FFT.
class RealFFT {
public:
    void init(int n);
    int size() const { return n_; }
    // in: n real samples. out: n/2+1 complex bins.
    void forward(const float* in, std::complex<float>* out);

private:
    int n_ = 0, m_ = 0;
    std::vector<int> rev_;
    std::vector<std::complex<float>> tw_;     // e^{-2pi i k / m}, k < m/2
    std::vector<std::complex<float>> twReal_; // e^{-2pi i k / n}, k <= m
    std::vector<std::complex<float>> z_;
};

// Windowed FFT -> magnitude spectrum in dBFS (a full-scale sine reads ~0 dB).
class SpectrumAnalyzer {
public:
    void configure(int fftSize, int windowType);
    int fftSize() const { return n_; }
    int windowType() const { return wtype_; }
    int numBins() const { return n_ / 2 + 1; }
    void process(const float* frame);              // frame has fftSize samples
    const std::vector<float>& db() const { return db_; }

private:
    int n_ = 0, wtype_ = -1;
    RealFFT fft_;
    std::vector<float> window_, tmp_, db_;
    std::vector<std::complex<float>> spec_;
    float scale_ = 1.f;
};
