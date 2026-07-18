# Looper-Audio

A cross-platform **loop-centric, AI-assisted DAW** written in C++ — for arranging and
generating music, in the spirit of FL Studio, Ableton Live, and Reason.

> **Status: Phase 2 complete.** On top of Phase 1 (engine, transport, WAV playback, metering):
> a 16-voice synth playing live MIDI (on-screen keyboard or external input) plus a step-grid
> piano roll whose pattern a sample-accurate `Sequencer` drives into the synth, looping in time.
> Next: Phase 3 (document model — tracks, clips, arrangement, undo/redo, save/load). See the full
> roadmap in [`docs/PLAN.md`](docs/PLAN.md).

## Tech stack

- **C++20**, **CMake** (≥ 3.24), dependencies via **CPM.cmake**
- **JUCE 8** for audio I/O, MIDI, plugin hosting, and GUI
- **Catch2 v3** for unit tests
- CI on macOS, Windows, and Linux via GitHub Actions

## Prerequisites

- A C++20 compiler (Xcode/AppleClang, MSVC 2022, or GCC/Clang on Linux)
- CMake ≥ 3.24 and Git
- **Linux only:** the JUCE system dependencies —
  ```bash
  sudo apt-get install -y libasound2-dev libjack-jackd2-dev ladspa-sdk \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
    libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
    libglu1-mesa-dev mesa-common-dev
  ```

## Build & run

```bash
# Configure (first run downloads JUCE + Catch2 via CPM — this takes a while)
cmake -S . -B build

# Build everything
cmake --build build --parallel

# Run the app
./build/bin/LooperAudio            # macOS/Linux
# .\build\bin\LooperAudio.exe      # Windows
```

On macOS the app is bundled — launch `build/LooperAudio_artefacts/<Config>/Looper-Audio.app`.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

For a fast test-only loop that doesn't pull JUCE:

```bash
cmake -S . -B build-tests -DLOOPER_BUILD_APP=OFF
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

## Troubleshooting

**macOS: `error: Please upgrade to Xcode 15.1 or higher`.** JUCE refuses to build with the
Xcode 15.0 toolchain because of a known linker bug in that exact release. Check your active
toolchain with `clang --version`; if it reports `clang-1500.0.x` you're on 15.0. Fix it by
pointing the build at any newer toolchain — in order of preference:

1. Upgrade `Xcode.app` (or select a newer one) — best for a project you'll ship.
2. If you already have newer **Command Line Tools** installed (check with
   `pkgutil --pkg-info=com.apple.pkg.CLTools_Executables`), use them:
   ```bash
   # one-off, per shell:
   export DEVELOPER_DIR=/Library/Developer/CommandLineTools
   # or make it the default (affects the whole machine):
   sudo xcode-select --switch /Library/Developer/CommandLineTools
   ```
   Re-run CMake from a **fresh** build directory afterwards so it re-detects the compiler.

## Layout

```
src/rt/     Lock-free real-time primitives (no JUCE dependency)
src/engine/ Headless audio engine: graph, transport, tempo map, nodes
src/app/    Application shell + engine-driven UI
tests/      Unit tests (rt + engine)
docs/       PLAN.md — the multi-year architecture & roadmap
cmake/      CPM bootstrap and build helpers
```

## License

MIT — see [`LICENSE`](LICENSE).
