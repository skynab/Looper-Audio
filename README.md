# Looper-Audio

A cross-platform **loop-centric, AI-assisted DAW** written in C++ — for arranging and
generating music, in the spirit of FL Studio, Ableton Live, and Reason.

> **Status: Phase 4 (mixer + effects) + app shell.** On top of Phases 1–3 (multi-track sequenced
> synth + audio playback, transport/loop, a project document with undo/redo and `.looper` save/load,
> offline WAV bounce): a master **filter → delay → reverb** chain (all unit-tested) and **master-gain
> automation** — all saved with the project. The UI has a real app shell: a **menu bar** (File:
> New/Open/Save/Import/Bounce/Audio Settings; Edit: Undo/Redo/Clear) and a **resizable two-pane
> workspace** with a draggable divider, plus a **Mixer tab** — a channel strip per track (fader,
> level meter, mute, solo, click to select/arm) and a master strip carrying the effects chain. Solo
> follows the standard "solo overrides, mute always wins" rule, verified headlessly by the bounce
> tool. The Arrange tab is a real timeline: zoomable/scrollable (a `Viewport` over a content-sized
> `ArrangementView`) with **click-to-seek** on the ruler/lanes and **drag-to-reposition** clips — the
> beat↔pixel geometry is unit-tested headless. Dragging is now a real scheduling change, not just
> cosmetic: a track's clip start beat **delays when its pattern begins** (it plays and loops
> indefinitely from there — an arrangement-style "this part enters at bar N"), verified by the bounce
> tool. Each track also has a **send** (a "Send" slider on its mixer strip) into a shared **send
> bus** — a dedicated, always-fully-wet reverb every track can dip into pre-fader, independent of its
> own fader — with its own toggle/room/damping/return controls on the master strip, saved with the
> project and applied on export; verified by the bounce tool.
>
> The engine now genuinely supports **multiple clips per track**, each gating playback to its own
> [start, start+length) window (silence between clips, silence after the last one) — a real
> correctness fix, since the document model has always supported N clips per track but the engine
> silently only ever played the first. A track with exactly one clip still loops indefinitely from its
> start (today's validated "plays until Stop" behaviour, so every existing project is unaffected);
> real length gating only kicks in once a track has more than one clip. Verified by the bounce tool
> (silence in the gap between two clips and after the last one, each clip sounding only in its own
> window). That engine capability now has a UI: an **Add Clip** button in the Arrange tab adds a new
> clip to the selected track (positioned after the last one); clicking any clip in the timeline both
> arms its track and opens that specific clip in the piano roll (highlighted in the timeline, and named
> in a header above the piano roll — "Editing: Track X | Clip N of M" — so it's always clear which
> clip you're editing).
>
> While scoping audio **recording**, found and fixed another silent gap of the same shape: the
> document model has supported `TrackType::Audio` tracks with a `Clip.audioFile` since Phase 3a, but
> the engine only ever played back the single globally-loaded preview file ("Import Audio...") —
> per-track audio clips did nothing. Every pool slot now also owns an audio-clip player, summed into
> the exact same per-track gain/mute/solo/send pipeline synth content already goes through, with the
> same clip-start gating as MIDI clips (silent until its start beat, no looping — audio clips are
> one-shots, unlike patterns). Verified by the bounce tool: a decoded clip plays back non-silently,
> −6 dB halves its amplitude, and it's silent before its start beat and sounding after. That capability
> now has a UI too: **File > Import Audio to Track...** decodes a file onto a brand-new Audio track as
> its one clip, so — unlike the older "Import Audio..." preview, which only ever fed a single
> disconnected global player — it actually plays back as part of the mix, with its own gain/mute/solo/
> send on its mixer strip like any other track.
>
> **Microphone recording** now works, built on that same audio-track path: hit **Record** (requests
> up to 2 input channels from the device), it starts the transport and captures input into a
> pre-allocated take buffer (RT-safe hand-off — the audio thread is the only writer, the message
> thread only reads after the audio thread itself confirms the take is finished, so there's no window
> where both touch the buffer); hit **Record**/**Stop** again and the take is written to a WAV in
> `~/Documents/Looper-Audio Recordings/` and added as a new Audio track, ready to play back like any
> other. Verified as far as headlessly possible: fed synthetic input directly into the capture logic
> and confirmed exact sample-for-sample capture, correct armed/finished state transitions, and safe
> capping when a take exceeds its buffer (a 3-minute-per-take v1 limit — no disk streaming yet). What
> can't be verified without you: whether your Mac's actual microphone reaches the callback — that's
> the one thing left to try live.
>
> Automation isn't master-only anymore: the same **Rec Auto** toggle now also arms **per-track gain
> automation** — touch a mixer strip's fader instead of the master's while it's on, and that track
> gets its own automation lane (saved with the project, format v8). **Clr Auto** clears both the master
> lane and the currently selected track's. This reuses entirely proven machinery (the same
> `AutomationLane` class, the same `setTrackGainDb` the static fader already uses), so there's no new
> engine surface to verify — live playback works; **sample-accurate export of per-track automation is
> deliberately deferred** (doing it right means rendering each track in isolation before the shared
> send bus sums them, a bigger change than this pass warranted) — exporting a track with automation
> currently uses its static gain, a safe fallback rather than wrong audio. Deferred to validate live:
> recording itself, disk streaming.
>
> The app shell now has a real **dockable workspace** instead of a single fixed tab strip: three
> independent `DockRegion`s side by side (Files on its own, Arrange + Edit sharing one, Mixer in its
> own), each a self-contained tab group with its own click-to-select and drag-a-tab-header-onto-
> another-region-to-move-it behaviour (`src/app/DockRegion.h`), wired up by
> `MainComponent::movePanelBetweenRegions`. Out of the box, file management, arrangement, and mixer
> tools are all visible **at the same time** — the concrete complaint that motivated this — and any
> panel can be dragged into whichever region you'd rather have it in.
>
> That Files region hosts a new **file-management pane** (`src/app/FileBrowserPanel.h`): a
> `juce::FileTreeComponent` filtered to audio files, with Home/Recordings quick-access buttons.
> **Drag a file onto the arrangement** and it imports as a new audio track's clip starting at the
> beat you dropped it on; double-click previews it through the existing global preview player. Both
> the drop path and the file-dialog path (**File > Import Audio to Track...**) now funnel through one
> shared `MainComponent::importAudioFileAtBeat(file, startBeats)` — no new engine surface, just a new
> front door onto the same proven `AudioEngine`/`history_` machinery. `ArrangementView` tells a
> file-drag apart from a dock-panel-drag by the dragged component's *type* (a `FileTreeComponent`),
> not by any string, so the two drag protocols can't cross-talk even though they share one
> `DragAndDropContainer`.
>
> Both of these are a pure UI-shell addition — zero engine/model changes: all 50 unit tests pass
> unchanged and the bounce tool's full check suite, including the `rmsDry=0.149266` regression
> sentinel, is bit-for-bit identical. What can't be verified headlessly: the actual drag gestures
> (panel-to-panel and file-to-arrangement), the file tree's visual rendering, and whether a real
> audio-only `.m4a`/`.mp4` actually decodes via `CoreAudioFormat` on this machine — genuine video
> `.mp4` (video+audio muxed) is deliberately out of scope for now, since extracting its audio needs a
> demuxer this pass didn't add. Try both drags live, and see [`docs/PLAN.md`](docs/PLAN.md) for the
> full design writeups (including the alternatives considered and why).
>
> **Bounce/export now honours per-track gain automation, sample-accurately** — the deferred item
> from when per-track automation first landed. Unlike master-gain automation (already exported by a
> single post-render multiply, since it applies uniformly to the whole mix), per-track automation
> can't be bolted on after tracks are already summed — `OfflineRenderer::render()` gained an optional
> `GainAutomationFn` callback; when set, each track renders in isolation at unity gain and gets folded
> into the mix with a sample-accurate curve instead of one flat per-block gain. The callback is
> JUCE/model-independent (engine code still doesn't know what "automation" is — `MainComponent`
> supplies a lambda reading `AutomationLane::valueAt`), and is only wired up when a project actually
> has a per-track lane, so a project with none renders through the exact same untouched fast path as
> before — verified by the bounce tool's new `perTrackAutomationWorks` check (one track fades
> sample-accurately while an unautomated sibling stays stable in the same render) alongside every
> existing check, including `rmsDry=0.149266`, unchanged.
>
> **Audio tracks now genuinely support multiple clips**, closing the same "engine already modelled
> N clips, UI/engine wiring only ever used one" gap multi-clip MIDI closed earlier — except this time
> it was the *audio-clip player* that only ever played clip zero. `AudioFilePlayerNode` was rewritten
> around a clip-*list* (mirroring `Sequencer`'s `ClipList`, same lock-free swap pattern) instead of a
> single clip: each clip plays only within its own `[startBeats, startBeats+lengthBeats)` window —
> real gating, silence between clips — the instant a track has more than one; a track's sole clip
> still gets an unbounded window (today's "plays once from its start" behaviour, unaffected).
> `AudioEngine::setTrackAudioClips` replaces the old single-file API and **caches decoded audio by
> file path**, so re-submitting a track's whole clip list on every edit (the same unconditional
> pattern MIDI clips already used) never re-decodes a file it's already loaded — even a file shared
> across tracks. The feature is reachable, not just internal plumbing: dropping a file from the
> file-browser pane onto an *existing* audio track's lane now adds a clip there (sized to the file's
> real duration, not a fixed guess) instead of always creating a new track; dropping anywhere else
> still creates one, as before. Verified by a new `multiClipAudioGates` bounce-tool check (two audio
> clips on one track, each sounding only in its own window) alongside every existing check — including
> `rmsDry=0.149266` and `audioTrackWorks` (the original single-clip path), both unchanged, confirming
> the rewrite didn't disturb the case every existing project already relies on.
>
> **The send bus can now be delay, not just reverb** — a mixer strip **Reverb/Delay** selector
> (`sendEffectTypeBox_`) picks which always-fully-wet effect every track's send dips into; room/damp
> and delay-time/feedback share the same two slider slots, only one pair visible at a time depending
> on the selection. `AudioEngine` now holds a `sendBusDelay_` alongside the existing `sendBusReverb_`
> (both always prepared/configured; the audio callback just picks which one processes the bus each
> block) and `OfflineRenderer::render()` gained matching trailing params so bounce/export honours
> whichever is selected. `SendBusSettings` grew `effectType`/`delayTimeMs`/`delayFeedback` fields
> (format bumped to `LOOPER 9`) — both effects' params are always stored, so switching types never
> loses whichever one isn't currently active. Verified by a new `sendBusDelayWorks` bounce-tool check
> (the delay-routed bus differs from both "bus off" and the existing reverb-routed bus) alongside
> every existing check, including `rmsDry=0.149266`, unchanged.
>
> Next: persisting the dockable-workspace layout, and user-editable file-browser bookmarks. See the
> full roadmap in [`docs/PLAN.md`](docs/PLAN.md).

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

# Run the app — JUCE puts the GUI app under LooperAudio_artefacts/, not build/bin/
# (build/bin/ only holds console tools like looper_tests and looper_bounce).
open build/src/app/LooperAudio_artefacts/Release/Looper-Audio.app                     # macOS
# ./build/src/app/LooperAudio_artefacts/Release/Looper-Audio.app/Contents/MacOS/Looper-Audio  # macOS, attached to terminal
# ./build/src/app/LooperAudio_artefacts/Release/Looper-Audio                          # Linux
# .\build\src\app\LooperAudio_artefacts\Release\Looper-Audio.exe                      # Windows
```

(Replace `Release` with your build type if you configured a different one.)

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
src/engine/ Headless audio engine: transport, tempo map, nodes, sequencer, instrument tracks
src/model/  Project document: Song, Track, Clip, undo history, save/load (no JUCE)
src/app/    Application shell + engine-driven UI
tools/      Command-line tools (headless WAV bounce / audio smoke test)
tests/      Unit tests (rt + engine + model)
docs/       PLAN.md — the multi-year architecture & roadmap
cmake/      CPM bootstrap and build helpers
```

## License

MIT — see [`LICENSE`](LICENSE).
