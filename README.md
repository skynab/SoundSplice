# SoundSplice

A cross-platform **audio editor** written in C++, in the spirit of Audacity: record, import,
edit and process audio on a multi-track timeline, then export the result.

> SoundSplice started from Looper-Audio, a loop-centric DAW, and is being stripped down to a
> focused editor. Some of that heritage is still here — MIDI clips, a basic synth and the Session
> view are kept for now while the Session view is reviewed. Looper-Audio's `.looper` project files
> are not read; [`docs/PLAN.md`](docs/PLAN.md) is its original build plan, kept for reference.

## Features

- **Tracks** — audio tracks holding file-backed clips, and instrument tracks holding MIDI clips
  played by a built-in synth. Clips can be moved, resized, copied, duplicated and dragged between
  compatible tracks on a zoomable timeline.
- **Import and record** — drag audio files onto the timeline or import them from the file browser;
  record from an audio input (with count-in, metronome and input monitoring) or from a MIDI
  controller. MIDI files can be imported and exported.
- **Audio editing** — a waveform editor with cut, copy, paste, delete, trim to selection, split,
  silence, fade in/out, reverse, normalize, per-clip gain, speed and pitch changes, noise reduction
  (capture a noise print, then reduce), and applying an effect chain to a selection. Edits write a
  new file, so they are undoable.
- **Effects** — a per-track insert chain of built-in effects (filter, EQ, delay, reverb, drive,
  compressor, gate, tremolo, chorus, wobble) and hosted VST3/AudioUnit plugins, plus master
  filter, delay, reverb and EQ.
- **Mixing** — a mixer strip per track (gain, pan, mute, solo, meter), gain and pan automation
  lanes you can draw or record, and master gain automation.
- **Mastering and analysis** — a master-bus mastering rack (EQ, exciter, widener, reverb,
  maximizer) with presets, and a spectrum analyser for a selection.
- **Export** — WAV, AIFF, FLAC, Ogg Vorbis or MP3, as a full mix or as per-track stems.
- **Workspace** — dockable panes you can rearrange by dragging tabs, with built-in *Music Creation*
  and *Audio Editing* layouts, and undo/redo for every document edit.
- **Session view** — a clip-launching grid of scenes, kept for review.

Projects are saved as `.soundsplice` files.

## Tech stack

- **C++20**, **CMake** (≥ 3.24), dependencies via **CPM.cmake**
- **JUCE 8** for audio I/O, MIDI, plugin hosting and GUI
- **LAME** (built from source) for MP3 export
- **Catch2 v3** for unit tests
- CI on macOS, Windows and Linux via GitHub Actions

## Prerequisites

- A C++20 compiler: Xcode/AppleClang, MSVC 2022, or GCC/Clang on Linux
- CMake ≥ 3.24 and Git
- **Windows:** Visual Studio 2022 (or its Build Tools) with the C++ workload. It ships its own
  CMake if you don't have one on your `PATH`.
- **Linux only:** the JUCE system dependencies —
  ```bash
  sudo apt-get install -y libasound2-dev libjack-jackd2-dev ladspa-sdk \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
    libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
    libglu1-mesa-dev mesa-common-dev
  ```

## Build & run

```bash
# Configure (first run downloads JUCE, Catch2 and LAME via CPM — this takes a while)
cmake -S . -B build

# Build everything
cmake --build build --config Release --parallel

# Run the app — JUCE puts it under SoundSplice_artefacts/, not build/bin/
open build/src/app/SoundSplice_artefacts/Release/SoundSplice.app                          # macOS
# ./build/src/app/SoundSplice_artefacts/Release/SoundSplice.app/Contents/MacOS/SoundSplice  # macOS, attached to terminal
# ./build/src/app/SoundSplice_artefacts/Release/SoundSplice                                # Linux
# .\build\src\app\SoundSplice_artefacts\Release\SoundSplice.exe                            # Windows
```

(Replace `Release` with your build type if you configured a different one.)

CMake options, all `ON` by default:

| Option | Builds |
| --- | --- |
| `SOUNDSPLICE_BUILD_APP` | the application (pulls JUCE) |
| `SOUNDSPLICE_BUILD_TESTS` | the unit and GUI tests (pulls Catch2) |
| `SOUNDSPLICE_BUILD_TOOLS` | the headless `soundsplice_bounce` render/smoke-test tool (needs the app) |

## Tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

That runs `soundsplice_tests` (headless: real-time primitives, engine DSP, model, layout
geometry), `soundsplice_gui_tests` (constructs real JUCE components and checks they are parented,
laid out and wired) and `soundsplice_bounce` (renders audio offline and checks the result).

For a fast test-only loop that doesn't pull JUCE:

```bash
cmake -S . -B build-tests -DSOUNDSPLICE_BUILD_APP=OFF
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

**Windows: the build fails to write `SoundSplice.exe`.** The app is still running. Close it
and build again.

## Layout

```
src/rt/     Lock-free real-time primitives (no JUCE dependency)
src/engine/ Headless audio engine: transport, sequencer, tracks, effects, export, recording
src/model/  Project document: Song, Track, Clip, undo history, save/load (no JUCE)
src/app/    Application shell and UI panes
tools/      soundsplice_bounce — headless offline render and audio smoke test
tests/      Unit tests (rt, engine, model, app) and GUI tests
docs/       PLAN.md — the original Looper-Audio build plan, for reference
cmake/      CPM bootstrap and the LAME build
```

## License

MIT — see [`LICENSE`](LICENSE).
