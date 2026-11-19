#pragma once
#include <cstdint>

enum ColormapType { CMAP_MAGMA = 0, CMAP_INFERNO, CMAP_VIRIDIS, CMAP_TURBO, CMAP_CLASSIC, CMAP_GRAY, CMAP_COUNT };
static const char* kColormapNames[CMAP_COUNT] = {"Magma", "Inferno", "Viridis", "Turbo", "Classic", "Grayscale"};

// Fills lut[256] with packed RGBA (little-endian: R in low byte).
inline void buildColormap(int type, uint32_t lut[256]) {
    static const uint32_t magma[] = {0x000004, 0x1c1044, 0x4f127b, 0x812581, 0xb5367a, 0xe55064, 0xfb8761, 0xfec287, 0xfcfdbf};
    static const uint32_t inferno[] = {0x000004, 0x1f0c48, 0x550f6d, 0x88226a, 0xba3655, 0xe35933, 0xf98e09, 0xf9cb35, 0xfcffa4};
    static const uint32_t viridis[] = {0x440154, 0x472c7a, 0x3b518b, 0x2c718e, 0x21908d, 0x27ad81, 0x5cc863, 0xaadc32, 0xfde725};
    static const uint32_t turbo[] = {0x30123b, 0x4662d7, 0x36aaf9, 0x1ae4b6, 0x72fe5e, 0xc8ef34, 0xfaba39, 0xf66b19, 0xca2a04, 0x7a0403};
    static const uint32_t classic[] = {0x000000, 0x000060, 0x5000a0, 0xb00070, 0xff2000, 0xff9000, 0xffe040, 0xffffff};
    static const uint32_t gray[] = {0x000000, 0xffffff};
    const uint32_t* stops = magma;
    int n = 9;
    switch (type) {
        case CMAP_INFERNO: stops = inferno; n = 9; break;
        case CMAP_VIRIDIS: stops = viridis; n = 9; break;
        case CMAP_TURBO: stops = turbo; n = 10; break;
        case CMAP_CLASSIC: stops = classic; n = 8; break;
        case CMAP_GRAY: stops = gray; n = 2; break;
        default: break;
    }
    for (int i = 0; i < 256; ++i) {
        float t = i / 255.f * (n - 1);
        int a = (int)t;
        if (a >= n - 1) a = n - 2;
        float f = t - a;
        uint32_t c0 = stops[a], c1 = stops[a + 1];
        auto ch = [&](int shift) {
            float v0 = (float)((c0 >> shift) & 0xff), v1 = (float)((c1 >> shift) & 0xff);
            return (uint32_t)(v0 + (v1 - v0) * f + 0.5f);
        };
        uint32_t r = ch(16), g = ch(8), b = ch(0);
        lut[i] = r | (g << 8) | (b << 16) | 0xff000000u;
    }
}
