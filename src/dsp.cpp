#include "dsp.h"
#include <cmath>

static const double kPi = 3.14159265358979323846;

const char* kWindowNames[WIN_COUNT] = {"Rectangular", "Hann", "Hamming", "Blackman", "Blackman-Harris", "Flat-top"};

void RealFFT::init(int n) {
    if (n == n_) return;
    n_ = n;
    m_ = n / 2;
    int logm = 0;
    while ((1 << logm) < m_) ++logm;
    rev_.assign(m_, 0);
    for (int i = 0; i < m_; ++i) {
        int r = 0;
        for (int b = 0; b < logm; ++b) if (i & (1 << b)) r |= 1 << (logm - 1 - b);
        rev_[i] = r;
    }
    tw_.resize(m_ / 2 > 0 ? m_ / 2 : 1);
    for (int k = 0; k < m_ / 2; ++k) {
        double a = -2.0 * kPi * k / m_;
        tw_[k] = {(float)std::cos(a), (float)std::sin(a)};
    }
    twReal_.resize(m_ + 1);
    for (int k = 0; k <= m_; ++k) {
        double a = -2.0 * kPi * k / n_;
        twReal_[k] = {(float)std::cos(a), (float)std::sin(a)};
    }
    z_.resize(m_);
}

void RealFFT::forward(const float* in, std::complex<float>* out) {
    const int m = m_;
    // Pack even/odd samples into a complex sequence, in bit-reversed order.
    for (int i = 0; i < m; ++i) z_[rev_[i]] = {in[2 * i], in[2 * i + 1]};
    // Iterative radix-2 Cooley-Tukey.
    for (int len = 2; len <= m; len <<= 1) {
        const int half = len >> 1, step = m / len;
        for (int i = 0; i < m; i += len) {
            for (int j = 0; j < half; ++j) {
                const std::complex<float> w = tw_[j * step];
                const std::complex<float> u = z_[i + j];
                const std::complex<float> v = z_[i + j + half] * w;
                z_[i + j] = u + v;
                z_[i + j + half] = u - v;
            }
        }
    }
    // Split into the spectrum of the real input.
    for (int k = 0; k <= m; ++k) {
        const std::complex<float> zk = z_[k % m];
        const std::complex<float> zmk = std::conj(z_[(m - k) % m]);
        const std::complex<float> even = (zk + zmk) * 0.5f;
        const std::complex<float> odd = (zk - zmk) * std::complex<float>(0.f, -0.5f);
        out[k] = even + twReal_[k] * odd;
    }
}

void SpectrumAnalyzer::configure(int fftSize, int windowType) {
    if (fftSize == n_ && windowType == wtype_) return;
    n_ = fftSize;
    wtype_ = windowType;
    fft_.init(n_);
    window_.resize(n_);
    tmp_.resize(n_);
    spec_.resize(n_ / 2 + 1);
    db_.assign(n_ / 2 + 1, -200.f);
    double sum = 0;
    for (int i = 0; i < n_; ++i) {
        const double x = 2.0 * kPi * i / n_;  // periodic window
        double w = 1.0;
        switch (wtype_) {
            case WIN_HANN: w = 0.5 - 0.5 * std::cos(x); break;
            case WIN_HAMMING: w = 0.54 - 0.46 * std::cos(x); break;
            case WIN_BLACKMAN: w = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x); break;
            case WIN_BLACKMAN_HARRIS:
                w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
                break;
            case WIN_FLATTOP:
                w = 0.21557895 - 0.41663158 * std::cos(x) + 0.277263158 * std::cos(2 * x) -
                    0.083578947 * std::cos(3 * x) + 0.006947368 * std::cos(4 * x);
                break;
            default: break;
        }
        window_[i] = (float)w;
        sum += w;
    }
    scale_ = (float)(2.0 / sum);  // coherent gain normalization: full-scale sine -> 0 dBFS
}

void SpectrumAnalyzer::process(const float* frame) {
    for (int i = 0; i < n_; ++i) tmp_[i] = frame[i] * window_[i];
    fft_.forward(tmp_.data(), spec_.data());
    const int nb = n_ / 2 + 1;
    const float s2 = scale_ * scale_;
    for (int k = 0; k < nb; ++k) {
        const float p = std::norm(spec_[k]) * s2 + 1e-20f;
        db_[k] = 10.f * std::log10(p);
    }
}
