# Tonjogram

A spectrogram application that records computer audio or a microphone input, with various quality of life features and visualization tweaks.

Tonjogram is a tiny real-time spectrogram, spectrum, and waveform viewer for **Windows, macOS, and Linux**. It runs as a single native file of about 1–2 MB, with no installer or runtime.

## Features

- **System audio capture**: see whatever your computer is playing, with no virtual audio driver needed. Microphones and line inputs also work.
- **Spectrogram** with FFT sizes from 256 to 65536, six window functions, and adjustable speed
- **Linear, log, or mel** frequency scales, frequency-range presets, an adjustable dB floor and ceiling, and six colormaps
- **Layouts**: horizontal, waterfall, or reverse waterfall. *Auto* picks one from the spectrogram panel's shape.
- **Hover lens**: around the cursor, every frequency uses its ideal FFT size (long for lows, short for highs), blended so that it keeps the sharpest detail of each and fades smoothly into the normal view
- **Magnifier** that follows the cursor, with a frequency scale and a marker for the dominant frequency. Scroll to zoom from 2× to 16×.
- **Dominant-frequency tracking**: shows the frequency, note, and level of the tone under the cursor and traces it through time. You can set the range and tightness.
- **Spectrum view** with peak hold and an **average** curve. The average also appears as a strip at the newest edge of the spectrogram.
- **Waveform** with zoom, gain, trigger, and RMS/peak meters
- When you change the FFT size, scale, or window, the history on screen is recomputed from stored audio
- **Simple / Advanced** control modes, a collapsible panel (the cog or `H`), and a compact mode for small windows
- Settings persist between runs

## Download & run

Download the file for your system from the [**Releases**](https://github.com/nick-tonjum/Tonjogram/releases) page.

| System | File | Steps |
|---|---|---|
| **Windows 10/11** | `Tonjogram-windows-x64.zip` | Unzip and double-click `Tonjogram.exe`. If SmartScreen says *"Windows protected your PC"*, click **More info → Run anyway** (one time only; the app isn't code-signed). |
| **macOS** (Apple Silicon & Intel) | `Tonjogram-macos-universal.zip` | Unzip and drag **Tonjogram.app** to Applications, then open it. The app isn't notarized, so macOS blocks the first launch. Go to **System Settings → Privacy & Security** and click **Open Anyway**. When asked, allow **System Audio Recording**. |
| **Linux x64** | `Tonjogram-linux-x64.tar.gz` | Extract and run `Tonjogram`. If double-clicking doesn't start it, right-click → *Run as program*, or run `./Tonjogram`. |

### System audio notes

- **Windows**: works out of the box through WASAPI loopback. You can also capture a specific output device ("Loopback: …").
- **macOS**: requires **macOS 14.2 or newer** for system audio (it uses Core Audio process taps). Microphone input works on older versions. If you denied the permission, enable Tonjogram under *System Settings → Privacy & Security → Screen & System Audio Recording*, then press **Restart** in the app.
- **Linux**: captures the default output through PulseAudio or PipeWire (`pipewire-pulse`), which nearly every desktop distribution ships. Monitor sources also appear as "Input: Monitor of …".

## Controls

| Action | How |
|---|---|
| Pause / resume | `Space` |
| Clear | `C` |
| Show / hide controls | `H` or the cog |
| Magnifier zoom | Scroll over the spectrogram |
| Resize views | Drag the gaps between them |

## Building from source

You need CMake 3.20+ and a C++17 compiler. Dependencies (GLFW, Dear ImGui, and miniaudio) are downloaded automatically.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Linux, install the X11/GL headers first:

```sh
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
```

GitHub Actions (`.github/workflows/build.yml`) builds all three platforms on every push. Pushing a tag such as `v1.0.0` publishes the downloads to a Release.

## Source layout

| File | Purpose |
|---|---|
| `src/main.cpp` | UI, spectrogram rendering, hover lens, magnifier, tracking, settings |
| `src/dsp.cpp` | Real FFT and window functions (calibrated to dBFS) |
| `src/audio_capture.cpp` | Device list and capture through miniaudio (WASAPI loopback, PulseAudio monitor, inputs) |
| `src/mac_system_tap.mm` | macOS system audio through a Core Audio process tap |
| `src/ring_buffer.h` | Lock-free buffer from the audio thread to the UI thread |

## License

Tonjogram is released under the [MIT License](LICENSE). You can use, modify, and redistribute it anywhere, including in commercial projects, as long as you keep the copyright notice:

> Copyright (c) 2026 Tonjum Technologies // Nick Tonjum

Tonjogram uses these third-party libraries, each under its own permissive license:

- [GLFW](https://www.glfw.org/) — zlib/libpng License
- [Dear ImGui](https://github.com/ocornut/imgui) — MIT License
- [miniaudio](https://miniaud.io/) — Public Domain (Unlicense) or MIT No Attribution
