# Looper-Audio — Build Plan

A cross-platform **Digital Audio Workstation (DAW)** in C++ for arranging and generating
music, in the spirit of FL Studio, Ableton Live, and Reason — with a **generative / AI-assist
layer** as its defining feature.

> **Direction chosen:** serious, shippable product · JUCE foundation · hybrid DAW + AI assist ·
> cross-platform (macOS, Windows, Linux) · multi-year phased roadmap.

This is a living document. As sections mature they should graduate into their own files
(`ARCHITECTURE.md`, `ROADMAP.md`, `AI.md`), and this file becomes the index.

---

## Table of contents

1. [Product vision & positioning](#1-product-vision--positioning)
2. [Scope & non-goals](#2-scope--non-goals)
3. [Technology stack](#3-technology-stack)
4. [High-level architecture](#4-high-level-architecture)
5. [The real-time audio engine](#5-the-real-time-audio-engine-the-heart)
6. [Document & data model](#6-document--data-model)
7. [Plugin hosting](#7-plugin-hosting)
8. [The UI layer](#8-the-ui-layer)
9. [Built-in instruments & effects](#9-built-in-instruments--effects)
10. [The AI / generative subsystem](#10-the-ai--generative-subsystem-the-differentiator)
11. [Cross-platform & packaging](#11-cross-platform--packaging)
12. [Testing & QA strategy](#12-testing--qa-strategy)
13. [Proposed project structure](#13-proposed-project-structure)
14. [Phased roadmap](#14-phased-roadmap)
15. [Key risks & mitigations](#15-key-risks--mitigations)
16. [Immediate next steps](#16-immediate-next-steps)
17. [Next planned features: MIDI I/O, file manager 2.0, piano-roll & drum tools](#17-next-planned-features-midi-io-file-manager-20-piano-roll--drum-tools)
18. [Next planned features: project-format versioning, metronome, editable clips & notes](#18-next-planned-features-project-format-versioning-metronome-editable-clips--notes)
19. [Session view: clip launching and scenes](#19-session-view-clip-launching-and-scenes)
20. [Plugin hosting](#20-plugin-hosting)
21. [A guitar instrument and its pane](#21-a-guitar-instrument-and-its-pane)
22. [Appendix: reference reading](#22-appendix-reference-reading)
23. [Guitar pedals](#23-guitar-pedals)
24. [Chord columns on the fretboard](#24-chord-columns-on-the-fretboard)
25. [Generative sound loops](#25-generative-sound-loops-first-slice-implemented)
26. [New synth sounds: filter envelope, sub-oscillator, unison](#26-new-synth-sounds-filter-envelope-sub-oscillator-unison-implemented)
27. [MIDI recording](#27-midi-recording-implemented)
28. [A starter song worth listening to](#28-a-starter-song-worth-listening-to-implemented)
29. [Tempo-aware audio clips](#29-tempo-aware-audio-clips-implemented)
30. [Sidechain compression](#30-sidechain-compression-implemented)
31. [Group buses](#31-group-buses-implemented)
32. [Automation you can see and draw](#32-automation-you-can-see-and-draw-implemented)
33. [Making the guitar sound better](#33-making-the-guitar-sound-better-implemented)
34. [A piano instrument](#34-a-piano-instrument-steps-1-4-implemented)

---

## 1. Product vision & positioning

**Looper-Audio** is a **loop-centric, AI-assisted DAW**. The name points at the identity: a
workflow built around clips and loops (like Ableton's Session View) rather than only a linear
tape timeline, with generative assistance that accelerates *ideation* while keeping every result
fully editable.

- **Who it's for:** producers who work loop-first — electronic, hip-hop, pop, lo-fi, scoring
  sketches — who want to move from idea to arrangement quickly.
- **The wedge (why it's different):** most DAWs treat AI as a bolt-on. Looper-Audio treats
  *generation* as a first-class citizen woven into the workflow: generate a melody in the current
  key/scale over the current chords, get four variations of a drum loop, extend a clip, fill
  harmony — all producing normal MIDI/audio clips you can edit by hand.
- **Guiding principles:**
  - **The human stays in control.** AI produces editable musical data, never an opaque black box in the signal path.
  - **Rock-solid real-time core.** Audio glitches are unforgivable; the engine's correctness and latency come before features.
  - **Loop-first, arrangement-ready.** Fast to jam, complete enough to finish a song.
  - **Cross-platform parity.** macOS and Windows first-class; Linux supported.

Competing feature-for-feature with 20-year-old incumbents is not the goal. Winning a *workflow*
(loop-centric + generative) is.

---

## 2. Scope & non-goals

**In scope (the product surface over the roadmap):**

- Multi-track audio + MIDI recording, editing, arranging
- Loop/clip launching (session view) **and** a linear arrangement timeline
- Piano roll, step sequencer, automation
- Mixer with routing, sends/returns, sidechaining
- Built-in instruments (synths, sampler, drums) and effects (EQ, dynamics, delay, reverb, …)
- Third-party plugin hosting: **VST3 + AU** first, **CLAP** later
- Generative/AI assistance: symbolic (MIDI) generation first, audio-domain generation later
- Offline render/export, stems, project save/load
- Signed, notarized installers per platform

**Non-goals (at least through 1.0):**

- Notation/score engraving (Sibelius/Dorico territory)
- Video editing (basic video-for-scoring reference at most, and only post-1.0)
- Hardware DSP / dedicated audio-interface driver development
- A cloud collaboration platform (interesting post-1.0, not core)
- Being a plugin *first* — Looper-Audio ships as an **application**; running the engine as a
  hostable plugin is a post-1.0 stretch goal.

---

## 3. Technology stack

| Concern | Choice | Notes |
|---|---|---|
| Language | **C++20** (consider C++23 where toolchains allow) | Concepts, `std::span`, `<atomic>` improvements, `constexpr` DSP tables. |
| App/audio framework | **JUCE 8** | Audio device I/O, MIDI, plugin hosting, GUI, cross-platform. Fastest credible path. |
| Build system | **CMake** (≥ 3.24) | Already implied by `.gitignore`. JUCE has first-class CMake support. |
| Dependencies | **vcpkg** (manifest mode) + **CPM.cmake** for JUCE | `.gitignore` already lists `vcpkg_installed/`. Pull JUCE via CPM or vcpkg. |
| Audio backends | CoreAudio (mac), WASAPI/ASIO (win), ALSA/JACK/PipeWire (linux) | All wrapped by JUCE's `AudioDeviceManager`. ASIO SDK has its own license. |
| Plugin formats hosted | VST3, AU (→ CLAP later) | VST3 SDK: GPLv3 or Steinberg proprietary agreement. AU is free on macOS. |
| DSP helpers | `juce::dsp`, plus targeted libs | Vectorized filters/FFT. Add specialist libs as needed (see below). |
| Time-stretch / pitch | **Rubber Band** or **SoundTouch** | Rubber Band is higher quality; check its dual GPL/commercial license. |
| Stem separation | **Demucs** (via ONNX/LibTorch) | Post-1.0 AI-adjacent feature. |
| ML inference (on-device) | **ONNX Runtime** (C++), and/or **llama.cpp/ggml** for transformer music-LMs | CPU + GPU execution providers; quantized models. |
| ML inference (cloud) | Pluggable service abstraction | For heavy audio-domain generation. Keep vendor-swappable. |
| Testing | **Catch2** or **GoogleTest**, plus **pluginval** | DSP unit tests, host/plugin validation. |
| CI | **GitHub Actions** matrix (mac/win/linux) | Build + test + artifact packaging. |
| Crash/telemetry | Sentry/Crashpad or Breakpad (opt-in) | Essential for a shipped desktop app. |

### Licensing budget (do not skip — this gates a commercial product)

- **JUCE (mid-2026):** dual-licensed **AGPLv3** *or* commercial. AGPL is unsuitable for a
  closed-source commercial DAW (network-copyleft), so plan on a commercial tier: **Personal**
  (free, under a revenue cap, adds a splash screen), **Indie ≈ $40/yr**, **Pro ≈ $800/yr**.
  *Verify current terms at [juce.com/get-juce](https://juce.com/get-juce/).*
- **VST3 SDK:** GPLv3 or a proprietary agreement with Steinberg.
- **ASIO SDK:** Steinberg license; can't be redistributed — users may use ASIO4ALL, or you sign the agreement.
- **AAX (Pro Tools):** Avid agreement + PACE signing — out of scope initially.
- **AI model weights:** licenses vary widely and **some forbid commercial use** (e.g., certain
  MusicGen weights are CC-BY-NC). Audit every model's license before shipping; prefer permissive/commercial-friendly weights.

---

## 4. High-level architecture

The single most important structural decision: **separate the real-time audio engine from the UI
and from the AI subsystem by hard boundaries.** They run on different threads and communicate only
through real-time-safe channels. Everything else follows from this.

```mermaid
flowchart TB
    subgraph UI["UI layer (message thread)"]
        AV[Arrangement view]
        SV[Session / clip view]
        PR[Piano roll]
        MX[Mixer]
        BR[Browser / library]
    end

    subgraph CORE["Core domain (message thread, non-RT)"]
        DOC[Document / Song model]
        UNDO[Undo/redo · command stack]
        SER[Serialization / project file]
    end

    subgraph AI["AI subsystem (worker threads)"]
        GENSYM[Symbolic generators - MIDI]
        GENAUD[Audio generators]
        RT[Model runtime: ONNX / llama.cpp / cloud]
    end

    subgraph ENGINE["Real-time engine (audio thread)"]
        GRAPH[Processing graph]
        TRANSPORT[Transport & tempo map]
        VOICES[Instruments / voices]
        FX[Effects & mixer DSP]
        PLUGS[Hosted plugins]
    end

    subgraph IO["I/O (worker threads)"]
        DISK[Disk streaming / recording]
        MIDIIO[MIDI I/O]
        DEV[Audio device manager]
    end

    UI <--> CORE
    CORE -- "RT-safe command queue" --> ENGINE
    ENGINE -- "lock-free FIFO: meters, events" --> UI
    AI -- "produces editable clips" --> CORE
    CORE --> AI
    DISK <--> ENGINE
    DEV --> ENGINE
    MIDIIO --> ENGINE
    PLUGS -.hosts.- ENGINE
```

**Layer responsibilities**

- **UI layer** — JUCE components. Renders state, captures intent. Never touches the audio thread directly.
- **Core domain** — the source of truth: the song/document model, undo/redo, serialization. Lives
  on the message thread; mutated only via commands.
- **Real-time engine** — the audio callback and everything it touches. Wait-free. Receives an
  immutable/RT-safe view of what to play; emits meters and events back.
- **AI subsystem** — off to the side. Consumes musical context, produces editable MIDI/audio that
  gets committed to the document like any user edit. **Never in the signal path.**
- **I/O** — disk streaming/recording, MIDI, device management, all on worker threads.

---

## 5. The real-time audio engine (the heart)

A DAW lives or dies here. The audio callback runs on a high-priority OS thread with a hard
deadline (e.g., at 48 kHz / 128-frame buffer you have **~2.7 ms** to produce every block). Miss it
and the user hears a click.

### The golden rules (non-negotiable on the audio thread)

- **No locks** (no mutexes, no priority inversion).
- **No allocation/deallocation** (`new`/`delete`/`malloc`/`free`, and no container growth).
- **No file, socket, or blocking syscalls.**
- **No exceptions thrown across the callback.**
- **No unbounded work** — every path is O(bounded) per block.
- **Allocate on the message thread, hand ownership to the audio thread via a queue; free back on
  the message thread.** The audio thread never constructs or destroys heap objects.

### Thread & communication model

```mermaid
flowchart LR
    MT["Message / UI thread<br/>(JUCE MessageManager)"]
    AT["Audio thread<br/>(device callback, real-time)"]
    WK["Worker pool<br/>(disk, waveforms, export, scan)"]
    AIW["AI worker(s)<br/>(inference)"]

    MT -- "SPSC command queue<br/>(add track, set param, swap graph)" --> AT
    AT -- "lock-free FIFO<br/>(meter levels, playhead, note events)" --> MT
    MT <--> WK
    MT <--> AIW
    WK -- "prefetched audio blocks<br/>(ring buffers)" --> AT
```

- **Message → audio:** a single-producer/single-consumer command queue. UI edits become commands
  (`SetParameter`, `AddNode`, `SwapGraphState`). Graph structural changes are done by building the
  new state on the message thread and atomically swapping a pointer (RCU-style); the old state is
  reclaimed later on the message thread.
- **Audio → message:** a lock-free FIFO for meter values, playhead position, and MIDI/automation
  events for display.
- **Parameters:** each automatable parameter is an atomic with a value-smoother on the audio side
  to avoid zipper noise.
- **A small, heavily-tested RT-primitives module** (`SpscQueue`, `LockFreeFifo`, `AtomicParam`,
  object pools) is the foundation everything else trusts. Build and test this first.

### Processing graph

- Nodes: tracks (audio/MIDI), instruments, effects, sends/returns, groups, master bus.
- Rendered in topological order per block; latency-compensated (**plugin/effect delay
  compensation, PDC**) so parallel paths stay phase-aligned.
- **Start with `juce::AudioProcessorGraph`** to move fast; expect to **evolve toward a custom
  graph** for finer control (parallel rendering across a thread pool, sample-accurate parameter
  changes, deterministic ordering). Design the node interface so the backing implementation can be
  swapped.

### Transport, time & sync

- Sample-accurate transport: play/stop/record, loop region, punch in/out.
- **Tempo map** (tempo + time-signature changes over the timeline) with musical position in PPQ;
  everything schedules against it.
- **Ableton Link** for tempo/beat sync with other apps and devices (core to a loop-centric DAW).
- MIDI clock / MTC as secondary sync options.

### Voices, mixing, and disk streaming

- Instruments use a **voice architecture** (voice pool, allocation/stealing, per-voice DSP).
- **Audio clips stream from disk** via background prefetch into ring buffers (never load whole
  files into RAM); recording writes through a background thread.
- Mixer DSP: gain/pan, sends, buses, sidechain routing — all block-based and branch-light.

---

## 6. Document & data model

The **Song/Document** is the authoritative, non-RT representation. The engine derives its RT state
from it; the UI renders it; the AI edits it.

- **Hierarchy:** `Song → Tracks → Clips → (MIDI notes / audio region + fades) `, plus `Buses`,
  `Sends`, `AutomationLanes`, `TempoMap`, `Markers`, `Scenes` (for session view).
- **Two timelines share one model:** an **arrangement timeline** (linear) and **session scenes**
  (clip grid). Clips are the common unit.
- **Undo/redo via the command pattern.** Every mutation is a reversible command; the command stack
  is the only way the document changes. This also gives a clean seam for scripting and AI edits
  (an AI result is just a command).
- **Serialization / project file:**
  - Human-diffable, forward-compatible container. Options: a documented JSON/XML manifest +
    binary blobs for audio, packaged in a project *bundle/folder* (like Ableton's `.als` in a
    project folder). Version every schema from day one.
  - Reference external audio by content hash; keep a "collect and save" to bundle assets.
- **IDs & references:** stable UUIDs for tracks/clips/params so automation, plugin state, and AI
  targets survive edits.

---

## 7. Plugin hosting

- **Formats:** **VST3** and **AU** (macOS) first — both hostable natively via JUCE. **CLAP** later:
  hosting is not native to JUCE yet (only alpha community modules such as `juce_clap_hosting`), and
  CLAP *authoring* lands in **JUCE 9**. **AAX** is out of scope initially.
- **Scanning:** scan/validate plugins **out-of-process** so a broken plugin can't crash the app
  during scan; cache a known-good plugin list.
- **Reliability:** third-party plugins crash. Start **in-process** for speed, but design the host
  boundary so a future **out-of-process / sandboxed** hosting mode can be dropped in (crash
  isolation is a real differentiator for stability).
- **State & automation:** persist plugin state blobs in the project; expose plugin parameters to
  the automation and modulation systems; manage plugin editor windows on the message thread.
- **Validation:** run **`pluginval`** in CI against anything we host or author.

---

## 8. The UI layer

Built with JUCE components, fully decoupled from the engine (renders document state + reads the
audio→UI FIFO for live meters/playhead).

**Primary views:**

- **Session view** — clip/scene grid, launch quantization, the "loop jamming" surface (identity view).
- **Arrangement view** — linear timeline, tracks, clips, automation lanes, ranges/markers.
- **Piano roll** — MIDI note editing, scales/chords overlay, quantize/humanize, and the entry
  point for symbolic AI generation.
- **Step sequencer** — drum/pattern programming.
- **Mixer** — channel strips, sends, routing, metering, plugin slots.
- **Browser/library** — instruments, effects, presets, samples, loops, and generated content.

**UI engineering notes:**

- Custom look-and-feel and a component library; GPU-accelerated rendering path where JUCE allows
  (JUCE 8's rendering improvements help with dense timelines/waveforms).
- Waveform/thumbnail rendering happens on worker threads and is cached.
- Keep the UI responsive by never blocking the message thread on disk/AI/plugin work.
- Plan for **accessibility** (JUCE accessibility API), theming/scaling (HiDPI), and full keyboard
  control early — retrofitting these is painful.

### Dockable workspace (implemented)

The original shell was a fixed layout: a transport sidebar plus a single `juce::TabbedComponent`
holding Arrange/Edit/Mixer as three tabs — only one of which could ever be visible, which made it
impossible to watch levels on the mixer while editing the arrangement. Three designs were
considered:

- **(A) Full custom drag-anywhere docking** (VS Code / Qt Advanced Docking System style) — most
  capable (arbitrary floating windows, live drop-zone previews, recursive splitting), but a large
  standalone engineering effort with real visual-polish risk.
- **(B) Generalize the existing split into an N-way region layout**, each region a tab group panels
  can be dragged between — chosen. Satisfies "use arrangement and mixer at once" with much less
  risk and no new dependency; the region layout is itself the foundation a fuller system (A) could
  later add floating/recursive-splitting on top of.
- **(C) A third-party JUCE docking library** — rejected for now; would need explicit sign-off given
  this project's existing licensing-budget discipline (§3) around new dependencies.

**What was built:** `src/app/DockRegion.h` — a self-contained tab-group component (custom
paint/mouse handling, not a `TabbedComponent` subclass, to keep full control of the drag gesture).
`MainComponent`'s workspace is now a **Files** region, `leftPane_` (transport, unchanged), and
**two more** `DockRegion`s side by side, separated by draggable dividers
(`juce::StretchableLayoutManager`, now 7 items). Default layout: Files owns its own region,
Arrange + Edit share region A, Mixer owns region B. Dragging a tab header onto another region
(`DragAndDropContainer`/`DragAndDropTarget`) moves that panel there via
`MainComponent::movePanelBetweenRegions`. **The layout now persists** across restarts: each of the
four known panels' current region, plus which panel is active within each region, is written to an
app-level `juce::PropertiesFile` (`~/Library/Application Support/Looper-Audio/` on macOS — separate
from the `.looper` project file, since this is a workstation preference, not song data) on every
panel move and on shutdown. Loading is defensive by construction rather than by validating a blob:
it asks each of the four hardcoded panel names "which region were you saved in," and silently leaves
a panel in its constructor-assigned default region if nothing (or something unrecognized) is found
— so a missing settings file, a stale value, or a future panel that didn't exist when it was saved
all fail safe rather than needing explicit corruption handling.

Verification: pure UI-shell change, zero engine/model impact — all unit tests and the bounce tool's
full check suite (including the `rmsDry=0.149266` regression sentinel) are unchanged. The drag
gesture, visual layout, and — for persistence specifically — actually restarting the app to confirm
a moved panel and the active tab come back where they were, could not be verified headlessly and
need a live try.

### File-management pane (implemented)

A left-side panel (`src/app/FileBrowserPanel.h`) to browse and drag audio files into the
arrangement, docking into the system above as its own default region (drag its tab elsewhere like
any other panel).

- **Browsing:** JUCE's built-in `juce::FileTreeComponent` (backed by `DirectoryContentsList` +
  `TimeSliceThread`), filtered to `*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3;*.m4a;*.mp4`. "Places" has
  two always-present buttons — **Home** and **Recordings** (bookmarking
  `MainComponent::recordingsDirectory()`) — plus a **user-editable bookmark list**: a **+** button
  opens a folder picker to add one, right-click a bookmark to remove it (`BookmarkButton`, a small
  `TextButton` subclass that also reports `ModifierKeys::isPopupMenu()` clicks). Bookmarks persist
  in the same app-level `juce::PropertiesFile` the dockable-workspace layout uses (joined with `\n`
  via `StringArray::joinIntoString`/`fromLines`, since folder paths won't contain literal newlines)
  — `FileBrowserPanel` only holds and displays the list; `MainComponent` owns saving/loading it,
  same separation of concerns as the dock layout above.
- **Drag-out:** implemented via type, not the drag's description string — `ArrangementView` (now
  also a `juce::DragAndDropTarget`) accepts a drag only when `SourceDetails::sourceComponent` is a
  `juce::FileTreeComponent`, reading the actual file back off it via `getSelectedFile(0)`, and
  computing the drop beat with the existing `TimelineGeometry::beatForX`. `DockRegion` explicitly
  rejects the same drags (by the same type check) so a file dropped on a region's tab strip doesn't
  get misinterpreted as a panel-move — JUCE resolves nested `DragAndDropTarget`s by walking up from
  the deepest hit component, so `ArrangementView` (nested inside a `DockRegion`) is asked first.
- **Backend — no new engine work:** dropping a file (or picking one via "Import Audio to
  Track...") both now funnel through one shared `MainComponent::importAudioFileAtBeat(file,
  startBeats)` — the same `AudioEngine`-track-creation + `history_.edit(...)` pattern this project
  already had, just parameterized on the start beat (0 for the dialog, the drop position for a
  drag) instead of duplicated.
- **Preview/audition:** double-clicking a file in the browser calls the same
  `MainComponent::previewAudioFile(file)` helper "File > Import Audio..." already used (also
  de-duplicated out of that menu action's callback).
- **Format-scope decision, as planned:** the filter includes `.mp4`/`.m4a`, which JUCE's
  `CoreAudioFormat` can decode when the container is audio-only (e.g. AAC in an M4A/MP4 box) — no
  new code needed for that case. Genuine video `.mp4` files will simply fail to decode
  (`AudioEngine::loadAudioFileForTrack` returns false, same as any unsupported file) since no
  demuxer was added — extracting audio from real video remains an explicitly separate, harder
  follow-up (AVFoundation/`AVAssetReader` on Apple, or a cross-platform demuxer dependency).
- Additive only, unchanged: **File > Import Audio to Track...** and **File > Import Audio...**
  still work exactly as before.

Verification: builds clean, all 50 unit tests and the bounce tool's full check suite (including
`rmsDry=0.149266`) are unchanged — this only added a new front door onto already-verified
`AudioEngine`/`history_` machinery. What can't be verified headlessly: the actual drag gesture, the
file tree rendering, and whether a real audio-only `.m4a`/`.mp4` decodes via `CoreAudioFormat` on
this machine — try dragging a `.wav` and an `.m4a` from the panel into the arrangement live.

---

## 9. Built-in instruments & effects

A DAW needs a credible factory set so it's usable without third-party plugins.

- **Reusable DSP core** (`dsp/`): oscillators (band-limited), filters (SVF, ladder), envelopes,
  LFOs, FFT, oversampling, saturation, delay lines, interpolation. Unit-tested against reference.
- **Instruments:**
  - Subtractive/wavetable **synth** (poly voice engine).
  - **Sampler** with disk streaming, zones/velocity layers, round-robin.
  - **Drum machine** / kit sampler tied to the step sequencer.
- **Effects suite:** EQ, compressor/limiter, gate, delay, reverb (algorithmic + convolution),
  chorus/flanger/phaser, distortion/saturation, stereo tools, utility/gain.
- **Modulation:** a matrix routing LFOs/envelopes/macros to parameters (host-side, so it works for
  built-ins *and* hosted plugins).
- Ship **factory presets and a loop/sample library** as installable content packs.

---

## 10. The AI / generative subsystem (the differentiator)

The design rule that keeps this sane: **AI is an asynchronous generator that produces editable
musical data — it is never in the real-time signal path.** Generation runs on worker threads; a
result becomes a normal clip/pattern committed to the document via a command (so it's undoable and
hand-editable).

### Two domains, sequenced deliberately

1. **Symbolic (MIDI) generation — build this first.** Highest value per unit effort: output is
   MIDI you can edit, it's cheap enough to run on-device, and it slots straight into the piano roll
   and step sequencer.
   - *Non-ML first:* music-theory helpers — scale/chord constraint, voice-leading, arpeggiation,
     humanization, Markov/grammar-based pattern variation. Deterministic, instant, no model weights,
     genuinely useful.
   - *ML next:* transformer music-LMs for melody/continuation/accompaniment (e.g. anticipatory /
     infilling models), drum-pattern models. Run via **ONNX Runtime** or a quantized model through
     **llama.cpp/ggml**.
   - *Use cases:* "generate a melody in this key/scale over these chords," "4 variations of this
     drum loop," "extend this clip," "suggest a bassline," "fill harmony," "humanize timing/velocity."

2. **Audio-domain generation — later, and mostly cloud.** Text-to-loop, one-shot/sample
   generation, and stem work.
   - Open models exist (MusicGen / AudioCraft, Stable Audio Open) but are **heavy**; good quality
     on-device needs a GPU. Realistic path: a **pluggable inference service** (self-hosted or
     third-party) for the heavy lifting, with on-device for smaller tasks.
   - *AI-adjacent DSP that's high value and cheaper:* **stem separation** (Demucs), **tempo/key
     detection**, **smart quantize/warp**, **pitch correction** — deliver these alongside generation.

### Runtime & integration

```mermaid
flowchart LR
    CTX["Musical context<br/>(key, scale, chords,<br/>surrounding clips, tempo)"]
    --> GEN["Generator<br/>(symbolic / audio)"]
    GEN --> RUNTIME{"Model runtime"}
    RUNTIME -->|on-device| ONNX["ONNX Runtime / llama.cpp"]
    RUNTIME -->|heavy| CLOUD["Cloud inference service"]
    ONNX --> RESULT["Result → editable clip"]
    CLOUD --> RESULT
    RESULT --> CMD["Commit as undoable command"]
    CMD --> DOC["Document"]
```

- **Model runtime abstraction:** one interface, multiple backends (on-device ONNX, on-device
  llama.cpp/ggml, remote service). Backends are swappable and testable in isolation.
- **Context builder:** gathers the musical context (key/scale/chords, neighboring clips, groove,
  tempo) so generation is *conditioned on the project*, not generic.
- **UX patterns:** contextual "Generate" actions in the piano roll / step sequencer / browser;
  always return **multiple candidates**; everything lands as editable MIDI/audio; nothing is
  destructive.
- **Licensing & privacy:** audit each model's weight license (some forbid commercial use); make
  cloud generation opt-in and transparent about what leaves the machine.

---

## 11. Cross-platform & packaging

| Platform | Audio backends | Packaging | Signing |
|---|---|---|---|
| **macOS** | CoreAudio | universal binary (arm64 + x86_64), `.dmg`/`.pkg` | Developer ID + **notarization** |
| **Windows** | WASAPI (shared/exclusive), ASIO | MSVC build, installer (WiX / Inno / NSIS) | Authenticode (EV cert) |
| **Linux** | ALSA, JACK, PipeWire | AppImage / Flatpak / `.deb` | — (community-supported tier) |

- **Priority:** macOS + Windows first-class from day one (both in CI); Linux supported and
  community-tested.
- JUCE abstracts the audio/MIDI/GUI differences; the work is in **build/signing/packaging** and
  device-specific QA (driver/buffer edge cases).
- Automate installer builds in CI so every commit can produce artifacts.

---

## 12. Testing & QA strategy

- **DSP unit tests** (Catch2/GoogleTest): impulse/step responses, frequency response, gain
  staging, null tests against reference renders within tolerance.
- **RT-primitives tests:** hammer the lock-free queues/pools under contention; run under
  **ThreadSanitizer** and **UBSan/ASan** (note: TSan + real-time audio is delicate — test the
  primitives standalone).
- **Audio regression:** render a fixed project to WAV, compare against a golden file within
  tolerance; fail CI on drift.
- **Plugin validation:** run **`pluginval`** against hosted/authored plugins.
- **CI matrix:** GitHub Actions on macOS/Windows/Linux — build, test, package artifacts.
- **Real-time safety discipline:** a lightweight "is this called on the audio thread?" assertion in
  debug builds catches accidental locks/allocations early.
- **Performance budgets:** track CPU per voice/effect and round-trip latency; regressions are bugs.

---

## 13. Proposed project structure

```
Looper-Audio/
├─ CMakeLists.txt
├─ vcpkg.json                 # manifest: catch2, onnxruntime, rubberband, …
├─ cmake/                     # toolchain files, CPM.cmake, JUCE helpers, signing
├─ docs/
│  ├─ PLAN.md                 # this document
│  ├─ ARCHITECTURE.md         # (graduates out of §4–§7)
│  └─ AI.md                   # (graduates out of §10)
├─ src/
│  ├─ rt/                     # lock-free primitives, object pools, RT assertions  ← build first
│  ├─ core/                   # Song/document model, commands, undo, serialization
│  ├─ engine/                 # real-time graph, transport, mixer, voices
│  ├─ dsp/                    # reusable DSP building blocks (tested)
│  ├─ instruments/            # synth, sampler, drums
│  ├─ effects/                # eq, dynamics, delay, reverb, …
│  ├─ plugins/                # VST3/AU (later CLAP) hosting + scanning
│  ├─ ai/                     # runtime abstraction, symbolic + audio generators
│  ├─ ui/                     # JUCE components: session, arrangement, piano roll, mixer, browser
│  └─ app/                    # application shell, main(), windows, command wiring
├─ modules/                   # vendored JUCE modules / third-party
├─ tests/                     # unit + integration + regression
├─ tools/                     # asset pipeline, out-of-process plugin scanner, benchmarks
└─ resources/                 # icons, fonts, factory presets, loops
```

Design each layer to depend only *downward* (`ui → core → engine → dsp/rt`), so the engine is
testable headless and the UI never reaches into real-time code.

---

## 14. Phased roadmap

Phased so that **every phase ends in something you can run and hear.** No calendar dates — a
serious DAW is a multi-year effort; these are ordered milestones, not deadlines. Each phase is a
vertical slice that de-risks the next.

| Phase | Theme | Definition of done |
|---|---|---|
| **0** | **Foundations** | CMake + vcpkg + JUCE build on mac/win/linux; CI green; app window opens; audio callback emits a test tone; **`rt/` lock-free primitives written and tested**; logging + crash reporting. |
| **1** | **Engine & transport (vertical slice)** | Processing graph + master bus; transport (play/stop/loop); tempo/time-sig map; play a WAV through the graph in time; metering to the UI. Proves the architecture end-to-end. |
| **2** | **MIDI + first instrument** | MIDI I/O; polyphonic synth voice engine; note scheduling; minimal piano roll. Hear MIDI drive a built-in synth on the grid. |
| **3** | **Document, tracks, clips, arrangement** | Full song model; audio+MIDI tracks; clips on a timeline; arrangement view; **undo/redo**; save/load project; audio recording; disk streaming. |
| **4** | **Mixer, routing, effects, automation** | Mixer with sends/returns/sidechain; built-in effects suite; automation lanes; offline bounce/export to WAV. |
| **5** | **Plugin hosting** | VST3 + AU hosting; out-of-process scanning; plugin windows; plugin state + automation. |
| **6** | **The "Looper" identity** | Session/clip launching; scenes; loop-centric workflow; **Ableton Link**; warping/time-stretch (Rubber Band); step sequencer. Product identity crystallizes. |
| **7** | **AI / generative layer** | Model-runtime abstraction; **symbolic MIDI generation first** (melody/chords/drums, variation/continuation, humanize) + music-theory helpers; then audio-domain (cloud text-to-loop, stem separation, tempo/key detection). All async, all editable. |
| **8** | **Polish & release engineering** | CPU/latency optimization (parallel graph rendering); accessibility; factory content; onboarding; signed/notarized installers; licensing/activation; docs; **beta → 1.0**. |
| **post-1.0** | **Beyond** | Ship the engine as a hostable plugin (VST3/AU/CLAP); collaboration/cloud sync; MIDI 2.0 / MPE; mobile companion; marketplace. |

---

## 15. Key risks & mitigations

| Risk | Why it's serious | Mitigation |
|---|---|---|
| **Sheer scope** | Commercial DAWs represent *hundreds* of person-years. This is the #1 killer. | Ruthless MVP; ship vertical slices; win a workflow (loop + AI), don't clone incumbents; consider a narrow, opinionated feature set. |
| **Real-time correctness** | Lock-free bugs are subtle, rare, and platform-specific; glitches are unforgivable. | Small tested `rt/` layer; the "allocate on message thread" rule; TSan on primitives; audio-thread assertions in debug. |
| **AI cost / latency / quality** | Good audio generation is heavy; cloud adds cost, latency, privacy concerns. | Lead with symbolic MIDI (cheap, editable, high value); make audio gen optional/cloud; pluggable backends. |
| **Licensing** | JUCE AGPL unsuitable for closed source; SDK agreements; **some model weights forbid commercial use**. | Budget JUCE Indie/Pro; sign VST3/ASIO agreements as needed; audit every model license before shipping. |
| **Cross-platform QA** | 3 OSes × many audio devices/drivers/buffer sizes. | CI matrix; mac+win first-class, Linux community tier; automate packaging. |
| **Plugin stability** | Third-party plugins crash and take the app down. | Out-of-process scanning now; design for out-of-process hosting later. |
| **Solo/small-team bandwidth** | Burnout and stall on a years-long build. | Keep each phase independently runnable/demoable; the loop-jam workflow is usable well before "full DAW." |

---

## 16. Immediate next steps

Concrete, in order — the first three get you to *hearing sound through your own code*:

1. **Scaffold the build.** `CMakeLists.txt` + `vcpkg.json`, pull **JUCE via CPM**, produce an empty
   JUCE app window that builds and runs on your Mac.
2. **Wire CI.** GitHub Actions matrix (macOS + Windows + Linux) building the scaffold; artifacts on green.
3. **Vertical slice — make noise.** Audio device callback → **test tone** → metering to the UI; then
   play a WAV through a minimal graph with a working transport (Phase 0 → into Phase 1).
4. **Build `rt/` first.** The lock-free SPSC command queue, audio→UI FIFO, object pool, and the
   audio-thread assertion — with tests. Everything trusts this layer.
5. **Write the threading contract** into `docs/ARCHITECTURE.md`: exactly what may/may not happen on
   each thread, and how message↔audio communication works. This document prevents the class of bug
   that's hardest to fix later.

---

## 17. Next planned features: MIDI I/O, file manager 2.0, piano-roll & drum tools

Four features requested together, each independent enough to build and verify on its own. Suggested
build order — smallest/lowest-risk first, and drum kits last since it genuinely benefits from two of
the others already existing:

1. Piano-roll key-name gutter (smallest, standalone).
2. MIDI import/export (standalone).
3. File manager 2.0 (standalone; also lays down the drag-onto-a-pad infrastructure drum kits reuse).
4. Drum kits (largest; reuses #1's gutter pattern and #3's drag-and-drop).

Each subsection below flags the judgment calls made so they're visible before implementation starts,
rather than buried in code.

### MIDI import/export (implemented)

Read and write Standard MIDI Files (`.mid`) so patterns can come from, or go to, other tools —
no new dependency: `juce::MidiFile` (already-linked `juce_audio_basics`) parses/writes SMF headers,
per-track `juce::MidiMessageSequence`s, and tempo/time-signature meta-events.

**What was built:** `src/engine/MidiFileIO.h` — engine layer, not model/, since it needs JUCE (same
reasoning that already puts `OfflineRenderer.h` in engine/ despite needing `juce_audio_formats`).
Two entry points, both JUCE-dependent, both operating on `model::Song`:

- `MidiImportResult importMidiFile(const juce::File&, model::Song&)` — one new `Instrument` track
  per imported MIDI track with at least one note (a tempo-only or otherwise note-less track imports
  nothing); each track's note on/off pairs (matched via JUCE's own
  `MidiMessageSequence::updateMatchedPairs`/`noteOffObject`) become `engine::Note{startBeats,
  lengthBeats, noteNumber, velocity}` via tick→beat conversion using `MidiFile::getTimeFormat()`
  (ticks per quarter note). Each imported track gets one clip spanning its whole content,
  `startBeats = 0`, the existing "single clip = unbounded, plays until stop" convention rather than
  multi-clip splitting something that doesn't need it. `MidiImportResult` reports
  `tracksImported`/`extraTempoEventsIgnored` rather than a bare bool, so the UI can say exactly
  what happened.
- `bool exportMidiFile(const juce::File&, const model::Song&)` — the reverse: one
  `MidiMessageSequence` per Instrument track, flattening *all* of that track's clips onto one
  continuous sequence at their timeline positions (each clip's `startBeats` becomes a tick offset,
  at a fixed 960-ticks-per-quarter-note resolution), plus one tempo meta-event from `song.bpm`.
  Audio tracks have nothing to export and are skipped; a track with zero notes across all its clips
  is skipped too, so the file only contains tracks that actually have content.

**Scope call — no tempo map, as planned:** the engine has one global `song.bpm`, not a
tempo-map-over-time. Import uses the file's *first* tempo event for `song.bpm` (every event after
that increments `extraTempoEventsIgnored` instead of being silently dropped or misapplied); a file
with no tempo event at all leaves the song's existing BPM untouched. SMPTE-based time formats
(`MidiFile::getTimeFormat() <= 0`) are rejected outright rather than misinterpreted.

**UI:** File > Import MIDI... / File > Export MIDI..., built the same way Import Audio / Bounce
already are (`juce::FileChooser`, `history_.edit(...)` wrapping the import mutation since it's a
real document change; export doesn't touch the document, so it isn't). Import's status message
reports the tempo actually used and any ignored tempo changes, e.g. "Imported 2 track(s) at 128.0
BPM (3 further tempo change(s) not imported)".

**Verification:** can't be a headless Catch2 test (needs JUCE) — verified by a new
`midiRoundTripWorks` bounce-tool check: a three-note `Song` at 128 BPM, exported to a temp `.mid`
and re-imported into a fresh `Song` seeded at a deliberately different BPM (90), asserting the
tempo, track/clip count, and every note's beat/pitch/velocity survived (velocity rounds through a
0–127 MIDI byte, so the tolerance accounts for that — verified to round-trip within ~0.005, well
under the check's 0.01 margin). Passed on the first run; all 57 unit tests and the bounce tool's
full check suite, including `rmsDry=0.149266`, are unchanged (no engine/model impact beyond the new
module itself).

### File manager 2.0 (implemented)

The plain `juce::FileTreeComponent` list became a two-pane file manager: the existing folder tree
on top (unchanged — still the drag-into-arrangement source), a sortable, color-coded file **grid**
below it, plus folder operations and a per-project root folder.

**What was built:** `src/app/FileGrid.h` — `juce::TableListBox` + `FileGrid : private
juce::TableListBoxModel`. Columns: Name / Type / Size / Modified / Duration (audio only, probed
lazily via the grid's own `juce::AudioFormatManager` — a file-header read, not a full decode — and
cached per path so re-sorting never re-probes). Sortable by column natively via `TableListBox`.
`src/app/FileTypeColors.h` — `classifyFile()`/`colourForFileKind()`/`labelForFileKind()`: audio
extensions one hue, `.mid`/`.midi` another, `.looper` project files a third, unrecognized dimmed —
used for both the grid's Type-column text color and a subtle per-row background tint (folders get
their own color too, for the tree).

**Layout call — simpler than first planned:** rather than splitting into a folders-only tree +
files-only grid (which would have needed a second drag-and-drop source, since `ArrangementView`/
`DockRegion` currently recognize file drags by `FileTreeComponent`'s *type*), the existing
`FileTreeComponent` was left exactly as it was — same filter, same drag-out, same double-click
preview — and the grid was added *underneath* it as a detail companion, showing whichever folder
was last clicked in the tree (or navigated to via Places). This kept the one already-proven,
working drag mechanism as the only one, at the cost of the grid not itself being a drag source in
this pass. A fixed 55/45 vertical split, not a draggable divider, for the same "keep this addition
contained" reason.

**Folder management:** right-click either the tree or the grid opens a `juce::PopupMenu`: New
Folder, Rename, Delete. New Folder/Rename use a manually-owned `juce::AlertWindow` (not
`deleteWhenDismissed = true`) because JUCE deletes an auto-delete-on-dismiss `AlertWindow` *before*
calling back — reading `getTextEditorContents()` in that callback would be a dangling-pointer bug,
so the panel owns the dialog itself, reads it, then resets it. Delete uses
`AlertWindow::showAsync` with a plain `MessageBoxOptions` confirm ("Permanently delete ... this
cannot be undone.") before calling `File::deleteRecursively()` — no custom lifetime handling
needed there since nothing is read back from the dialog afterward.

**Project root folder:** `model::Song::projectRootFolder` (`std::string`, empty = unset) —
serialization bumped to `LOOPER 10` (a `PROJECTROOT <path>` line, the path as the rest of the line
like `audioFile`/track `name` already are, since a real folder path can contain spaces — the
round-trip test was updated with a path that deliberately has one). Set via **File > Set Project
Root Folder...**; once set it shows as an extra, always-present "Places" button in
`FileBrowserPanel` (hidden via `setVisible(false)` *after* `addAndMakeVisible`, not before —
`addAndMakeVisible` unconditionally forces visibility true, so setting it false first and then
calling `addAndMakeVisible` would silently undo it; a real bug caught in review before it shipped).
Synced from `MainComponent::refreshFromModel()`, so it updates on new/open project and on
undo/redo, same as every other per-song UI sync already grouped there.

**Verification:** mostly filesystem/UI logic, not audio — all 57 unit tests (including the updated
serialization round-trip) and the bounce tool's full check suite, including `rmsDry=0.149266`, are
unchanged (no engine impact). The destructive folder operations, the grid's rendering/sorting, and
the dialog flows are all JUCE-dependent and need a live try before being trusted — Delete
especially, given it's irreversible.

### Piano roll: key-name gutter (implemented)

The concrete ask: a left-hand gutter naming each row's pitch ("C4", "C#4", "D4", ...) — `PianoRoll`
previously had *no* such gutter at all, the grid filled the full width with only black/white row
shading to go on. Built the same way `ArrangementView` already names its lanes
(`TimelineGeometry.gutterWidth`), applied to the pitch axis instead of the time axis.

**What was built:** `src/app/PianoRollGeometry.h` — a JUCE-free geometry struct (`pitchForRow`,
`rowForPitch`, `cellAt`, `xForStep`/`yForRow`) mirroring `TimelineGeometry`, pulled out of
`PianoRoll` itself so the row↔pitch math is unit-tested headless for the first time
(`tests/app/PianoRollGeometryTests.cpp`). Two small JUCE-free helpers moved into the existing
`engine::MidiNote.h` module rather than living in the UI layer: `midiNoteName(int)` (scientific
pitch notation, middle C = C4 — this project's existing convention, per `PianoRoll`'s demo pattern)
and `isBlackKey(int)` (previously a private, untested `PianoRoll` method) — both unit-tested in
`tests/engine/MidiNoteTests.cpp`. No JUCE note-naming API was used, since a hand-written,
JUCE-free version could be tested the same way every other pure-math helper in this project is.

Bundled, as planned: hover-row highlighting (`mouseMove`/`mouseExit`), and a heavier line every 12
rows at octave boundaries. Not bundled, as planned: scrolling/zoom, drag-to-resize notes.

**Drum-track connection:** not wired up yet — `TrackType::Drum` doesn't exist until the drum-kits
subsection below is built. `PianoRollGeometry`/`PianoRoll` are already structured so that seam (a
`labelForRow(row)` swapping pitch names for pad names) is a small, contained addition later rather
than a rewrite.

**Verification:** the row↔pitch math is now unit-tested (`PianoRollGeometryTests.cpp` plus two new
`MidiNoteTests.cpp` cases — 7 new test cases in total, all passing). The rendering itself
(gutter/hover/octave-line layout) is JUCE-dependent and could not be verified headlessly — needs a
live look. All 57 unit tests pass and the bounce tool's full check suite, including
`rmsDry=0.149266`, is unchanged (pure UI change, zero engine/model impact).

### Drum kits (implemented)

A track type where each row is an independent one-shot sample (kick, snare, hat, ...), replaceable
per-pad, instead of one melodic synth timbre shared across every note. The last, biggest item —
built on top of the piano-roll gutter (pad-name labels) and the file browser's drag-and-drop
(sample assignment), as planned.

**Model:** new `model::TrackType::Drum`. A `DrumKit` struct (`src/model/DrumKit.h`) held per-track
alongside `gainAutomation`, with a small pad list — `struct DrumPad { int noteNumber; std::string
label; std::string samplePath; };`. `model::addTrack` auto-populates the starting default the
moment a `Drum` track is created: four pads — Kick (36), Snare (38), Hat (42), Other (45),
matching "one or more bass, snare, and other instrument types" rather than a full GM drum map.
`samplePath` empty means silent, the same safe default an audio clip with no file gets. Serialization
bumped to `LOOPER 11` (a `DRUMKIT`/`DPAD` section per track; `label` is a space-free token since no
pad-rename UI exists, `samplePath` is the rest of the line like `audioFile`/track `name` already are).

**Engine:** new `engine::DrumKitNode` (`src/engine/DrumKitNode.h`), built the way
`SynthInstrumentNode` already wraps `juce::Synthesiser` — reusing its polyphony/sample-accurate
dispatch rather than a bespoke voice pool. `DrumSampleVoice : juce::SynthesiserVoice` looks up the
triggered note's assigned `ClipData` on `startNote()` (via a whole-pad-map swap — `DrumPadMap`,
the same lock-free message→audio hand-off shape as `AudioFilePlayerNode`'s clip-list swap) and
plays it once to the end, *ignoring note-off* — confirmed by reading JUCE's own
`Synthesiser::noteOff` before relying on it: a normal note-off passes `allowTailOff = true`, which
`DrumSampleVoice::stopNote` deliberately ignores (a drum hit isn't a sustained voice); only a hard
stop (`allowTailOff = false` — voice stealing, all-notes-off) cuts it immediately. Reuses
`AudioEngine::decodeOrGetCached` so assigning one sample to several pads (or tracks) never
double-decodes. `InstrumentTrack` gained a `DrumKitNode drumKit` alongside `synth`/`audioPlayer`,
plus an explicit `isDrumTrack` atomic flag routing a track's notes to one or the other —
*unlike* audio clips (which naturally stay silent with nothing submitted), the synth always
produces *some* sound for any note it receives, so Drum-track routing has to be explicit rather
than left to content-gating.

**UI:** `src/app/DrumKitEditor.h` — one row per pad (label, assigned sample name or
"(no sample)", a "Load..." button), shown above the piano roll only when the selected track is a
Drum track, *and* accepting a file dragged straight from `FileBrowserPanel`'s tree (the same
`DragAndDropTarget`-by-sourceComponent-type check `ArrangementView` established) — dropping a file
on a row reassigns that pad. `PianoRoll` gained `setDrumPads()`/`setMelodicMode()`: in drum mode
the gutter shows pad names instead of pitch names, black/white-key shading and octave lines are
skipped (neither means anything for pads), and the row count matches the pad list instead of the
usual 2-octave range. A new **Add Drum** button (mixer toolbar, next to **Add Track**) creates one.

**Bug caught in review:** `DrumKitEditor`'s implicit default constructor was, for reasons not
fully root-caused, rejected by the compiler as a `MainComponent` member (`juce::Component` +
`juce::DragAndDropTarget` multiple inheritance works fine elsewhere in this codebase without an
explicit constructor — e.g. `ArrangementView` — so this wasn't simply "that pattern needs one").
Adding `DrumKitEditor() = default;` resolved it; flagged here in case the same shape recurs.

**Verification:** exactly what the bounce tool already does well — a kick+snare pattern (kick on
beats 0/2, snare on 1/3) rendered through `DrumKitNode` via the normal sequencer path, asserting
sound in each hit's window and silence in the gaps — including a *third*, deliberately unassigned
pad (Hat) whose note fires but produces nothing, confirming a triggered-but-empty pad stays silent.
New `OfflineRenderer::renderDrumPattern` mirrors `renderClips`/`renderAudioClips`'s existing shape.
Passed on the first run. All 57 unit tests (including the updated serialization round-trip, which
now also covers a `Drum` track and a `samplePath` with a space) and the bounce tool's full check
suite, including `rmsDry=0.149266`, are unchanged.

---

## 18. Next planned features: project-format versioning, metronome, editable clips & notes

Where things actually stand: Phases 1–4 are done (multi-track sequencing, mixer, master effects,
gain automation, offline bounce) and the app shell has grown a freely splittable docking workspace
with dedicated Synth and Drums panes. What's missing is less "another subsystem" than **the ability
to write a real musical idea in it** — notes are all one length and one velocity, clips are all four
beats, there's no clipboard, and there's no click to play to.

This batch closes that gap. Build order — the format fix first because it protects work already on
disk, then smallest-to-largest:

0. **Project-format versioning** (a present bug, not a feature — see below).
1. **Metronome + count-in** (small, standalone, unblocks recording in time).
2. **Piano roll: note length, velocity, zoom/scroll.**
3. **Clip length + resize handles.**
4. **Copy/paste/duplicate, quantize, swing.**

As in §17, the judgment calls are flagged here rather than buried in the code.

### 0. Project-format versioning (implemented)

`model::deserialize` reads the version token after `LOOPER` but never *checks* it, and then requires
every record of the current format in order. Two format bumps landed in one sitting (`12` for
per-track `SynthSettings`, `13` for per-pad drum mix), which means **every project saved before those
bumps now fails to open** — the parser reaches a missing `SYNTH` line and returns `false`, surfacing
as a generic "couldn't open" with no explanation.

The fix is to treat the version as data: parse it, and make records added after version *N* optional
when reading a file older than *N*, falling back to the struct's defaults (which are already chosen
to be behaviour-preserving no-ops). Two judgment calls:

- **Forward compatibility is explicitly not offered.** A file newer than this build is rejected with
  a clear message rather than partially parsed — silently dropping records the user can't see is
  worse than refusing.
- **The reader gets the version-tolerance, not the writer.** `serialize` always emits the current
  format; there's no "save as old version". Keeps one write path and one set of tests.

Every subsequent item in this batch bumps the format again, so this lands first.

### 1. Metronome + count-in (implemented)

There is no click at all today, which makes recording in time guesswork. A metronome is a source
node driven by the existing `TempoMap`/`Transport`, emitting a short synthesized tick (accented on
the bar) — no sample assets, no new dependency.

- **Not a track.** The click is engine-level and deliberately excluded from the bounce, so it can
  never end up in an export. That means it sums in *after* the master chain rather than through it.
- **Count-in is a transport property**, not a metronome one: arm, hit record, and the transport rolls
  a configurable number of bars before the playhead starts capturing.

### 2. Piano roll: note length, velocity, zoom/scroll (implemented)

The piano roll is honest about being minimal ("fixed step length, no drag-resize, no scrolling/zoom
yet") and that is now the main thing between this app and writing an actual part. Three changes, in
order of value: drag a note's right edge to set its length; drag vertically on a note (or a velocity
lane under the grid) to set velocity; scroll and zoom the pitch range beyond the fixed two octaves.

- **`engine::Note` already carries `lengthBeats` and `velocity`** and the sequencer already honours
  both — this is a UI-side gap only, so it needs no engine work and no format change.
- **Keep the click-to-toggle step behaviour** for fast drum-style entry; length/velocity editing is
  additive, not a replacement. The drum step grid keeps its one-click-per-step model unchanged.
- Zoom/scroll wants the pitch range to become state on `PianoRollGeometry` rather than the current
  fixed `lowPitch`/`numRows` constants — which keeps the conversion math unit-testable headless, as
  it is today.

### 3. Clip length + resize handles (implemented)

Clips are created at a hardcoded 4 beats and `ArrangementView` supports moving them but not resizing
them, so an eight-bar section is unreachable. Adds a drag handle on each clip's right edge, mirroring
the existing move-drag (`onClipMoved` → `onClipResized`), plus a length field for exact values.

- **Clip length and pattern length are separate concepts** and stay separate: the clip's window on
  the timeline vs. the loop length of the pattern inside it. Resizing the window should *not*
  silently re-loop the content, so both get their own control.
- The single-clip "unbounded length" special case in `syncEngineTracks` (a lone clip loops forever)
  has to survive this, or existing projects change behaviour.

### 4. Copy/paste/duplicate, quantize, swing (implemented)

No clipboard exists anywhere in the app; duplicating a bar means redrawing it by hand. Adds
copy/paste/duplicate for both clips (in the arrangement) and note selections (in the piano roll),
then quantize and swing over a note selection.

- **An app-level clipboard holding model values**, not a system-clipboard serialization — pasting
  between two instances of the app isn't worth the format work yet.
- **Quantize needs a selection model** in the piano roll (there isn't one today — clicks toggle
  single notes), so selection lands as part of this item rather than being assumed.
- Swing is expressed as a percentage offset applied to off-beat subdivisions at edit time, writing
  real note positions rather than a playback-time feel parameter — keeps the engine unchanged and
  the result visible and editable, consistent with "AI produces editable musical data" elsewhere.

All five landed. Two things the plan didn't anticipate, recorded because they
shaped the result:

- **Clip resize alone doesn't give you a longer part.** A track holding a single clip is still
  given an unbounded window by `syncEngineTracks` (the "one clip plays until Stop" rule the plan
  said had to survive), so resizing a lone clip changes what you see and what exports, but not when
  it stops sounding. *Pattern length* is what actually makes a longer part — and the piano roll was
  hardcoded to 16 steps, so a longer clip wasn't even editable. Item 3 grew to cover all three.
- **The song's time signature was never pushed anywhere.** Both tempo maps sat at 4/4 regardless of
  the document, so a 3/4 project got the wrong bar/beat readout, the wrong loop length, and (once
  the metronome existed) its accent on every fourth beat instead of every third. Fixed as part of
  item 3, since the bars-to-beats maths depends on it.

### After this batch

The ordering beyond here, with the reasoning:

- **Per-track insert effects (implemented).** Each track now has its own filter, delay and reverb,
  pre-fader, reusing the master bus's settings structs and a new Track FX pane. Shipped as a *fixed
  trio* rather than the general chain this entry originally imagined: the engine's no-real-time-graph-
  surgery rule makes a fixed set free (members of `InstrumentTrack`, prepared once, bypassed when
  off), while an arbitrary reorderable chain needs a slot abstraction with a lock-free swap. That is
  better designed alongside plugin hosting, which forces the question anyway — so the abstraction
  moves there rather than being guessed at now.
- **Automating more than gain (implemented).** Lanes are now keyed by a `TrackParam`, so adding an
  automatable parameter is an enumerator plus the code that applies it. Gain, **pan** (which didn't
  exist as a parameter at all and had to be added first) and send level are automatable; the export
  path automates gain and pan sample-accurately. Insert-effect and synth parameters are the obvious
  next enumerators and need no new machinery. Playback is **sample-accurate** as of the follow-up
  work: tracks carry their own curves and ramp them across each block, which also let the offline
  renderer's separate isolation path be deleted — export and playback now run the same code.
- **Session view: clip launching + scenes** — the loop-first identity §1 is built around, and still
  entirely absent. Held until the items above land, because clip launching is far more compelling
  once clips are properly editable.
- **Plugin hosting**, then the **AI/generative layer** (symbolic MIDI first, which wants the
  key/scale awareness that quantize in this batch already starts to need).

Known limits not scheduled yet, recorded so they aren't rediscovered as surprises: the fixed 8-track
pool (`kMaxTracks`), RAM-only recording capped at 180 s with no disk streaming, and drum pads having
no choke groups or velocity layers.

---

## 19. Session view: clip launching and scenes (implemented)

The thing §1 says this product *is* — "a workflow built around clips and loops (like Ableton's
Session View) rather than only a linear tape timeline" — and the one major piece of that identity
that has never been built. Everything so far is a linear-timeline DAW that happens to be good at
loops.

### The shape of it

A grid: **tracks are columns, scenes are rows**, and each cell either holds a clip or is empty.
Clips are *launched* rather than positioned — you click one and it starts at the next musical
boundary and loops until something replaces or stops it. A scene launches its whole row at once.
Nothing about this is on a timeline; a session clip has no start position, only a slot.

### What v1 covers

1. **Model.** A `SessionSlot` per (track, scene), and a list of `Scene`s on the Song. Deliberately a
   *separate container* from `Track::clips` rather than a flag on the existing clips: session and
   arrangement are genuinely different things — one is a grid of alternatives, the other a sequence
   of placements — and conflating them would put a meaningless `startBeats` on every session clip.
2. **Launch quantization.** A clip launched mid-bar waits for the next bar line. This is what makes
   the workflow musical rather than a game of reflexes, so it is not optional polish.
3. **Engine playback.** Each track plays at most one session clip, looping from wherever it was
   launched. Per-track stop, and a global stop-all.
4. **UI.** A dockable Session pane: the grid, clip cells you click to launch, a scene-launch column,
   and stop buttons.
5. **Serialization**, so a session survives a save.

### Deliberately not in v1

Recording into session slots; follow actions; per-clip launch modes (gate/toggle/repeat) beyond
plain looping; audio clips in the session grid (MIDI first — audio clips need warping to be useful
at a tempo other than the one they were recorded at, and that's its own entry); and dragging clips
between the session grid and the arrangement. Each is worth having; none is needed for the workflow
to be real, and shipping them together would make the first version unverifiable.

### The decisions worth recording

- **Session and arrangement are mutually exclusive per track, chosen by the track itself.** A track
  playing a session clip ignores its timeline clips and vice versa. Ableton resolves this the same
  way (launching a clip takes the track out of arrangement playback). The alternative — summing both
  — has no musical meaning.
- **Launch quantization is engine-side, not UI-side.** The UI records "the user wants slot 3"; the
  audio thread decides *when* that becomes true, because only it knows the sample-accurate playhead.
  Doing it on the message thread would quantize to the 30 Hz timer, which is exactly the mistake the
  automation work just undid.
- **The quantum is a transport property**, like tempo — one setting for the whole session (off, 1
  bar, 2 bars...), not per clip. Per-clip quantum is an Ableton feature worth having eventually and
  is pure addition later.
- **Launching is a request, not a command.** The message thread writes a single atomic per track;
  the audio thread consumes it at the next boundary. No queue, because a second click before the
  boundary should *replace* the pending launch rather than stack up behind it — which a queue would
  get wrong and an atomic gets right for free.
- **The pattern-playback loop is shared with the arrangement sequencer** rather than copied. Both
  emit the same notes from the same Pattern; only the question of *which* pattern and *from when*
  differs. Copying it would mean two places to fix the next timing bug.

### Build order (all three landed)

Three independently verifiable stages, in this order:

1. **Model, serialization, and the launch-boundary maths** (JUCE-free and unit-tested, like
   SequencerMath and MetronomeMath before it). Nothing audible yet, but the part where an off-by-one
   is silent and permanent.
2. **Engine playback** — the session player, per-track routing, stop-all — verified by the bounce
   tool: a launched clip must sound, must start *on* the boundary and not before, and must stop when
   told.
3. **The Session pane.** Last, because by then the thing underneath it is already known to work, and
   the UI is the part that can't be verified here.

---

## 20. Plugin hosting (implemented)

VST3 and Audio Unit hosting — the thing that turns a fixed set of built-in
effects into an open one, and (per §15) the single biggest multiplier available.

**Feasibility, checked before planning rather than assumed.** JUCE 8 bundles the
VST3 SDK under `juce_audio_processors_headless/format_types/VST3_SDK` (it moved
there from `juce_audio_processors`, which is where an obvious first look fails to
find it), and AU hosting on macOS needs only system frameworks. Building the app
with `JUCE_PLUGINHOST_VST3=1` and `JUCE_PLUGINHOST_AU=1` compiles clean, so no
external SDK and no new dependency is required. Those defines are in
`src/app/CMakeLists.txt` from stage 1 onward.

### The decision this forces: one effect chain, not two

§18 shipped per-track effects as a *fixed trio* — filter, then delay, then reverb
— and said the general, reorderable chain "is better designed alongside plugin
hosting, which forces the question anyway." It does: a hosted plugin is an effect
in the same chain as the built-ins, and bolting a separate "plugin list"
alongside the trio would leave two effect concepts that each need their own
ordering, bypass, serialization and UI.

So the model becomes a **list of effect slots**, each slot being either a
built-in (filter/delay/reverb) or a hosted plugin. A slot keeps *all* the
built-in settings regardless of which kind it currently is, so switching kind
doesn't lose the others — the same pattern `SendBusSettings` already uses for its
reverb-or-delay choice.

### What the document stores for a plugin

Deliberately **JUCE-free**: a format tag, the identifier JUCE uses to find the
plugin again, a display name, and an opaque state blob. Three reasons, in order
of weight:

1. `looper_tests` links Catch2 only, so anything JUCE-typed can't be tested
   headlessly — and this is a serialization-shaped problem, exactly the kind that
   needs tests.
2. The document should survive a plugin being missing. Storing a name means a
   project that references a plugin this machine doesn't have can say *which* one
   rather than silently dropping it.
3. `juce::PluginDescription` is a UI/engine concern; converting at the boundary
   is the same split already used for `AutomationCurve` vs `AutomationLane`.

### Build order

1. **Model + serialization** (this stage). The effect chain, the plugin
   reference, and migration of the existing per-track trio into chain slots — an
   old project must come back with its effects in the same order and sounding the
   same. Testable headlessly, which is the point of doing it first.
2. **Engine.** A variable-length chain per track, replacing the fixed trio, with
   the lock-free swap the rest of the engine uses. Then plugin instantiation and
   a scan. This is the real-time-risky stage: a hosted plugin allocates, blocks,
   and misbehaves in ways the built-ins never do.
3. **UI.** A chain editor (add/remove/reorder/bypass), a plugin browser backed by
   a cached scan, and plugin editor windows. *Landed*, replacing the fixed
   filter/delay/reverb panel — which could only edit one of each, and stopped being
   true the moment a chain could hold two filters or a plugin.

### Recorded now so it isn't rediscovered

- **Scanning is in-process, and that is still outstanding.** Stage 2 shipped crash
  *recovery* (a dead man's pedal file records the plugin being probed, so one that kills
  the app is skipped next run) but not crash *isolation* — the first crash still takes the
  app down. Real isolation needs the probe in a child process. §15 lists plugin stability
  as a risk and this is the unpaid part of it.
- **`KnownPluginList` and `PluginDirectoryScanner` are GUI-module only.** `PluginHost` keeps
  its own list of descriptions instead, which is what lets the headless bounce tool scan and
  instantiate — and therefore verify hosting against a real plugin rather than a mock.
- **Hosted plugins break the engine's no-allocation rule.** They allocate in
  `prepareToPlay` and some misbehave in `processBlock`. The chain hand-off keeps
  *instantiation* on the message thread, but a badly-behaved plugin can still
  glitch audio — that's inherent to hosting, and worth stating rather than
  pretending the RT discipline extends into third-party code.
- **Plugin state is opaque and version-fragile.** It is stored as the plugin's own
  blob, base64'd. A plugin that changes its format across versions is its own
  problem, not something this document can fix.
- **Licensing.** §15 already flags it: shipping VST3 hosting means the Steinberg
  agreement, and JUCE's own licence gates a closed-source product. Neither is a
  code problem, and neither is solved by this section.

---

## 21. A guitar instrument and its pane (implemented)

A fourth track type alongside Instrument, Audio and Drum: a plucked-string
instrument that is *played like a guitar* rather than a synth patch that happens
to sound stringy. The difference between those two is almost entirely in the
performance model, not the oscillator, which is what shapes this plan.

### What actually makes it sound like a guitar

Ranked by how much each contributes, because that ordering decides what to build
first:

1. **One note per string.** A guitar has six strings and can sound at most six
   notes, one per string — and a new note on a string *cuts the one already
   ringing there*. Nothing else on this list is as audible. A polyphonic synth
   playing six notes is the single clearest giveaway that something isn't a
   guitar.
2. **Strums are not chords.** Strings are struck in sequence, roughly 10–30 ms
   apart, alternating down and up. Played simultaneously, the same six notes
   read as an organ.
3. **Per-string timbre.** The same pitch on the low E at fret 12 and on the high
   E open are different sounds — different string mass, tension and length. A
   model that only knows pitch cannot produce this.
4. **Pluck excitation and pick position.** Plucking near the bridge is bright and
   thin, over the soundhole round and full. This is a comb filter set by where
   along the string it's excited.
5. **Body resonance.** The instrument's body colours everything, and is most of
   what separates an acoustic from an electric.
6. **Articulation.** Hammer-on and pull-off (a new pitch on a *still-ringing*
   string, no new pluck), slides, bends, vibrato, palm mutes, dead notes.
7. **Noise.** Finger squeak on position changes, fret buzz, pick attack.

### Synthesis approach: extended Karplus-Strong, not samples

**Recommendation: a digital waveguide (extended Karplus-Strong) per string.**

A plucked string is one of the few instruments where physical modelling is both
cheap and genuinely convincing: a delay line whose length sets the pitch, a
lowpass in its feedback path for damping, excited by a noise burst. That is a
handful of multiplies per sample.

Why this over a sample library, which would be the other obvious answer:

- **It needs no content.** A convincing multisampled guitar is thousands of
  recordings across strings, frets, velocities and round-robins. This project has
  no sample library and no pipeline for one.
- **Articulation falls out of the model.** A hammer-on is "change the delay
  length without re-exciting" — one line. In a sampler it's a whole extra layer
  of recordings, and it's still an approximation.
- **Per-string timbre falls out too**, because each string is a separate
  resonator with its own damping and stiffness rather than a transposed copy of
  one recording.
- It fits what's already here: `DelayLine`, `StateVariableFilter`, and an engine
  built around fixed pre-allocated per-voice state.

The honest cost: it will sound like a *good synthetic* guitar, not like a
recorded one. Sampling wins on raw realism and always will. If photorealism is
the goal, this is the wrong approach and a sample library is the right one — that
choice should be made deliberately, up front, not discovered later.

### Low-level: the DSP

**The string.** For each of six strings, a delay line of length `L = sampleRate /
f0` samples, fed back through a damping filter:

```
    y[n] = filter( y[n - L] )        // the loop
```

**Fractional delay is not optional, and the existing DelayLine can't do it.**
`DelayLine::processSample` takes an `int delaySamples` and reads an integer index.
Rounding the loop length quantises pitch, and the error grows as pitch rises
because `L` shrinks. Measured at 48 kHz:

| note | f0 | delay (samples) | pitch error |
|---|---|---|---|
| E2, open low E | 82.41 Hz | 582.45 | +1.3 cents |
| E4, open high E | 329.63 Hz | 145.62 | −4.5 cents |
| E5, 12th fret | 659.26 Hz | 72.81 | −4.5 cents |
| E6, 24th fret | 1318.51 Hz | 36.40 | +19.4 cents |

Twenty cents sharp is a fifth of a semitone — audibly out of tune, and worse the
further up the neck you play. So the string needs its own delay line with a
**fractional read**: linear interpolation is the floor, a first-order allpass or
Lagrange interpolator is better because linear interpolation is itself a lowpass
and so leaks into the damping. `AudioFilePlayerNode` already does linear
interpolated reads (`sampleLinear`) — the technique is in the codebase, just not
in `DelayLine`.

**Damping (the loop filter).** A one-pole lowpass in the feedback path sets decay:
more damping, faster decay and duller tail. Two things it must do that a naive
implementation gets wrong:

- **Pitch-compensate.** A fixed coefficient makes high notes die far too fast,
  because they go round the loop more often per second. Decay time should be
  specified in *seconds* and the coefficient derived from `f0`.
- **Preserve loop gain < 1** at all pitches, or the string self-oscillates.

**Stiffness/dispersion.** Real strings are stiff, so high partials travel faster
and the sound is slightly inharmonic — this is why a piano and a guitar don't
sound like a synth sawtooth. A cascade of 1–4 allpass filters in the loop gives
it. Worth having; not worth blocking on.

**Excitation.** A short noise burst, lowpass-shaped by pick hardness, its length
tied to pluck strength. **Pick position** is a comb: mix the burst with a delayed
copy of itself, delay = `pickPosition × L`. This is a two-line change that buys a
large amount of the character.

**Body.** Cheapest useful version is a small bank of resonant peaks (3–5 biquads).
Better is convolution with a body impulse response, which is a bigger dependency
(a partitioned convolver) and is the natural place to stop for v1 — especially
since the effect chain can now host a convolution *plugin*.

**Per-string state.** Six `GuitarString` objects, each owning its delay line,
loop filter, dispersion allpasses and current fret. Fixed and pre-allocated, like
every other voice pool here — no allocation on the audio thread.

### The performance model, which is the part that matters

**Voice allocation is string allocation.** A note arriving must choose a string.
The rule: pick a string that can reach the pitch (`f0 >= openString[i]`),
preferring the one that needs the lowest fret and is not already ringing; if all
candidates are ringing, take the one whose note is oldest and cut it. That "cut"
is not voice stealing to save CPU — it is the instrument working correctly.

**Where do string and fret come from?** MIDI carries neither, and this is the
central design question. Three options:

1. **Infer at play time** from the rule above. No format change, works with every
   existing clip and with MIDI import. Can pick a different fingering than a
   player would.
2. **Store string/fret on the note.** Exact, but it's a change to
   `engine::Note` (which sits under `model::Clip`), a format bump, and MIDI
   export would silently drop it.
3. **A MIDI channel per string**, which is what real guitar-MIDI hardware does.
   Standard, survives export, and costs nothing structurally.

**Recommendation: (1) now, designed so (3) can be honoured later if a channel is
present.** Inference gets the instrument playable against everything that already
exists; the channel convention is then purely additive.

**Strumming** belongs in the *pane*, not the engine: it writes real notes with
real time offsets into the pattern, the way the swing implementation in §18
writes real positions rather than adding a playback-time feel parameter. It stays
visible and editable, and the engine stays simple.

### The pane

A fretboard, because that is the interface the instrument actually has — six
strings across, frets down (or across, laid out like a neck), which is also the
only view in which "one note per string" is self-evident.

- **Fretboard grid.** Click a fret to sound that note on that string; the
  currently ringing note per string is highlighted, which makes the cut-on-retrigger
  behaviour visible rather than mysterious.
- **Chord palette.** Named shapes (open chords, barre shapes) that stamp a
  voicing into the pattern. This is what makes the pane fast to use, and it is
  the thing a piano roll can't express.
- **Strum controls.** Direction, spread in milliseconds, humanise amount — the
  three parameters that turn a chord into a strum. Applied as an edit, per above.
- **Per-string tuning**, so drop-D and open tunings work. Six pitch fields, with
  presets.
- **Tone controls.** Pick position, pick hardness, damping/decay, body amount,
  string stiffness — the model's parameters, named as a guitarist would name
  them rather than as the DSP does.

The piano roll keeps working on a guitar track, since the notes are still notes.
The fretboard is an additional way in, exactly as the Drums pane sits alongside
the piano roll for drum tracks.

### Verification

The parts that can be proven headlessly, which is most of the DSP:

- **Pitch accuracy.** Render each open string and every fret to 24, measure f0 by
  autocorrelation, require < 2 cents error. This is the check that would have
  caught the integer-delay problem above, and it should be written *before* the
  string is.
- **Decay time.** Set a decay of *n* seconds, measure the time to −60 dB, require
  it within tolerance — and require it to hold across the whole pitch range,
  which is the pitch-compensation bug.
- **Loop stability.** No string may grow in amplitude over 30 seconds at any
  pitch or damping setting.
- **One note per string.** Retriggering a string must cut the previous note:
  assert the earlier pitch is gone from the spectrum.
- **Strum offsets.** Six notes stamped by a strum must have monotonically
  increasing start times, spread within tolerance of the requested milliseconds.
- **RT-safety.** No allocation in `process` — the existing discipline.

`rmsDry` and the other bounce checks must not move: this is a new track type, and
nothing it adds may touch the existing render path.

### Build order

1. **`GuitarString` DSP alone** *(implemented)*, JUCE-free, with the pitch/decay/stability
   tests above. Nothing audible in the app; the part where being wrong is silent.
2. **`GuitarNode`** *(implemented)* — six strings, string allocation, MIDI in — plus
   `TrackType::Guitar`, `model::GuitarSettings`, serialization, and the
   bounce checks for one-note-per-string.
3. **The fretboard pane** *(implemented)* — grid, tuning, tone controls. Clicking a fret
   sounds it through the armed track, so the pane plays the same instrument the sequencer
   does, cut rule included. Which notes are ringing is read from the engine rather than
   inferred, since a string keeps sounding after its note-off.
4. **Chords and strumming** *(implemented)*, as pattern edits — a palette of open shapes
   (movable up the neck by a fret offset) stamped into the clip as real, staggered notes.

**Articulations.** Hammer-ons and pull-offs are *implemented*, and needed no format change
at all: when every reachable string is already held, the note is re-fretted rather than
struck. That is both what a guitarist does when their hand is already on the string and
why those notes are softer — the softness falls out of the model instead of being
simulated. Inferring it from the notes matches the same decision made above for string and
fret assignment, and keeps clips playable by anything that can emit MIDI.

Slides, bends and vibrato are deliberately **not** done. Unlike a hammer-on they are
*continuous* pitch changes, so they need a per-note curve rather than a per-note flag —
which is automation-shaped, and belongs with the automation system (§18) rather than being
bolted onto the note. Palm muting is likewise continuous and is the natural first candidate
for a guitar-specific `TrackParam` lane; `GuitarSettings::muteOnNoteOff` is a track-wide
stand-in until then.

---

## 22. Appendix: reference reading

- **Real-time audio programming:** Ross Bencina, *"Real-time audio programming 101: time waits for
  nothing"* (the no-locks/no-allocations canon).
- **ADC (Audio Developer Conference)** talks — especially Fabian Renn-Giles & Dave Rowland,
  *"Real-time 101"*, and Timur Doumler on lock-free programming and `std::atomic`.
- **JUCE** documentation, tutorials, and the `juce::dsp` / `AudioProcessorGraph` sources.
- **DSP:** Will Pirkle, *Designing Audio Effect Plugins in C++*; Julius O. Smith's online DSP books.
- **Plugin formats:** Steinberg VST3 SDK docs; Apple Audio Unit docs; the **CLAP** spec (`cleveraudio.org`).
- **Sync:** Ableton **Link** SDK.
- **ML audio:** ONNX Runtime C++ docs; Meta **AudioCraft/MusicGen**; **Stable Audio Open**; **Demucs** (stem separation).
- **Validation:** **`pluginval`** (Tracktion).

---

*Document owner: Anthony Lazzaro · Status: draft v1 · License: MIT (see `/LICENSE`).*

---

## 23. Guitar pedals

The guitar synthesis in §21 produces a clean electric-guitar tone. Clean is
the one sound almost no electric-guitar part actually uses: the instrument's
voice is mostly what happens *after* the strings. Pedals are that.

### Where they live: the existing chain, not a pedalboard

§20 decided one effect chain per track rather than two, and pedals do not
justify reopening it. A pedal is an insert effect on a track; the chain
already runs insert effects in a user-ordered list with bypass, and ordering
is the whole point of a pedalboard (drive into delay sounds nothing like
delay into drive). So pedals are new `EffectKind` values in the chain that
exists, not a parallel system with its own routing, ordering and UI.

The one thing they add is that a pedal is only *sensible* on a guitar track,
whereas a filter is sensible anywhere. That's a presentation concern — the
chain's add-menu can group them — not a routing one.

### What actually makes a distorted guitar sound right

Three things, in descending order of how badly they hurt when missing:

1. **Anti-aliasing.** Clipping generates harmonics without limit. Every one
   above Nyquist folds back to a frequency that is not harmonically related
   to anything being played, so it doesn't read as "bright" — it reads as
   metallic, detuned grit that gets worse as you play higher. This is the
   single biggest difference between a distortion that sounds like an amp and
   one that sounds like a bit-crusher.

2. **Speaker simulation.** A guitar speaker rolls off hard above ~5kHz. Real
   distortion is full of energy up there, and without the cab it is heard as
   fizz. A distorted signal through a cab sim sounds like a guitar; the same
   signal without one sounds like a broken tweeter. This is why it ships in
   stage 1 rather than "later" — drive without it would be judged as sounding
   wrong, and the wrongness would be blamed on the drive.

3. **Where the tone control sits.** A tone stack *before* clipping decides
   which frequencies get distorted; *after*, it only shapes what came out.
   Both are real pedal designs and they sound different. We do pre-emphasis
   before and a tilt after, which is the arrangement most drive pedals use.

### Low-level: antiderivative anti-aliasing

Oversampling is the usual answer to (1) and costs a resampler, its filters,
and 4x the work in the hot path. First-order ADAA buys a *measured* 4.5-7dB
of alias reduction for a handful of flops and no buffers at all — which also
means nothing to allocate, so it stays RT-safe by construction.

4.5-7dB is worth having and is not a solved problem: heavy drive high on the
neck will still fold audibly. Second-order ADAA or 2x oversampling on top is
the next step, and both fit behind the same interface. The figure is quoted
here because it was measured rather than assumed — an earlier draft of this
section claimed ADAA got "most of the benefit" of oversampling, and the
measurement did not support it.

For a memoryless shaper `f`, with antiderivative `F`:

    y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])

which is the average of `f` over the segment the signal traversed this
sample, rather than a point sample of it. That average is what suppresses the
aliases.

Two details that are easy to get wrong and are therefore pinned by tests:

- As `x[n] -> x[n-1]` the quotient is 0/0. Below a threshold it must fall
  back to `f((x[n] + x[n-1]) / 2)`, or a sustained note — where consecutive
  samples are nearly equal — turns into noise, which is precisely backwards.
- For `f = tanh`, `F = log(cosh(x))`, and `cosh` overflows to infinity around
  |x| = 710. The stable identity `log(cosh(x)) = |x| + log1p(exp(-2|x|)) -
  log(2)` has no overflow anywhere. A drive pedal is exactly where large
  input values show up.

### Verification

The claim "this reduces aliasing" is measurable without a listener, so it
gets measured. Drive a sine at 5kHz at 48kHz: its 5th harmonic at 25kHz is
above Nyquist and folds back to 23kHz, which is not a harmonic of 5kHz and so
lands in a bin nothing else occupies. A DFT at that one bin gives an aliasing
figure directly, for naive shaping and for ADAA. The test asserts the
reduction, and fails if ADAA is replaced by the naive shaper.

The cab sim is asserted by magnitude response: unity-ish at 1kHz, strongly
down at 10kHz. The fallback path is asserted by feeding a constant and a very
slow ramp, where the quotient is degenerate at every sample.

### Build order

1. DSP core, JUCE-free and headless: `Waveshaper` (ADAA tanh and hard clip),
   `CabinetSim`. Tests as above.
2. `DriveEffect` composing them with pre-emphasis and a post tilt, plus the
   model/serialization/engine wiring as a new `EffectKind::Drive`.
3. The pedal's controls in EffectChainPanel.
4. Further pedals on the same seam: compressor, tremolo, chorus. Each is a
   new kind, and none of them need new architecture.

---

## 24. Chord columns on the fretboard

Stamping chords currently means picking from eight fixed open shapes. That is
how a beginner's chord chart works, not how a guitar works: a guitarist plays
a shape *at a fret*, and the same hand position slid up the neck is a
different chord. The fretboard pane already draws the neck, so the neck should
be the control.

### The gesture

Clicking a fret plays the note there — that stays. With a chord mode
selected, clicking a fret instead plays the chord **rooted on that string at
that fret**: click the 3rd fret of the low E in Power mode and you get G5,
in Major mode a G barre chord.

Power chords earn their own mode rather than being one more shape, because
they're most of rock rhythm guitar and because their shape is only two or
three strings wide, which makes the click target unambiguous.

### Shapes are derived from the tuning, not hardcoded

The obvious implementation is to hardcode the familiar fingering: root, then
two frets up on each of the next two strings. That is only correct because
those strings happen to be a fourth apart, and it silently produces a wrong
chord as soon as it crosses the G–B pair (a major third) or the user
retunes — and §21 shipped drop tunings, so retuning is expected.

So a shape is a set of *intervals from the root* (power = 0, 7, 12; major =
0, 4, 7, 12; minor = 0, 3, 7, 12), and the fret for each is solved against
the actual tuning of the string it lands on. That is correct in every tuning
by construction, including drop D, and it is exactly what a player does when
they work out a shape in a new tuning.

A note is dropped if its fret falls outside the neck, so a shape rooted high
up doesn't invent frets that don't exist.

### Verification

Headless, in the existing GuitarChords tests: a power chord on the low E at
fret 3 is G, D, G. The same shape crossing the G–B pair must still be a fifth
and an octave — the case a hardcoded fingering gets wrong. In drop D, a power
chord at the same fret must move with the tuning. Fret bounds are asserted at
both ends of the neck.

---

## 25. Generative sound loops, first slice (implemented)

§10 ("The AI / generative subsystem") and Phase 7 of the roadmap have named a
generative layer since this document's first draft, but nothing in it had
been built — no `src/ai/`, and (checked by grep before starting) no
scale/key/Markov infrastructure anywhere in the codebase. §10 is explicit
about sequencing: **"Non-ML first... deterministic, instant, no model
weights, genuinely useful"** before any ML runtime. This is that first
slice: a one-click **Generate Loop...** that produces a real, editable MIDI
clip — a scale-constrained melodic loop for Instrument/Guitar tracks, a
Euclidean-rhythm drum loop for Drum tracks.

### What was built

- **`src/engine/Scale.h`** — a `Scale{type, rootNote}` over six scales
  (Major, Natural Minor, Major/Minor Pentatonic, Dorian, Mixolydian), with
  `isInScale`/`snapToScale`/`degreeToNote`. Mirrors `GuitarChords.h`'s
  "shape = array of semitone offsets from a root" representation rather than
  inventing a different one for the same idea.
- **`src/engine/GenerativeLoop.h`** — `euclideanRhythm(steps, pulses,
  rotation)`, the maximally-even onset distribution (the partition
  Bjorklund's algorithm produces), computed via integer floor division
  rather than its recursive bucket merge — simpler to get right than
  reimplementing Bjorklund from memory, and checked against a hand-verified
  tresillo (`E(3,8)`) as well as invariants (exact pulse count, determinism,
  rotation as a cyclic shift). `generateDrumLoop` builds three Euclidean
  voices (kick, snare, hat) using the caller-supplied note numbers, not
  hardcoded ones; `generateMelodicLoop` walks scale degrees (seeded, ±2 per
  onset, clamped to about an octave either side of the root) over a
  Euclidean onset rhythm. Both take a `seed`, so a given seed always
  reproduces the same loop — the same determinism discipline `DrumSynth.h`
  already established for procedural audio, applied here to procedural MIDI.
- **A bounce-tool check** (`generativeLoopWorks`), alongside `drumKitWorks`:
  a fixed-seed drum loop and fixed-seed melodic loop rendered through the
  normal `DrumKitNode`/synth paths, asserting real sound at the positions
  `euclideanRhythm`'s construction guarantees for *any* seed (rotation-0
  voices always onset on step 0; the snare's fixed backbeat always lands on
  beats 1 and 3) — `rmsDry=0.149266` and every existing check unchanged.
- **UI**: a **Generate Loop...** button next to **Add Clip** in the Arrange
  toolbar, enabled for every track type except Audio (no MIDI pattern to
  generate into). A small `AlertWindow` dialog — root note + scale + density
  for Instrument/Guitar tracks, density alone for Drum tracks — commits
  through `history_.edit(...)` exactly like `addClipToSelectedTrack`, and
  for Drum tracks reads the *track's own* pad note numbers (matched by
  label, falling back to pad order) rather than assuming the factory kit.

### The decisions worth recording

- **Deterministic + seeded, not ML.** Euclidean rhythms give musical-sounding
  onset patterns from zero training data — exactly the "no model weights,
  genuinely useful" bar §10 sets. A seeded RNG handles pitch-walk and
  velocity variation on top.
- **One-shot generate, not a live candidate carousel.** §10's UX notes say
  "always return multiple candidates"; building a preview/candidate-browser
  UI is real scope on its own. Here, **Generate Loop...** commits directly
  as a new clip — a different take is Undo + click again, which the
  existing undo stack already gives for free. A multi-candidate picker is a
  natural, separable follow-up.
- **The snare's rotation is fixed, not seed-derived, and the kick always
  starts on beat 1.** The original plan for this section assumed rotation
  itself would vary with the seed; while implementing `generateDrumLoop` it
  became clear that's the wrong knob — a random rotation regularly put the
  snare off the backbeat, which reads as broken rather than as variety.
  `euclideanRhythm(2, 16, rotation=stepsPerBar/4)` deterministically lands
  the snare on beats 2 and 4 in any 4/4 loop; the *seed* instead perturbs
  the kick/hat pulse counts (±1) and every voice's velocity, which is where
  "regenerate for a different take" actually should live musically.
- **A small, fixed scale set**, not a fully general theory engine — enough
  to deliver "generate a melody in this key/scale" without an open-ended
  taxonomy. More scales are a pure addition to `intervalsForScale` later.
- **Generated into the arrangement, not a Session slot.** Reuses
  `addClipToSelectedTrack`'s existing "append after the last clip"
  placement as-is. Session-slot generation (§19) is a natural next step but
  adds launch-quantum questions this slice doesn't need.
- **No `src/ai/` yet.** Every other JUCE-free helper in this codebase lives
  directly in `engine/`; `src/ai/` per §13 is reserved for when an actual ML
  runtime abstraction exists to justify its own layer. Symbolic ML
  generation (transformer melody/drum models, per §10) is the natural
  trigger for that graduation.

### Verification

`ScaleTests.cpp` and `GenerativeLoopTests.cpp` (headless, Catch2): scale
membership/snapping/degree math, Euclidean pulse-count/determinism/rotation
invariants plus the hand-verified tresillo case, and both generators'
seed-determinism, scale-membership, in-bounds, and density-scales-note-count
properties. All 322 tests pass; the bounce tool's full check suite,
including `rmsDry=0.149266`, is unchanged, plus the new
`generativeLoopWorks` check passes. The app builds warning-clean and starts
up without a crash. What can't be verified headlessly: the dialog's actual
appearance/layout, and whether a generated loop sounds *good* rather than
merely present and in-scale — try **Generate Loop...** on an Instrument
track and a Drum track and listen.

### Genre selection (implemented)

A follow-up request: let a **Genre** be chosen alongside root/scale/density,
where a genre both biases the generated pattern's rhythm feel *and*, for
Instrument tracks, swaps in a matching synth sound — reusing the existing
`model::SynthPreset` system (`MainComponent::applyPreset()`) rather than
inventing a second one.

**What was built:**

- **`src/engine/Genre.h`** — a six-way `Genre` enum (House, Techno, Hip-Hop,
  Trap, Ambient, Lo-Fi — the same "small, fixed set" scoping call already
  made for Scale) plus `rhythmProfileForGenre()`, hand-picked
  `{density, swing}` pairs per genre (e.g. Techno: busy and dead straight;
  Hip-Hop: sparse with heavy boom-bap swing).
- **`GenerativeLoop.h` gained a real `swing` parameter.** Both
  `DrumLoopParams`/`MelodicLoopParams` now carry `double swing = 0.0`,
  applied via the existing `NoteOps::quantizeNotes` — no new swing math, and
  the `0.0` default is behavior-preserving (the bounce tool's
  `generativeLoopWorks` check, which predates this, is bit-for-bit
  unchanged). **A real bug surfaced while writing this feature's own
  tests, not found by inspection:** a note near the *end* of a pattern,
  once swung later, could claim to sound past the pattern's own length —
  its `lengthBeats` had been sized against its original, unswung position.
  Both generators now clamp `lengthBeats` to what's actually left in the
  pattern after quantizing, right before returning.
- **`src/model/GenrePresets.h`** — `presetForGenre()`, six literal
  `SynthPreset`s built the exact way `seedFactoryPresets()` builds its four
  factory presets (same fields, same units), constructed in memory rather
  than written to the presets directory — there's no reason to clutter a
  user's saved-presets folder with six files nobody asked to save, when
  applying one is exactly as cheap as loading a file, minus the file I/O.
- **UI**: one more combo box, "Genre (overrides Density...)", in both the
  drum and melodic dialog branches, index 0 = "None" (today's exact
  behavior — untouched by this change). Picking a genre replaces the
  Density combo's value with that genre's rhythm profile and, for
  Instrument tracks only, folds a `synthSettings`/`effectChain` swap into
  the *same* `history_.edit(...)` call that adds the clip — one undo step
  for both, matching `applyPreset`'s own "a preset is one thing, not two
  separate edits" reasoning.

**The decisions worth recording:**

- **Genre overrides Density, doesn't blend with it.** Avoids adding
  show/hide interactivity to an `AlertWindow` — every dialog in this file is
  a static field list once shown (New Folder, Rename, Save Preset, and this
  one); a genre-aware live-updating Density field would be the first
  exception, for no real benefit over "the combo you picked wins."
- **The synth-preset swap is Instrument-only, not Guitar.** Guitar tracks
  are driven by `GuitarSettings`/`GuitarNode`, not `SynthSettings`, at all
  (§21) — there's nothing for a genre synth sound to apply *to* on a Guitar
  track. Genre still shapes a Guitar track's rhythm. A genre-appropriate
  amp/pedal choice for guitar is a real, separate feature this doesn't
  attempt.
- **Drum tracks: rhythm only, no sample swap.** Changing which *samples*
  a kit uses per genre is kit content, not kit-agnostic pattern generation
  — a distinct, larger feature than this slice.
- **Two separate lookups, not one combined table.** `rhythmProfileForGenre`
  (engine layer, pure numbers) and `presetForGenre` (model layer,
  referencing `engine::Genre` — consistent with the existing model→engine
  dependency direction, e.g. `Clip.h` already holds an `engine::Pattern`)
  stay independent, so the rhythm engine never needs to know what a
  `SynthPreset` is.

**Verification:** new `GenreTests.cpp` (rhythm-profile ranges, genres
actually differ, names distinct) and `GenrePresetsTests.cpp` (every genre
preset has a name, names are distinct, waveform in range, every effect
slot's `enabled` matches its own kind's `enabled` — the same invariant
`seedFactoryPresets()` maintains), plus new swing test cases in
`GenerativeLoopTests.cpp` (swing changes the pattern; the first/downbeat
onset — always an even grid step — is swing-invariant; every note still
fits inside the pattern after swinging, the regression test that caught the
length-clamp bug above). All 334 headless tests pass; the bounce tool's
full suite, including `rmsDry=0.149266` and `generativeLoopWorks`, is
unchanged. The app builds warning-clean. What can't be verified headlessly:
the dialog's actual layout with the new combo box, and whether each genre's
hand-tuned synth sound and rhythm feel actually reads as that genre by
ear — try a few genres on an Instrument track and a Drum track and listen.

---

## 26. New synth sounds: filter envelope, sub-oscillator, unison (implemented)

Two requests in one session pointed at the same gap: "make other sounds available to
the engine, starting with something that mimics Mutemath's sound," and "make
synthesizers and auto-generators that mimic the dark synth sound" of a linked
playlist (page metadata only — cyberpunk/synthwave/retrowave tags, not one song by
one artist; nothing here listens to audio). Both turned out to want the same missing
DSP: `engine::SynthVoice` had one oscillator, amplitude ADSR, and a **static** filter —
`StateVariableFilter::setCutoff()` was only ever called once per block, from fixed
`model::SynthSettings` fields, with no modulation input. A Moog-style bass's "pluck"
*is* a filter cutoff sweeping down over the note (a filter envelope); a synthwave
lead's size *is* several detuned oscillators stacked together (unison). Neither
existed. Both are genuinely reusable DSP once built — not one-off hacks for two
presets — so they were built as real synth capabilities, with two new factory
presets demonstrating them.

### What was built

- **A second, independent ADSR drives the filter cutoff.** `SynthVoiceSettings`
  (`src/engine/SynthVoice.h`) gained `filterEnvAmount` (Hz, bipolar) and its own
  `juce::ADSR::Parameters` — reusing `StateVariableFilter::setCutoff()`'s existing
  pure-function coefficient recompute (it's a TPT/Cytomic design specifically meant
  for time-varying coefficients, so calling it every sample is the *intended* usage,
  not a workaround) rather than adding new envelope math.
- **A sub-oscillator**: a fixed sine one octave down, mixed in for low-end weight
  (`subOscEnabled`/`subOscLevel`) — not a second selectable waveform, since a sine
  adds no harmonic content of its own to alias or clash against the main oscillator.
- **Unison**: up to `kMaxUnisonVoices = 7` copies of the main oscillator, detuned
  symmetrically across `unisonDetuneCents` and summed (`Oscillator::sample` called N
  times with N independently-advancing phases instead of once). Detunes pitch only —
  every voice already renders one mono sample duplicated to all output channels, so
  true stereo spread would be a separate change to how `SynthVoice` writes its
  output, not just how it generates a sample.
- **`applySettings` takes one `SynthVoiceSettings` struct**, not positional
  parameters — it was already at 7; adding 9 more would have made call sites an
  unreadable, error-prone wall of same-typed arguments.
- **`model::SynthSettings` gained the same 9 fields**, threaded through both
  serializers (`Serialization.h` `kFormatVersion` 25→26, `PresetSerialization.h`
  `kPresetFormatVersion` 1→2) the same tolerant-append way every prior field addition
  was — each reader variable pre-set to the real struct default (not 0) before the
  `>>` chain, so an old file simply stops filling them in at whichever field it
  predates.
- **`ScaleType::Phrygian`** — the quintessential "dark"/unsettled melodic color (the
  half-step above the root), one more `intervalsForScale` case.
- **`Genre::Synthwave`** — driving, straight rhythm profile (`{0.65, 0.0}`), wired to
  a new preset in `GenrePresets.h`.
- **Two new factory presets**: **"Analog Pluck Bass"** (`seedFactoryPresets()`) — a
  low base cutoff swept open by a fast, short filter envelope (the pluck *is* the
  sweep, not the amp envelope) plus a sub-oscillator, in the spirit of the
  analog-synth-bass tone Paul Meany plays in Mutemath — and **"Cyberpunk Stack"**
  (`GenrePresets.h`'s `Synthwave` case) — a 5-voice detuned saw stack under a slower
  filter sweep and drive, general cyberpunk/synthwave/retrowave character. Both names
  are descriptive; the actual creative reference lives in a code comment, not the
  product-facing name, so it's accurate about the inspiration without implying an
  official or licensed connection.

### The decision that mattered most: a fast path, not one unified render loop

This project treats several bounce-tool renders (`rmsDry=0.149266`, `rmsFiltered=
0.103703`, ...) as exact regression sentinels. Rather than writing one generalized
N-voice render loop that happens to reduce to the old behavior at `unisonVoices=1`,
`SynthVoice::renderNextBlock` keeps the **original single-oscillator code path
completely untouched** and branches to it whenever `unisonVoices <= 1 &&
!subOscEnabled && filterEnvAmount == 0` — the defaults, and therefore every existing
project and preset. The new general path (unison sum, sub-osc mix, per-sample filter
cutoff) is only reached once a preset actually opts in. Confirmed, not assumed: every
existing bounce-tool value printed bit-identical after this landed.

### Verification

Three new bounce-tool checks (`filterEnvChangesSound`, `subOscChangesSound`,
`unisonChangesSound`), matching the existing `chorusChangesSound`/
`driveChangesSound` "capability off vs. on, assert the renders differ" pattern —
`SynthVoice` is JUCE-dependent (`juce::SynthesiserVoice`, `juce::ADSR`) so this can't
be a headless Catch2 test, the same reason none of the other per-voice DSP in this
file is. No dedicated bounce check per *preset* — consistent with the four
pre-existing factory presets, none of which has one either; a preset is a data
combination of already-individually-verified capabilities. All 334 headless tests
pass (Scale/Genre/GenrePresets tests extended to cover Phrygian/Synthwave); the
bounce tool's full suite — including `rmsDry=0.149266` and `rmsFiltered=0.103703`,
proving the fast path really does preserve default behavior exactly — plus all three
new checks pass. The app builds warning-clean. What can't be verified headlessly:
whether "Analog Pluck Bass" and "Cyberpunk Stack" actually sound like what they're
going for — load them on an Instrument track and listen; also worth a project
save/reload to confirm the new fields round-trip.

---

## 27. MIDI recording (implemented)

`§2` has promised "multi-track audio **+ MIDI** recording" since the beginning, and the
audio half has been real and validated since the microphone-recording pass. The MIDI
half is not, and the gap is precise rather than vague: `AudioEngine::handleIncomingMidiMessage`
(`src/engine/AudioEngine.cpp`) pushes every incoming message into `midiCollector_`, the
audio callback drains it into `incomingMidi_`, and `processBlock` hands that buffer to
whichever track is armed — so a controller **plays** but is never **captured**. Nothing
in the codebase writes an incoming note into a `Pattern`. Every note in every project
to date has been drawn with the mouse or produced by `GenerativeLoop`.

This is the same shape of gap as the two the project has already closed twice (the
engine playing only clip zero of a track that modelled N; the audio-clip player doing
the same). The difference is that here the capability doesn't exist at all rather than
existing and being under-wired, so this is genuinely new surface — and it is the last
missing *verb* in the core DAW loop.

### The design rule this follows

The same one `AudioRecorder` established: **the audio thread is the only writer, the
message thread only reads what the audio thread has confirmed it is done with, and
nothing on the audio thread allocates.** MIDI makes this easier than audio did, not
harder — a take is a few thousand 16-byte events, not megabytes of samples, so there
is no disk streaming and no `ThreadedWriter`; a pre-allocated `rt::SpscRingBuffer` is
the whole mechanism.

### Two new modules, both JUCE-free

The valuable, subtle part of MIDI recording is *note pairing*, and it is pure math.
Following the precedent set by `SequencerMath`, `MetronomeMath`, `NoteOps`,
`PianoRollGeometry` and `Scale`, it goes in a headless, unit-tested module rather than
staying a private detail of a JUCE class — which is why, unlike `AudioRecorder` (JUCE
all the way down, and therefore only ever verifiable through the bounce tool), this
feature can be tested properly by Catch2.

1. **`engine/MidiCapture.h`** — the pairing. Two POD event types (`RecordedMidiEvent`,
   timed in samples, as captured; `TimedMidiEvent`, timed in beats, after the caller
   has converted through the tempo map) and one pure function turning a flat event
   stream into `Note`s. It never sees a `juce::MidiMessage` and never sees a
   `TempoMap`: the engine translates JUCE messages into PODs on the way in, and the
   caller converts samples to beats on the way out. That leaves a function whose whole
   contract is expressible as "these events in, these notes out".

   The cases it must get right — each one a test:
   - A note-on pairs with the **next** note-off of the same pitch.
   - **A note-on with velocity 0 is a note-off.** Standard MIDI running-status
     convention; a large fraction of real controllers send this, and missing it would
     mean every note from those controllers records as held-forever.
   - **A note still held when the take ends** gets its length clamped to the end of the
     take rather than being discarded. Someone holding the final chord when they hit
     Stop must not lose it.
   - A note-off with no matching note-on is ignored (it happens whenever a key was
     already down when recording started).
   - The same pitch retriggered before its first note-off closes them **FIFO** — the
     first on pairs with the first off. Chosen over LIFO because it keeps note lengths
     in the order they were played.
   - Zero- and negative-length results are clamped to a musical minimum rather than
     emitted, since a `lengthBeats <= 0` note is silent in the sequencer and invisible
     in the piano roll: it would look like a dropped note, not a short one.
   - Output is sorted by start time, because the ring buffer's order is arrival order.

2. **`engine/MidiRecorder.h`** — the capture state machine, deliberately mirroring
   `AudioRecorder`'s lifecycle (`arm`/`disarm`/`isFinished`/lead-in skip/dropped
   counting) so there is one recording discipline in the codebase, not two. It holds
   an `rt::SpscRingBuffer<RecordedMidiEvent>`; `process()` is called from the audio
   callback with the block's events, and the message thread drains continuously.

   Two decisions worth recording:
   - **The message thread drains on a timer, not only at the end of the take.** A
     fixed ring big enough for a long take would be a hidden cap of exactly the kind
     the audio recorder's history warns about — the take that comes back short with no
     error. Draining continuously into a plain `std::vector` on the message thread
     means the ring only ever has to hold one timer interval's worth of playing, and
     the take itself is unbounded.
   - **Overflow is counted, never swallowed** (`droppedEventCount()`), for the same
     reason `AudioRecorder::droppedSampleCount` exists. A take missing a note it
     cannot report is worse than one that failed outright.

### Which recorder the Record button drives

Today the button always records audio: with a Guitar or Instrument track selected it
records a microphone take onto a **new audio track**, because a synth track cannot
hold an audio clip. That is now the wrong answer to the right question.

**The rule becomes: the record type follows the armed track's type.** An Audio track
records audio, through the existing, proven, entirely untouched path. An
Instrument/Drum/Guitar track records MIDI into a clip on that track. This needs no new
UI, no mode switch to forget the state of, and it makes the Record button mean the
same thing the rest of the app already means by "armed". It is a behaviour change for
the synth-track case only, and in that case the old behaviour was close to useless.

### Where the notes land

On stop: a **new `ClipType::Instrument` clip appended to the armed track**, starting
at the beat where capture actually began (from the recorder's latched start playhead,
after any count-in — the same value and the same reasoning as the audio path's
`startPlayheadSamples`), committed through `history_` as one undoable step.

Two things deliberately deferred, and named here so they are not mistaken for
oversights:
- **Overdub/merge into an existing clip.** Punching into a part and having the new
  notes join the old clip is what a user eventually wants, but it needs a merge policy
  (replace the punched range? union? per-pitch?) that deserves its own pass. A new
  clip is what audio recording already does, it is consistent, and multi-clip tracks
  are proven.
- **Record quantize.** `NoteOps::quantizeNotes` already exists and already has a UI
  path, so a recorded take can be quantized immediately after with a machine that is
  already tested. Doing it *during* the commit would mean the raw performance is
  unrecoverable behind one undo step.

Clip length rounds **up to the next whole bar** (via the tempo map's
`quartersPerBar()`), not to the last note. A take is a musical phrase; ending the clip
on the final note's release would make a loop of it jarringly short.

### Verification

- Headless Catch2 for both new modules — every pairing case above, plus the recorder's
  armed/lead-in/finished transitions and its overflow counting. This is the part that
  can be tested properly, and it is also the part most likely to be subtly wrong.
- The bounce tool gets a check that a *recorded* pattern (built from a synthetic event
  stream, committed the way the app commits it) renders to audible sound through the
  normal synth path — proving the produced `Pattern` is real musical data and not just
  a struct that passes its own tests.
- `rmsDry=0.149266` and the rest of the existing suite must be unchanged: nothing in
  this pass touches the signal path, and the audio-recording path is not modified at
  all.
- **What cannot be verified headlessly, and needs a live try:** whether a real
  hardware controller reaches the callback and records in time — the same single open
  question microphone recording ended on, for the same reason.

### Build order

1. `engine/MidiCapture.h` + tests (pure pairing).
2. `engine/MidiRecorder.h` + tests (capture state machine).
3. Wire into `AudioEngine`: a `midiRecorder_` member, capture from `incomingMidi_` in
   the audio callback, and a `beginMidiRecording`/`drain`/`finish` API mirroring the
   audio one.
4. Wire into `MainComponent`: route the Record button by armed track type; on finish,
   convert samples to beats through `uiTempoMap_`, pair, and commit the clip.
5. Bounce-tool check; README/PLAN updates.

### What actually landed, and the one thing the plan got wrong

Built as designed, in the stated order, with one correction the bounce tool
forced — and it is the interesting part of this pass.

The obvious way to skip a count-in is what `AudioRecorder` does: while the
lead-in is outstanding, drop the block and count it down. For audio that is
merely imprecise (capture starts up to one buffer late). For MIDI it is
**wrong**, and the `midiRecordingWorks` check failed on the first run because of
it. The lead-in almost never ends on a block boundary, so the block it ends
*inside* gets discarded whole — and that block contains the downbeat the
count-in was counting to, which is precisely where a player puts their first
note. The most common note anyone records was being silently dropped.

`MidiRecorder::process` now subtracts a partial lead-in *within* the block and
starts capturing mid-block at that offset: events before it belong to the wait,
events at or after it belong to the take. The take's start playhead is latched
as the exact sample the count-in ended on rather than the start of the
containing block, which also makes the recorded clip land on the downbeat
instead of up to a buffer early. A dedicated unit test now pins this
("A note on the count-in downbeat is captured, not swallowed").

Worth recording because the same latent flaw is still in `AudioRecorder` — it
is far less serious there (a few hundred samples of silence at the head of a
take, not a lost note), which is presumably why nothing has ever caught it, but
it is the same mistake.

A second thing caught in review before it shipped: `AudioEngine::captureMidi`
returns early when no take is armed, which is right for cost but was initially
guarded on `! isArmed()` alone. `MidiRecorder::process` is what publishes
`finished_` after a take is disarmed, so that guard would have meant no take
ever finished and the UI stuck mid-record forever. The guard is now
`! armed && isFinished()`, and the buffer walk — not the `process()` call — is
what a disarmed-but-unfinished take skips. This is the same failure mode
`AudioRecorder` documents at length in its own `! armedNow && ! finished_`
branch; encountering it independently in the mirrored class suggests the shape
is inherent to the pattern rather than incidental.

### Verification as run

- **562 headless tests pass** (up from 539: 12 new `MidiCapture` pairing cases,
  11 new `MidiRecorder` lifecycle cases including the count-in downbeat and the
  ring-overflow counting).
- **The bounce tool's full suite passes, including the new
  `midiRecordingWorks`** — a synthetic performance driven through the whole
  chain (blocks with a count-in → ring → drain → samples-to-beats → pairing)
  and rendered through the normal synth path, asserting the notes land on the
  beats they were played, sound, and stop when they were released.
- `rmsDry=0.149266` and `rmsFiltered=0.103702` are unchanged, as are all 50+
  other checks: nothing in this pass touches the signal path, and the
  audio-recording path is not modified at all.
- The app builds warning-clean.
- **Still unverified, and only a live try can settle it:** whether a real
  hardware controller reaches the callback and records in time. The capture
  logic is proven against synthetic input, exactly as microphone recording was;
  what remains is the same open question that pass ended on.

### Correction: the record-source decision, and hot-plugged MIDI

The routing rule above — "the record type follows the armed track's type" — was wrong
as shipped, and testing found it immediately: with the default Instrument track
selected and no controller attached, Record armed a *MIDI* take on a machine that had
only a microphone, captured nothing, and looked like a dead button.

The mistake is worth naming precisely, because the rule reads sensibly right up until
it fails. **"What can this track hold?" and "what is there to record from?" are two
independent questions**, and the rule answered only the first. A track's type
constrains what a take can *become*; it says nothing about whether a source exists.

Both are now inputs to `app::RecordSourceChoice` — a JUCE-free module holding the whole
decision table, with every row tested (including the exhaustive invariant that no
decision ever selects a source that isn't connected). The behaviour:

| Armed track | MIDI connected | Audio input | Result |
|---|---|---|---|
| Instrument/Drum/Guitar | yes | either | record MIDI |
| Instrument/Drum/Guitar | no | yes | record audio to a new track, *and say so* |
| Instrument/Drum/Guitar | no | no | refuse, naming both possible fixes |
| Audio | either | yes | record audio |
| Audio | either | no | refuse, naming mic permission if opening failed |

The fallback row is the one that matters: it restores exactly what the app did before
MIDI recording existed, rather than inventing a third behaviour, and it reports itself
in the status bar — a Record button that quietly does something other than what the
armed track implies is worse than one that explains itself.

Recording audio while a MIDI-capable track is armed *and* a controller is connected is
deliberately not reachable: arm an audio track for that. That is the standard DAW
answer, and inventing a modifier or a mode toggle to express it would be exactly the
"mode to forget the state of" this design set out to avoid.

**A second, independent bug surfaced while investigating it.** MIDI inputs were
enumerated once, in the `AudioEngine` constructor, and never again — so a controller
plugged in after launch was invisible to the app for the entire session: it could not
record, and could not even *play*. This predates MIDI recording (it made the existing
live-monitoring path silently useless too) and was only noticeable once anything
depended on a controller being present. `AudioEngine::refreshMidiInputs()` now
re-enumerates, registering arrivals and unregistering departures, tracking registered
identifiers rather than re-deriving them from the device list — registering the same
device twice would deliver every message twice, and an unplugged device is no longer
*in* that list, so it could never be unregistered (which also fixes the destructor
leaking callbacks for devices unplugged mid-session). It runs on a slow cadence from
the UI timer (every two seconds — enumeration is a system call and the timer is 30Hz)
and again whenever a take is armed.

569 headless tests pass (7 new); the bounce suite is unchanged, `rmsDry=0.149266` and
`midiRecordingWorks=1` included.

### Prompting for microphone permission at the moment it matters

The "no audio input — check microphone permission, then restart" message was accurate
and useless. It described a fix rather than offering one, and the restart it demanded
was not even necessary.

The underlying macOS behaviour is what makes this awkward. The OS shows its microphone
prompt **once**, when an app first opens an input — and this app opens its input in the
`AudioEngine` constructor, so that prompt lands at launch, before anyone has a reason to
care, and is trivially dismissed. After that the OS never asks again: CoreAudio reports
a device with no input channels forever, which from inside the app is indistinguishable
from having no microphone at all. Nothing the app does can re-trigger the prompt.

JUCE is no help here — `RuntimePermissions::request` is an Android-only implementation
that returns true everywhere else, which `src/app/CMakeLists.txt` already documented for
the Info.plist key. So `app::MicrophonePermission` is a small platform shim:
`microphonePermission()`, `requestMicrophonePermission()` and
`openMicrophonePrivacySettings()`, implemented over AVFoundation in an Objective-C++
translation unit on macOS and as an honest `NotRequired` everywhere else (Windows and
Linux have no per-app gate an application can query — a blocked device there simply
appears as no device, which the existing path already reports).

Pressing Record now resolves the permission before deciding anything is impossible:

- **Never asked** → request it, which is the OS prompt appearing at the moment the user
  actually wants to record. On a grant, the input is re-opened and **the take starts by
  itself** — answering a prompt should not mean pressing Record a second time. Guarded
  by `retryingAfterMicPermission_` so a grant that still yields no usable input reports
  that instead of looping.
- **Denied** → a dialog with an **Open Settings** button that goes straight to the
  Privacy & Security microphone list, rather than a sentence describing where it lives.
- **Granted but no input open** (permission switched on in Settings *after* launch, the
  case the old message called for a restart) → `AudioEngine::reopenAudioInput()` re-asks
  the device manager for input, falling back to output-only exactly as the constructor
  does if it still fails. Playback is never the price of asking.

Two ordering decisions that are easy to get backwards:

1. **A MIDI take is decided before any permission question.** A controller needs no
   microphone, and prompting for one would be a non-sequitur.
2. **Permission is settled before "nothing is connected" is treated as final.** A denied
   microphone presents as a device with no input channels — i.e. exactly like absent
   hardware. Checking in the other order reports a one-click fix as a missing device,
   which is the same class of mistake as the record-source bug above: acting on a
   symptom that two very different causes share.

**Not verifiable headlessly, and not unit-tested:** every branch here is either an OS
call or a modal dialog. The decision *table* that feeds it is tested
(`RecordSourceChoiceTests`); this layer is glue over `AVCaptureDevice` and
`NSWorkspace`, and only a live run on a machine in each permission state can confirm it.
What is checked mechanically: the built bundle carries `NSMicrophoneUsageDescription`
and links AVFoundation and AppKit.

---

## 28. A starter song worth listening to (implemented)

The old starter song was three tracks, one bar each, all starting at bar 1 and looping
forever: a generic 4/4 beat, a drop-C chug riff, and whatever pattern the piano roll
happened to default to. Its job was to prove the app made sound, and it did that. What
it did not do was demonstrate that the app is a *DAW* — the timeline showed three
identical stubs at the origin, and nothing about it suggested arrangement, mixing or
structure. It also wasn't really music: the guitar was rooted on a drop-C low string
and the synth clip was in no key at all, so the two clashed.

### What it is now

Sixteen bars in C minor, i-VI-III-VII, one chord per bar, arranged as a build:

| Bars | Drums | Bass | Guitar | Lead |
|---|---|---|---|---|
| 1-4 | intro (kick + hats, no backbeat, snare pickup into bar 5) | | | |
| 5-8 | full groove | ✓ | | |
| 9-12 | full groove | ✓ | ✓ | |
| 13-16 | groove + sixteenth-note fill | ✓ | ✓ | call, then answer |

Parts *arrive*, which is the point: on first launch the timeline shows a staircase of
clips across four tracks instead of three stubs stacked at bar 1, and playing it
demonstrates multi-clip gating, the mixer, the drum kit, a hosted-quality guitar chain
and two synth presets without anyone having to go looking for them.

### The decisions worth recording

**Everything is derived from the guitar's lowest open string.** That is the one pitch
here which isn't free — the Modern Metal preset is drop tuned, and a riff must land on
a string the instrument actually has (the existing reason
`makeDefaultGuitarRiffPattern` takes the open note as a parameter rather than assuming
standard tuning). So the key root *is* that note, the bass sits an octave above it and
the lead two octaves above, and all four parts are in one key by construction rather
than by coincidence. This is precisely what the old starter song got wrong.

**The progression is entirely diatonic** (i-VI-III-VII in natural minor), which is what
lets the lead be written from the scale alone without having to dodge a chord tone. A
test pins it, because it is an assumption the melody depends on rather than a
preference.

**The four-bar groove repeats `makeDefaultDrumLoopPattern` rather than restating it**,
so "the basic 4/4 beat" still has exactly one definition and the starter song cannot
drift away from it. In the same spirit, the riff's fourth bar *is*
`makeDefaultGuitarRiffPattern` transposed onto the VII chord — not an arbitrary reuse:
that figure already ends by walking up a minor third and a fourth, which from the VII
lands back on the tonic exactly where bar one begins. Both functions would otherwise
have become dead code the moment the starter song stopped calling them.

**The lead is two clips, not one.** A track holding a single clip loops it
indefinitely; a track holding more than one gates each to its own window. Splitting the
lead into a call and an answer is both the more musical shape and what makes the
starter song demonstrate that gating — and, less obviously, it is what stops the lead
looping forever past the end of the song.

**Drum clips tile contiguously across all sixteen bars.** With more than one clip on a
track the silence between clips is real, so a gap here would be a hole in the song
rather than a harmless spacing choice.

**"New Project" is deliberately unchanged** — it still resets to a single blank synth
track. Someone who explicitly asks for a new project wants a clean canvas, not a demo,
and that distinction predates this change.

### Verification

All the musical claims are headless, JUCE-free and tested — the material lives in
`engine/DefaultContent.h` for exactly that reason. 582 tests pass (13 new): every bass
and lead note is in the natural minor scale; each bar's downbeat is that bar's chord
root; the guitar riff stays between the open low string and the last fret, and
transposes with the tuning rather than against it; the intro withholds the backbeat
until its pickup; the fill is genuinely finer than the eighth-note grid and nothing
overruns its cycle; the answering phrase resolves onto the tonic so the loop closes.
The bounce suite is untouched (`rmsDry=0.149266`), since none of this is signal path.

**What cannot be checked headlessly:** whether it actually sounds good, and whether the
clip staircase reads well on the timeline — `makeStarterSong` assembles the arrangement
inside `MainComponent`, so only launching the app confirms the placement. Worth
listening to end to end, and worth soloing each track.

---

## 29. Tempo-aware audio clips (implemented)

`§1` calls this a **loop-centric** DAW. It is not one yet, in the specific sense that
matters most: an imported loop does not follow the project's tempo. Drop a 174 BPM
break into a 120 BPM song and it plays at 174, against everything else.

That is not an oversight so much as a deliberate decision that has outlived its
context. `AudioFilePlayerNode` keeps its read position in samples and says why:
"the read position below stays in samples, which is what stops a tempo change from
stretching the audio." That was right when the alternative was a *bug* — audio
accidentally warping when someone edited the tempo map. It is wrong as a permanent
answer, because following the tempo is exactly what a loop should do.

The only tool for it today is `applySpeedAndPitch`: the user works out the ratio
themselves, and it **destructively rewrites the samples**. Change the project tempo
afterwards and you do it again, from already-degraded audio.

### Why this is mostly connection, not invention

Every hard piece already exists:

- `engine/TimeStretch.h` has a real phase vocoder (`timeStretch`, `pitchShift`) —
  pitch-preserving, already unit-tested.
- `engine/Fft.h` and `engine/Spectrum.h` give the analysis machinery for onset
  detection, with no new dependency.
- `TempoMap` already expresses musical time properly.
- `ClipData` is fully decoded in RAM and handed to the audio thread as a
  `shared_ptr` swapped under the existing lock-free discipline, and
  `AudioEngine::setTrackAudioClips` already caches decoded audio per file path.

### What gets built

**1. `engine::TempoDetect` — JUCE-free BPM estimation.**
Spectral-flux onset envelope, autocorrelation across a plausible tempo range, then
explicit octave-error resolution — the classic failure of every naive tempo detector is
reporting 87 for a 174 BPM loop (or vice versa), and it has to be handled deliberately
rather than hoped away. Returns a BPM *and a confidence*, so a weak result can be
offered as a suggestion instead of silently applied as a wrong answer.

This module is where the testing story is unusually good for DSP: synthetic click
trains at known tempos have an exact expected answer, so detection accuracy is a
headless Catch2 assertion rather than something only checkable by ear. The octave cases
get tests of their own.

**2. `model::Clip` gains `sourceBpm` and `warpEnabled`** (format bump to `LOOPER 12`).
Both default to "off / unknown", so every existing project plays back byte-identically
— the same fast-path discipline `SynthVoice` used for the new synth DSP.

**3. Playback follows the tempo, without the audio thread ever stretching anything.**
The stretch is **pre-rendered on the message thread** whenever the ratio changes and
swapped in as a new `ClipData`, cached per file+ratio. Phase vocoding is emphatically
not a real-time operation, and doing it in the callback would violate the engine's
first rule. This is the same "prepare on the message thread, publish a pointer" pattern
every other node here already uses.

**4. UI.** Import reports what it found ("Detected 174 BPM - warped to 120"); a per-clip
**Warp** toggle; and a "Set project tempo from this clip" action for when the loop
should lead rather than follow.

### Deliberate v1 boundaries

- **Warp against a single constant project tempo.** With a tempo map the ratio becomes
  time-varying, which means a different (and much larger) rendering strategy. Deferred
  explicitly, and a clip in a project with tempo changes will warp against the tempo in
  force at its start rather than silently doing something wrong.
- **No per-transient warp markers.** Ableton-grade scope; a single ratio per clip is
  what makes a loop usable, and it is the 90% case.
- **Detection is a suggestion, never a silent rewrite.** Nothing is applied
  destructively, and warp can always be switched off to hear the original.

### Build order

1. `engine/TempoDetect.h` + tests — self-contained, and worth stopping to check the
   detection accuracy before anything depends on it.
2. `model::Clip` fields + serialization round-trip test (`LOOPER 12`).
3. Warp rendering + the cache, and the playback ratio.
4. UI: import reporting, the Warp toggle, "set tempo from clip".
5. A bounce-tool check that a warped clip really does line up with the grid.

### What landed, and the two things worth recording

Built as designed, in the stated order. Two decisions turned out to matter more than
expected.

**Pre-rendering the stretch meant `AudioFilePlayerNode` needed no changes at all.**
This was the design's real payoff and it is worth being explicit about why: a clip
warped on the message thread arrives at the audio thread as an ordinary buffer that is
simply the right length. The player already plays a buffer at its native rate, so a
4-beat loop stretched from 174 to 120 BPM lasts exactly 2 seconds and lands on the grid
without the player knowing warping exists. The alternative — a playback ratio the
player applies itself — would have put resampling in the callback and changed pitch,
and phase vocoding there is not an option at all.

**Octave errors could not be fixed by folding the result into a preferred range**, which
is what the plan implicitly assumed. The first implementation reported a 174 BPM loop as
87, and 87 is a perfectly ordinary tempo — there is nothing out of range to notice.
Whichever of the two wins is decided by noise. The fix is to prefer the *fastest*
interpretation the envelope genuinely supports: if half the winning lag correlates
nearly as well (≥ 0.8), there really are onsets at that rate. A 76 BPM loop has almost
no correlation at half its lag, so its slower reading survives — both cases now have
tests. The range fold stays, but only for what the subdivision step deliberately
overshoots (hats on sixteenths support a reading four times the pulse).

A third, smaller correction: **confidence saturated at 1.0 for audio with no onsets at
all.** Measuring only "how far does the peak stand above the mean" is meaningless once
bias removal has left an essentially zero envelope — the ratio is numerical noise, and
can be arbitrarily large. Confidence is now periodicity *times* peakiness, where
peakiness is the raw envelope's crest factor measured before bias removal. A drum loop
is mostly silence with spikes; a held chord is flat.

### Decisions made while building

- **Recorded takes are never analysed or warped.** A take was just played against this
  project's own click, so it is at the project tempo by definition; detection could only
  agree (pointless) or disagree (wrong — and a confident mis-detection would stretch the
  performance the user just gave). Its `sourceBpm` is still recorded as the project
  tempo, which keeps it useful if the tempo later changes.
- **Auto-warp on import requires confidence ≥ 0.5 *and* a tempo that actually differs.**
  A low-confidence result is still stored — it costs nothing and makes the menu item
  available to accept by hand — but is not acted on, so a sustained pad that happens to
  correlate at 91 BPM is never silently stretched. Either way the status line says what
  happened.
- **The warp cache holds one rendering per file path, not per (path, factor).** Keying
  by both grows without bound as someone drags the tempo around, and every superseded
  entry is a whole decoded file held for a tempo nobody is at. Changing tempo re-renders;
  sitting at one costs a single rendering.
- **Warping is a tick, not an action.** Unlike the existing "Speed and pitch", it never
  rewrites samples — switching it off returns the original audio exactly.

### Verification

602 headless tests pass (23 new). The detector is tested against synthetic click trains
with exact expected answers, including both octave-error directions, a backbeat pattern
whose *pattern* repeats at half its beat rate, silence, too-short input, and the
confidence ordering. `warpStretchFactor` has its own tests, including the inversion —
source-over-project, the easiest mistake here and the hardest to notice, since the wrong
answer is still "in time". Serialization round-trips the new fields, and a file written
without the `CLIPWARP` record still reads as unwarped.

The bounce tool's new `warpFitsTheGrid` covers what none of those can: that
detect → factor → stretch *composes* into audio of the right musical length, rather than
three individually correct steps that disagree about which direction "faster" is. Every
existing check is unchanged, `rmsDry=0.149266` included — the audio thread is untouched
by this work.

**What cannot be checked headlessly:** how warped audio actually *sounds*. The phase
vocoder is not transparent, and a loop stretched a long way will smear — worth dragging
a real loop in at a few different project tempos and listening, especially something
percussive where smearing is most audible.

---

## 30. Sidechain compression (implemented)

Routing was flat: every track to the master, plus one shared send bus. No track could
listen to another, which made the pumping compressor — the sound underneath most house,
techno and hip-hop, i.e. the audience `§1` names first — impossible to build here.

### The DSP was already right; only the routing was missing

`Compressor::gainFor(detectorInput)` (`engine/PedalDsp.h`) already takes its detector as
an explicit argument and *returns* a gain rather than applying one. That was written so a
stereo pair could share one detector, but it is exactly the shape a sidechain needs: the
detector was never assumed to be the signal being compressed. Nothing in the DSP changed.

What was missing was getting another track's audio to it, and that turned out to be
cheap for a reason already in the design: **every track already renders into its own
`scratch` buffer** before summing into the mix. A track's isolated signal exists during
its own render; it simply wasn't readable by anyone else.

### How it is wired

- `EffectProcessor::setSidechainInput()` — a virtual with a no-op default, pushed once
  per block, following `setBpm`'s stated precedent exactly ("a default here rather than
  widening `process()`'s signature, so the other seven nodes' call sites don't have to
  thread through a value none of them read"). Only `CompressorNode` overrides it.
- `InstrumentTrack::blockOutput()` publishes this block's rendered audio, taken **after
  its inserts but before its fader** — the same point the send is taken from, and for
  the same reason: pulling a fader down should change how loud a track is, not how hard
  it ducks something else.
- `AudioEngine` routes source → target and orders the render so sources come first.
- The document stores a **track id**, not an index (`CompressorSettings::sidechainTrackId`,
  format v35). Indices move when a track is deleted or reordered, and a sidechain
  silently re-pointing at a different instrument is the kind of bug nobody would think to
  look for. `MainComponent::trackIndexForId` is the only place that bridges the two.

### The decisions worth recording

**Render order is only rearranged when a sidechain actually exists.** Float addition is
not associative, so summing the same tracks in a different order changes the mix in the
last bits — and a project with no sidechain must render bit-identically to how it always
has. The `rmsDry=0.149266` sentinel would have caught it, which is precisely why the
guard is there rather than reordering unconditionally.

**Two passes, not a topological sort.** Sources first, then everyone else, each in index
order. A full sort is the general answer, but with one detector per track the only case
it buys is a chain of sidechains (A ducks B ducks C), and against that it would have to
define what a cycle means. Anything deeper than one level reads the silence fallback
below rather than misbehaving.

**A source that produced nothing yields silence, not self-detection.** Muted, soloed
out, or not yet rendered — the compressor gets a permanently-empty buffer. Falling back
to compressing its own input would be worse than doing nothing: the track would change
character for a reason the user never asked for, exactly when they muted the thing that
was supposed to be ducking it.

**Export parity came free.** `processBlock` is shared by live playback and
`renderOffline`, so a bounce ducks exactly as playback does — none of the deferral that
per-track automation needed.

### Verification

The new `sidechainDucks` bounce check is the one that matters, and it is written to fail
the way this feature would actually break. A compressor that quietly falls back to its
own input still compresses and still passes every "does it reduce gain" test while being
completely useless — so the check compresses a **constant-amplitude tone** with a
separate pulsing detector: a steady tone can only dip periodically if the detector really
is the other signal. It asserts at least 6 dB of duck at each pulse, recovery between
them, and — as a control — that the same tone with no sidechain routed comes out steady.

605 headless tests pass (3 new): the routing round-trips as an id, an unrouted compressor
stays unrouted, and a file written before v35 reads as unrouted. Every existing bounce
check is unchanged, `rmsDry=0.149266` included.

**Not verified:** how it sounds musically, and the combo box itself — set a bass track's
compressor to duck from the drum track and listen. **Group buses are still absent** and
remain the next routing gap: this adds one detector per track, not a bus you can
compress as a unit.

---

## 31. Group buses (implemented)

The routing gap §30 left open: a sidechain gives one track a detector, but there was
still no way to treat several tracks as one — no drum bus to compress as a unit, no
single fader for "all the drums", no shared reverb insert for a group.

### The decision that made this small: a bus is a track

The obvious design is a `Bus` entity alongside `Song::tracks`, with its own list, its own
mixer strips, and its own selection. That was rejected after looking at what a bus
actually needs: a fader, pan, mute, a meter, an insert chain, automation, a name, a
colour, serialization, and a mixer strip. A `Track` already has every one of them.

Worse, a parallel entity would need a parallel *selection model* — `selectedTrackIndex_`
appears 174 times in `MainComponent.cpp`, and every pane keys off it. Teaching all of
that to mean "a track or a bus" is a large, risky change with nothing musical to show
for it.

So a bus is `TrackType::Bus`: a track that **receives** other tracks' output instead of
generating any. Everything downstream — the mixer strip, the Track FX pane, selection,
undo, automation, serialization, metering — works unchanged, because from their point of
view nothing new exists. What differs is one field on its members
(`Track::outputBusId`) and one branch in `InstrumentTrack::render`.

This is the same shape as the sidechain pass: the expensive-looking feature was cheap
because the existing design already had the right pieces, and the work was noticing that.

### How it renders

- The engine **clears each bus's buffer at the top of the block**, before any member
  runs. The bus cannot do it itself: it renders last, and clearing then would discard
  the entire group. This is the one genuinely delicate part, and it is what the bounce
  check below is aimed at.
- Members render **into their bus's buffer** instead of the mix.
- Buses render **last**, and skip content generation entirely: their buffer already
  holds everything routed in. Inserts, gain, pan, mute and metering then apply exactly
  as for any track — which is how the drum bus gets a compressor without a line of new
  UI.
- Ordering composes with §30's: sidechain sources, then ordinary tracks, then buses.
  Buses stay last whatever else is true.

### The decisions worth recording

**A bus ignores other tracks' solo.** Soloing a kick has to keep playing *through* the
drum bus; a bus silenced by the standard "solo overrides" rule would take its members
with it. Correspondingly a bus's own solo is not counted when deciding whether anything
is soloed — otherwise arming it would mute every real track while the bus carrying them
stayed open, which is silence with no visible cause. A bus's **mute** does work, and
mutes the whole group.

**Routing is stored as a track id, not an index** — the same reasoning as the sidechain
source, and the same single bridge (`trackIndexForId`) between the document's ids and
the engine's positional pool.

**Stale routing degrades to the master.** The engine re-checks every block that the
target still exists, is still a bus, and is still active. A routing left pointing at a
track that has stopped being a bus writes to the master rather than into another
instrument's scratch buffer.

**Buses cannot feed buses, and a stem ignores routing.** Both are rejected in
`setTrackOutputBus`/`processBlock` rather than left to produce a loop the audio thread
would have to detect every block. A stem is one track in isolation, so it goes straight
out — routing it through a group would make a stem the group's sound, not the track's.

### Verification

The new `groupBusWorks` bounce check drives real `InstrumentTrack`s in exactly the order
the engine uses, and targets the four ways this breaks — each of which is silence or a
doubled signal rather than a subtle difference:

- a unity bus passes the group through **intact** (within 2% of the same content routed
  straight to the mix) — proving the bus neither cleared what it was given nor generated
  on top of it,
- the bus's fader at −6 dB halves the **whole group**,
- muting the bus silences the group, not just the bus,
- and the direct render is non-silent in the first place, so none of the above passes
  vacuously.

607 headless tests pass (2 new): routing round-trips as an id, and a file written before
v36 reads as feeding the master. Every existing bounce check is unchanged,
`rmsDry=0.149266` included — render order is only rearranged when a bus or sidechain
actually exists, precisely so that stays true.

**Not verified:** the mixer's new **Out** picker and **Add Bus** button, and how a
compressed drum bus sounds. Add a bus, route the drum tracks into it, put a compressor
on it from the Track FX pane, and listen.

**Still deferred:** buses feeding other buses (one level only), and per-bus sends.

---

## 32. Automation you can see and draw (implemented)

Automation has been real in the engine for a long time — written by touching a fader
with **Rec Auto** armed, ramped sample-accurately across each block, exported correctly
— and completely **invisible**. Nothing drew a curve. `AutomationLane` could only ever
*gain* points: it had `addPoint`, `valueAt` and `clear`, and no way to find a point
again, let alone move or delete one. The only way to fix a fader move that went slightly
wrong was to clear the lane and perform it again.

### Why a pane, not a lane in the arrangement

The obvious home is a row under each track in the arrangement, and that is where this
was originally going to go. It was moved after reading `ArrangementView`: it already
carries **three drag protocols** — clips, files dragged from the browser, dock panels —
whose separation is load-bearing enough that the code documents how they are told apart
(by the dragged component's *type*, deliberately not by a string). Adding a fourth
gesture that shares a coordinate space with clip dragging is how you get a click that
moves a clip when it meant to add a breakpoint.

An automation *pane* has its own coordinate space and its own gestures, touches none of
that code, and the dockable workspace exists precisely so it can sit next to the
arrangement rather than instead of it. It takes its x axis from the same
`TimelineGeometry` the arrangement uses and follows the same zoom and playhead, so a
breakpoint lines up with the bar above it rather than approximately with it.

### What was built

- **`model::AutomationLane` gained editing**: `indexNear` (find the point under the
  cursor), `removePointAt`, and `movePoint` — which returns the point's *new* index,
  because dragging one past its neighbour reorders the lane and a caller tracking "the
  point I am dragging" has to be told where it went. Sorted order is not cosmetic here:
  `valueAt` binary-searches it, so an out-of-order lane doesn't merely look wrong, it
  reads wrong.
- **`app::AutomationGeometry`** — JUCE-free value↔pixel mapping, per-parameter ranges,
  and radius hit-testing, unit-tested headless like `TimelineGeometry` and
  `PianoRollGeometry`. Gain is edited over −60…+6 dB rather than a fader's full travel:
  a lane spending half its height between −60 and −inf dB would waste it on differences
  nobody can hear.
- **`app::AutomationPane`** — the curve, its breakpoints, the default-value line, bar
  lines, and the playhead. Click empty space to add a point (and keep dragging it, so the
  common gesture is one movement), drag to move, right-click to remove.

### The decisions worth recording

**One undo step per gesture, not per breakpoint.** The pane reports the whole lane when a
drag *ends* — the same rule the mixer faders already follow. Reporting per drag event
would put hundreds of entries in the undo history for one move.

**The lane is passed whole rather than as a delta.** The edits here are add/move/remove
on a sorted list; replaying those against the document would mean implementing the same
sort twice, in two places that must agree.

**An emptied lane is erased, not stored empty.** A track with no automation carries no
lanes at all — the state every serialization and playback path already treats as "use
the static value" — so clearing a lane returns the track to exactly the shape it had
before anything was automated.

**Values before the first point and after the last are drawn as held.** That hold is
what actually plays; a curve starting at the first breakpoint would misrepresent the
entire span before it.

### Verification

Nine new GUI tests drive the real component through real `juce::MouseEvent`s: a click
adds a point, a click in the toolbar does not, a click with no track selected does
nothing, a drag moves the point that was grabbed (and does not add a second), a
right-click on a point removes it, a right-click on empty space adds nothing — the case
where "remove" would otherwise silently become "add" whenever you missed — and an edit
is reported exactly once, when the gesture ends. 624 headless tests pass (17 new across
the lane's editing operations and the geometry).

**An existing test caught a real defect in the previous pass.** `PaneWiringTests`
asserts every control on a mixer strip reports *something* when touched; the group-bus
**Out** picker failed it, because it was added visible-by-default with no items in it —
a control that is on screen in every fresh project, has nothing to choose between, and
reports nothing when clicked. It now starts hidden and appears once a bus exists, and
both that test and the layout test were extended to cover it properly rather than
excluded. This is the second time this session that a test written for an earlier
feature has caught a later one.

**Not verified:** how the curve reads on screen, and whether the pane's default dock
position is sensible. Open the **Automation** panel, select a track, draw a fade, and
play it.

**Still deferred:** automation for effect and hosted-plugin parameters — `TrackParam`
was designed for exactly that extension ("a new enumerator plus the code that applies
it"), and the pane's parameter picker is where they would appear.

---

## 33. Making the guitar sound better (implemented)

The guitar is already well past a naive Karplus-Strong: a fractional-delay waveguide
with pitch-compensated decay (§21), a pickup RLC resonance, a cabinet with a
cone-breakup peak and notch, ADAA oversampled shaping (§23), palm mutes and hammer-ons.
So what is left is specific rather than vague, and reading the code says exactly what.

Three things are missing that a listener hears immediately, and three that are deeper
work. Phase 1 is the first three.

### The gaps, in order of audible payoff per unit of work

1. **Every note is timbrally identical.** `GuitarString::pluck` scales *amplitude* by
   velocity and nothing else — `pickHardness_` is a static setting. On a real
   instrument, picking harder is *brighter*, not merely louder. This is the single
   biggest reason a programmed part sounds machine-gunned.
2. **The six strings are completely independent.** `GuitarNode` sums them and nothing
   more. There is no bridge coupling, so struck strings never excite the others and
   nothing rings sympathetically — most of why a modelled chord sounds like six separate
   notes rather than one instrument.
3. **It is mono.** One sum is copied to both channels, so the guitar has literally zero
   width.
4. **No inharmonicity.** A waveguide is perfectly harmonic; real strings are stiff and
   their partials stretch progressively sharp. Most audible low and under distortion —
   a real component of "growl".
5. **The cabinet still is not an impulse response**, as `CabinetSim` says of itself. Two
   of the three reasons it gives have since expired: `engine/Fft.h` now exists, and an
   IR can be *synthesised* rather than shipped, so nothing has to be licensed.
6. **One waveshaper is not an amp.** Real amps cascade gain stages with filtering
   between them; each stage shapes what the next one distorts.

### Phase 1: dynamics, coupling, width

**Velocity to timbre.** The excitation's lowpass smoothing becomes a function of the
velocity as well as the static hardness, plus a small per-pluck jitter of the pick
position so no two plucks comb identically.

Expressed as a *deviation from a reference velocity* rather than as a new absolute
mapping: at the reference (0.8, which is `engine::Note`'s default and what every
generated pattern uses) the excitation is bit-identical to today's. That keeps this an
addition rather than a retune of everything already written, the same discipline
`SynthVoice`'s fast path follows.

**Bridge coupling.** Strings meet at the bridge, which is not perfectly rigid: energy
crosses between them there. Modelled as a fraction of the summed bridge signal injected
back into every string's loop each sample.

Two consequences worth planning for rather than discovering:
- A string that is not ringing has to be *processed anyway* once coupling is on, or it
  can never start ringing sympathetically — which is the entire effect. The existing
  "skip silent strings" optimisation is therefore conditional on coupling being off.
- The feedback path must not be able to grow. With a coupling coefficient *k*, the
  worst-case string-to-string-and-back loop gain is on the order of *k²* times the
  string count, so a conservative *k* keeps the existing "no string may grow over 30
  seconds" guarantee intact — and that guarantee gets a test at the coupled setting.

**Stereo width, by panning the strings.** Not a delay, not a chorus: each string is
spread slightly across the field and the two sums are filtered by identical pickups.
Because a linear filter distributes over a weighted sum, that is *exactly* equivalent to
filtering each string and then panning, so it costs one extra filter instance and no
correctness. It is also **mono-compatible by construction** — the strings are distinct
signals, not copies of one, so folding to mono cannot comb-filter, which is precisely
what a Haas delay would do.

### What this deliberately changes

Existing guitar tracks **will** sound different — coupled, wider, and more dynamic. That
is the request rather than a regression, and it is called out here because this
codebase's usual rule is the opposite. The velocity mapping is the one part held
identical at its reference point, so already-written parts keep their balance.

### Verification

- Headless: spectral centroid must rise with velocity; a struck string must put
  measurable energy into an untouched neighbour, and none when coupling is off; no
  string may grow over 30 seconds at the coupled setting; the mono sum of the widened
  output must not lose level (the mono-compatibility claim, measured).
- Bounce tool: the guitar's two channels must differ once width is up, and be identical
  at zero width.
- `rmsDry` and every other sentinel must not move: none of this is on the non-guitar
  path.

### Phase 1 as built, and the three things measurement changed

Built as planned — velocity to timbre, bridge coupling, string panning — with new
`velocitySensitivity`, `stringCoupling` and `stereoWidth` on `model::GuitarSettings`
(format v37, seeded from the defaults so an older project loads the *improved*
instrument rather than the old one), sliders on the fretboard pane, and the engine
plumbing to match.

Three things were wrong on the first attempt, and each was caught by a test rather than
by listening. They are worth recording because all three are the same shape: a change
that looked local turned out to be coupled to something already measured.

**1. Coupling did nothing at all.** `couple()` added the injected signal into
`buffer_[writeIndex_]` — which is the slot `process()` is about to *assign*, not add to.
Every injected sample was silently discarded, and the neighbouring string received
exactly zero energy. It now accumulates into a pending value that joins the loop's input
where the string's own feedback enters. The test that caught it asserts an untouched
string ends up ringing, which is the whole point of the feature and would have been
impossible to notice by ear as "slightly less bloom".

**2. Jittering the pick position was a tonal change, not a variation.** The intent was
that no two plucks comb identically. But the comb's notches fall on real harmonics: it
is what nulls the even ones when you pluck at the midpoint, and a large part of what
makes a palm-muted note dark. Two independent checks moved — the pick-position test
(which measures the midpoint null directly) and the bounce tool's palm-mute brightness
ratio, which went from 0.82 to 0.96 against a 0.9 threshold. The jitter now varies the
pick's *hardness* instead: that shapes how bright the burst is, which is exactly the
"no two plucks alike" quality wanted, and leaves every comb property intact.

**3. The jitter's random draw corrupted the excitation.** Taking one sample from
`nextNoise()` to compute the jitter shifted every subsequent sample of the noise burst,
changing the entire realisation of each note — enough to flip a controlled A/B
comparison in the pick-position test. Pick variation now has its own generator. The
lesson generalises: any new draw from a shared RNG silently re-rolls every measurement
downstream of it.

**And one measurement error of my own.** The first brightness test measured a 250ms
window starting at 10ms and reported almost no difference between a soft and a hard
pluck — not because velocity wasn't working, but because the loop's damping filter
dominates the spectrum within a few hundred milliseconds. Velocity shapes the
*excitation*, so it has to be measured across the attack; the test now uses a 30ms
window and a first-difference energy ratio, which is blind to how loud the note is.

### Verification

630 headless tests (6 new): picking harder is measurably brighter; sensitivity 0
restores the old behaviour exactly; a note at the reference velocity is
*sample-identical* with sensitivity at any setting; a struck string makes an untouched
neighbour ring; with coupling off it stays exactly silent; and a coupled pair at nearly
twice the coupling the node ever applies still decays rather than growing.

Two new bounce checks: `guitarHasWidth` (channels identical at width 0, genuinely
different above it, **and** the mono fold keeps its level — the mono-compatibility claim
that justifies panning over a delay) and `guitarStringsCouple` (a coupled chord's tail
carries more than an uncoupled one). Every existing check passes unchanged, `rmsDry` and
the palm-mute sentinel included.

**Not verified: whether it actually sounds better.** That is the whole point of the pass
and the one thing no test here can answer. Load a Guitar track and play a chord — the
Dynamics, Coupling and Width sliders are on the fretboard pane, and setting all three to
zero returns the instrument to exactly what it was before.

Phases 2 and 3 (inharmonicity; then cascaded amp stages and a synthesised cabinet IR)
remain as described above.

### Phase 2 as built: string stiffness

An ideal string is perfectly harmonic — partial *n* at exactly *n* times the
fundamental, which is what a plain waveguide gives and part of why one sounds
synthetic. A real string resists bending, which makes it **dispersive**: high
frequencies travel faster, arrive early, and end up progressively sharp. It is
strongest on thick wound strings, and it is a genuine component of a drop-tuned
guitar's growl — the partials of a stiff low string beat against each other instead of
locking into a clean stack.

Modelled the standard way: a cascade of first-order allpasses inside the loop, with a
**negative** coefficient so the delay falls with frequency. A positive one flattens the
partials instead, which sounds like a detuned string rather than a stiff one, and is the
easiest sign error available here. `GuitarNode` scales it per string — the thickest gets
the full amount, the thinnest a third — because giving all six the same value makes the
top strings sound out of tune rather than stiff. Exposed as **Stiffness**
(`model::GuitarSettings::stiffness`, format v38).

### What measurement forced, again

**The first attempt did essentially nothing: 0.06 cents of stretch at the 8th partial.**
A first-order allpass's phase delay is nearly flat until well up towards Nyquist, so
with a modest coefficient the entire guitar range sits in the constant part of its
response. Getting the dispersion to act *in the audible band* meant both a much stronger
coefficient (a pole near 0.9, whose time constant is a handful of samples) and eight
sections rather than four. This is the sort of thing that reads as "add an allpass" in a
paper and is a measurement problem in practice.

**Then high notes went out of tune.** Each section's delay is a fixed number of
*samples*, but a note's period is not: at 440 Hz the string is only ~109 samples long,
and eight sections wanted ~125. The delay line went negative, the clamp took over, and
the pitch was whatever fell out. The cascade is now **budgeted against the note's own
period** — a quarter of it — so only as many sections run as actually fit. High notes get
less stiffness, which is the graceful failure rather than a wrong one: they have fewer
audible partials to stretch anyway. That budget is what makes the 2-cent tuning
guarantee hold at every stiffness setting, which is now itself a test across four
stiffness values, six strings and four frets.

### Verification

638 headless tests (4 new): partials stretch sharp and *progressively* so (the 8th
further than the 4th); at stiffness 0 the string stays harmonic within 2 cents, so this
is opt-in rather than a retune; the fundamental stays within 2 cents at every stiffness,
string and fret — the compensation check; and a maximally stiff string still decays,
since the allpasses sit inside a feedback loop and "unity gain in theory" is worth
measuring once.

One new bounce check, `guitarStiffnessChangesTone`, drives the real `GuitarNode` so the
per-string scaling is covered too. All existing checks pass unchanged, `rmsDry` and the
palm-mute sentinel included.

**Not verified: whether it sounds like a stiffer string or merely a different one.**
Play a low drop-tuned chug with **Stiffness** at 0 and at 100% and listen for growl
rather than detuning — if it reads as out of tune, the coefficient is too high for
taste, not wrong in kind.

Phase 3 (cascaded amp stages, then a synthesised cabinet IR now that `engine/Fft.h`
exists) remains.

### Phase 3a as built: cascaded gain stages

`DriveEffect` was one clipper. A real amp is two or three gain stages with a coupling
network between them, and the difference is not "more distortion" — it is a different
kind. `DriveSettings::stages` (1..3, format v39) selects it; 1 is exactly the previous
behaviour, so every existing project and preset is untouched and the amp-like tones opt
in. The Modern Metal preset's amp half is now a three-stage cascade.

Between stages sit a highpass (~120 Hz) and a lowpass (~6.5 kHz). The highpass is the
important one: it takes the bass out *before the next clipper sees it*, which is what
makes a high-gain amp tight rather than muddy, because low strings otherwise
intermodulate with everything above them and no EQ afterwards separates them again. The
whole cascade runs inside a single oversampler call rather than one per stage, and the
interstage coefficients therefore exist in two versions — running base-rate coefficients
at 4x would put both corners two octaves low and quietly revoice the amp.

### Two wrong claims, both caught by measuring

**"A cascade compresses more."** The first version shared the drive out as the n-th root
so the total push stayed constant, and asserted the output would grow less when the
input doubled. It measured the opposite (1.64 vs 1.46): three gentle stages have a
*softer* composite knee than one hard one, so the output grows more freely. The claim
was wrong, not the code.

**"...so it must at least be denser."** Rewriting the check to measure high-order
harmonic content showed the cascade with **a tenth** of the single stage's — 0.0039
against 0.0369. Three stages at 12^(1/3) barely clip at all. The n-th root idea was the
real error: **a cascade is not a gentler route to the same distortion, it is more
distortion of a different shape**, and each stage in a real preamp has its own full
gain. With every stage driven fully the same measurement gives **0.487 against 0.037** —
thirteen times denser — and that is now the check.

What stops that being merely a louder fuzz is the interstage filtering. Clipping is
close to idempotent: a second clipper handed the first one's output would do almost
nothing. Reshaping the wave in between is what gives the next stage something to work
on, and it is where the character comes from.

**Then the preset clipped.** Giving the Modern Metal amp three stages meant dropping its
drive from 75 to 22 — which *raised* its output, because make-up gain falls as
1/sqrt(drive), and the tone started peaking above full scale. The bounce tool caught it
as `wetPeak > 1`; the preset's level came down from 2.0 to 1.05. The retuned tone
measures louder and with more cut than before (wetRms 0.28 → 0.36, presence 0.39 →
0.43) and less low-mid weight (body 0.28 → 0.15, against a 0.02 floor) — tighter, which
is what the interstage highpass is for, but it is the number to watch if it now sounds
thin.

### Verification

`cascadedStagesEnrich` measures harmonics 5-9 against the fundamental for a single stage
and for three, and requires the cascade to be at least 10% denser; it is 13x. All 634
headless tests, 173 GUI tests and every existing bounce check pass, `rmsDry` included —
`stages` defaults to 1, so nothing that did not opt in can have moved.

**Not verified: whether the retuned Modern Metal preset is better.** It is measurably
denser, louder and tighter; whether that is an improvement is a listening question, and
the halved `body` figure is the specific thing to listen for.

**Phase 3b — the synthesised cabinet IR — remains.**

### Phase 3b as built: a synthesised cabinet impulse response

`CabinetSim` said of itself that it was "still not an impulse response: a convolution
would be more faithful and would need an IR to ship, a partitioned convolver and a
latency story." All three objections are answered rather than accepted:

- **Nothing is shipped.** The response is *synthesised* at prepare time, so there is no
  asset and no licence.
- **No partitioning and no latency.** It is ~10ms and convolved directly in the time
  domain. Partitioned FFT convolution only earns its complexity on responses long enough
  to make direct convolution expensive, and it pays for that with a block of latency —
  which on a guitar someone is playing is the one cost you cannot accept. Ten
  milliseconds is where a close-mic'd cabinet keeps essentially all its character; what
  follows is the room, which a close mic barely hears.

**What it adds that a filter cannot.** An IIR filter has one path from input to output:
it shapes magnitude, but it cannot represent the same sound arriving *twice*. A real
cabinet does exactly that — straight off the cone, again off the baffle edge (inverted,
as a diffraction is), again off the back of the box — and those arrivals comb with one
another and smear the cone breakup in time. That is what the ear reads as a speaker in a
box with a microphone in front of it.

**The response is built by running the impulse train through the cabinet's own
filters**, so the two modes are voiced identically by construction. Restating the
response instead would have been a second description of the cabinet, free to drift from
the first.

### The level-matching mistake

Switching modes must change the sound and not the volume, or every comparison anyone
makes — by ear or by measurement — measures the level difference instead. The first
version matched **DC gain**, and the preset immediately clipped (`wetPeak` 1.02, caught
by the bounce tool). The reflections partly *cancel* at DC — they sum to about 0.79
there — so holding DC constant scaled every other frequency up by a quarter. Matching
**energy** is what level-matching an IR actually means, and with it the preset came back
to 0.76 peak.

### Verification

12 new headless tests: the direct arrival is at tap zero (the zero-latency property that
makes this usable while playing); each reflection lands where its geometry says; the
baffle reflection really is inverted, since a train with every arrival positive is just
a brighter cabinet rather than a combed one; the response is deterministic, because a
cabinet that differed per launch would be a baffling bug; the convolver is an identity
for a unit response and computes a hand-checkable convolution for a delayed one;
normalising matches energy; the two modes are within ±40% on broadband noise; they are
audibly *different*; and the cabinet defaults to its filter chain, so nothing that has
not opted in changes.

646 headless tests, 173 GUI tests, and every bounce check pass, `rmsDry` included.

**One existing test had quietly stopped testing anything.** "A file written before
sidechains reads as unrouted" simulated an older file by stripping the *last* field from
the positional `FXSLOT` line — so the moment v39 and v40 appended two more, it was
stripping the cabinet-IR flag and asserting a sidechain that was still there. It now
drops a named count of fields, with a comment that any future appended field has to be
counted too. Worth recording as a hazard of positional formats: a compatibility test
written against "the last field" decays silently every time the format grows.

**Not verified: whether any of it sounds better.** That is the whole question and no
test here answers it. The Modern Metal preset now runs a three-stage cascade into a
convolved cabinet; play it, and compare against Stiffness/Coupling/Width at zero and
`stages` back at 1, which is exactly the instrument as it was.

---

## 34. A piano instrument (steps 1–4 implemented)

There is no piano. `TrackInstrument` is Synth, Drum or Guitar, and a piano part today
means a synth patch approximating one — which is the one instrument nobody accepts an
approximation of, because everyone has heard a real one.

### Modelled, not sampled

This project does not ship recordings. `DrumSynth` says so in as many words — procedural
one-shots "so the app can ship a starter kit without bundling (or licensing) anyone
else's recordings" — and §33 synthesised a cabinet impulse response rather than shipping
one. A convincing sampled piano is hundreds of megabytes to several gigabytes of
multi-velocity-layer recordings, and a licence.

But the stronger argument is that **most of a piano string already exists**.
`GuitarString` is a fractional-delay waveguide with pitch-compensated decay, damping,
bridge coupling (§33 phase 1) and, since §33 phase 2, **dispersion** — the stiffness that
stretches partials sharp. Inharmonicity is *the* defining feature of piano tone, far
stronger there than on a guitar; that file's own comment already notes the guitar's
stretch is "subtle compared to a piano's". The instrument is largely a re-use of
machinery that is already written and already tested.

### What actually differs from a guitar — the real work

**1. A hammer is not a pluck.** A pluck sets an initial displacement and combs it by
where the pick was. A hammer is a felt mass in *contact* for a brief time, and the
contact shortens as the blow gets harder — so a loud note is not merely a louder quiet
one, it is dramatically brighter, and nonlinearly so. That is the whole expressive range
of the instrument. The §33 velocity-to-hardness mapping is the right shape but the wrong
model: the excitation itself has to become a force pulse whose width depends on
velocity.

The strike *position* matters too, and is not arbitrary: pianos are struck between 1/7
and 1/9 of the string's length specifically to suppress the seventh partial, which would
otherwise be dissonant against the rest. That is a real design fact and belongs in the
model rather than as a tuned constant.

**2. Two or three strings per note, slightly detuned.** One string in the low bass, two
in the tenor, three in the treble. This is what produces the piano's shimmer — and, more
importantly, its **double decay**: the strings are coupled at the bridge, exchange energy
with one another, and the result is a fast initial decay followed by a much longer
"aftersound". A single decaying exponential does not sound like a piano and no amount of
EQ makes it one. The bridge-coupling mechanism from §33 phase 1 is exactly the machinery
this needs, pointed at strings of the same nominal pitch instead of different ones.

**3. Stiffness that varies across the range.** A piano's inharmonicity is not one number:
it is high in the short thick bass strings, lowest through the middle, and rises again in
the short treble. `GuitarNode` currently scales stiffness by *string index*; the piano
needs it as a function of *pitch*.

One known limit to measure rather than discover: §33's dispersion cascade is budgeted
against the note's own period, so only as many allpass sections run as fit in a quarter
of it. A bass note at A0 (27.5 Hz, ~1750 samples) has budget to spare — which is
fortunate, since that is where the stretch is largest — but the top octave has almost
none. Whether the treble ends up stiff enough is an open question this plan should
answer with a measurement, not an assumption.

**4. Dampers, and a sustain pedal.** A guitar string rings until replucked; a piano note
stops when the key is released. So note-off must *damp*, which is the opposite of the
guitar's default. Beyond that:

- **The sustain pedal (CC64) lifts every damper**, letting all strings ring and resonate
  sympathetically with whatever is played. With bridge coupling already built, this comes
  out of the model rather than being faked with reverb — which is the single most
  convincing thing a modelled piano can do.
- **There is no MIDI CC handling anywhere in the engine today.** `Pattern` holds notes
  and nothing else, so a pedal has nowhere to live in a `Clip` either. This is genuinely
  new surface — a data-model change, a serialization change, and a playback path — and
  it is the largest non-DSP piece of this feature.
- The top two octaves of a real piano have no dampers at all.

**5. Range, and the CPU question.** 88 notes from A0 (27.5 Hz) to C8. `GuitarString`
sizes its buffer for ~30 Hz, so A0 does not fit and that bound has to come down.

More seriously: with the pedal down, a passage can leave thirty notes ringing, each with
up to three strings, each string running an eight-section allpass cascade. That is a
different order of cost from six guitar strings, and it is the one design risk here that
is not about how it sounds. It needs a voice cap, voice stealing, the existing
skip-silent-strings trick, and — unlike most things in this codebase — an actual
measurement of worst-case load before the design is committed to.

**6. A soundboard.** The guitar's last stage is a `Pickup` (an RLC resonance). A piano's
equivalent is a soundboard, whose response is far too complex for a few poles. §33 phase
3b already built the machinery for exactly this problem: a synthesised impulse response,
convolved directly, no asset and no latency. Reusing it here is the natural symmetry.

### Deliberately not in v1

Una corda (the soft pedal), key-release noise and the thump of the key bed, sympathetic
resonance from *silently depressed* keys, half-pedalling, and per-note tuning curves
(stretch tuning). Each is real and each is a refinement of something this plan builds
first.

### Build order

1. **Hammer excitation** in the string model — added alongside `pluck`, not replacing it,
   so every existing guitar test keeps passing unchanged. Verifiable immediately: a
   harder strike must be brighter, the way the guitar's velocity test measures.
2. **`PianoNote`** — one to three detuned strings coupled at a shared bridge. This is
   where the double decay must appear, and where it is measured.
3. **`PianoNode`** — the voice pool, note-on/off with dampers, and voice stealing, with
   the worst-case CPU measurement taken here before anything is wired up.
4. **The sustain pedal**, end to end: `Pattern` gains pedal events, serialization
   follows, the sequencer emits them, and the node lifts its dampers.
5. **The instrument as a track**: `TrackType::Piano`, a pane, presets, and starter
   content.

### Verification

Most of this is measurable, and the guitar harness extends to it almost unchanged:

- **Tuning** across all 88 notes, within the same 2 cents every other tuning claim here
  holds to.
- **Inharmonicity** against the textbook stretch formula, *and* that it varies correctly
  across the range rather than being a single constant — including whether the treble
  gets enough, per the budget limit above.
- **Double decay**: a three-string note's envelope must show a fast initial segment and a
  slower aftersound; a one-string note must *not*. This is the check that says the thing
  sounds like a piano, and it is the one to write first.
- **Hammer dynamics**: a harder strike is brighter, not merely louder.
- **Dampers**: a note-off silences within a few tens of milliseconds; with the pedal
  held, it does not; and a note in the top octave rings regardless.
- **Sympathetic resonance**: with the pedal down, striking one note must put measurable
  energy into an untouched neighbour — the same shape as §33's coupling test.
- **Stability**: no string may grow over 30 seconds at maximum coupling, the guarantee
  that already exists and that a shared bridge makes harder to keep.
- **CPU**: worst-case polyphony measured, not assumed.
- `rmsDry` and every other sentinel unchanged: this is a new instrument and must not
  touch the existing render path.

### Step 1 as built: hammer excitation

`GuitarString::strike(velocity)` sits alongside `pluck`, and the pluck path is untouched
— every existing guitar test passes unchanged, which was the point of adding rather than
generalising.

What the two share is now shared in code as well as in principle: the comb, the placement
relative to the write pointer and the state reset were identical for both, so they moved
into one `commitExcitation`. Only the shape of the burst differs, which is exactly the
physical distinction — a pluck sets *displacement* and lets go, a hammer stays in
**contact** for a moment.

The expressive part is that contact time is not fixed. A harder blow compresses the felt,
the hammer leaves sooner, and the string keeps far more high-frequency energy — so a loud
note is a *brighter* note rather than a louder one. That is intrinsic here rather than an
optional mapping the way `velocitySensitivity` is for a pluck: a hammer that ignored
velocity would not be a hammer. Felt hardness (`setHammerHardness`) shortens contact the
same way, which is why a brightly voiced piano sounds bright even played gently.

Two details worth recording:

- **The force pulse is a raised cosine, not a square one.** Felt compresses; it does not
  strike like a hammer on an anvil, and a step in the string reads as a click.
- **The strike-position comb is what removes the pulse's DC**, and that matters more for
  a hammer than a pluck. A one-sided force pulse is full of DC, and the loop filter has
  unity gain at DC by design — so an offset would sit in the string and decay only as
  slowly as the note itself, heard as a thump under every key. It gets a test of its own
  rather than being left as a property that happens to hold.

The strike position reuses `setPickPosition`, because it is the same geometry: the wave
leaves in both directions and the near reflection returns inverted. A piano is struck
between a seventh and a ninth of the way along precisely to put that comb's notch on the
seventh partial, and the tests strike at an eighth.

### Verification

Six new headless tests: a harder blow is measurably brighter (the defining property);
harder felt is brighter at the same blow; a struck string sounds and decays; a strike
leaves no DC offset; striking does not move the pitch, to the same 2 cents everything
else here holds to; and a hammer does not produce the same note as a pick, which would
make the whole distinction decorative.

652 headless tests pass, every bounce check included `rmsDry` is unchanged, and nothing
outside `GuitarString` was touched — `strike` has no caller yet, which is what step 2
(`PianoNote`: the coupled, detuned strings and the double decay) is for.

### Step 2 as built: the unison, and the double decay

`engine::PianoNote` is one key: the one to three slightly detuned strings a hammer
strikes together, joined at a shared bridge. Two pure functions go with it —
`pianoStringCount` (single-strung through the low bass, double through the upper bass,
triple from the tenor up) and `pianoStiffness`, which is **U-shaped** across the
keyboard: high in the bass where the wire is short and very thick, least through the
middle where the scaling is most ideal, climbing again in the top octaves. That shape is
what a real instrument measures, and neither a constant nor a simple slope reproduces it.

**The bridge loads the strings; it does not feed them.** Each string receives a fraction
of the summed bridge motion *with the sign reversed*. In phase that is a strong damping
term — the strings drive the bridge hard and genuinely lose energy through it; out of
phase their forces cancel at the bridge, there is almost nothing to damp, and what is
left rings on. That is the double decay, and the sign is the whole thing: feeding the sum
back in phase would reinforce exactly the motion that is supposed to be dying, and the
note would have an *anti*-decay rather than an aftersound.

This is the same coupling §33 built for the guitar, pointed at strings of the same
nominal pitch instead of different ones.

### The control that failed, and what it taught

The first test set asserted that a trichord shows a double decay and that an **uncoupled**
trichord shows none. The second failed: an uncoupled trichord's decay was 15.7 dB/s early
and 7.8 dB/s late — a pronounced double decay with no bridge involved at all.

The reason is straightforward once measured: three *detuned* strings beat against one
another, so the summed peak falls faster at first than any single string does. **Detuning
alone buys part of the effect**, and the original test would have passed just as happily
with the coupling deleted — which is the worst kind of test, since it would have reported
success for a feature that had stopped existing.

The check is now comparative: the coupled trichord must fall away faster *relative to its
own aftersound* than the uncoupled one. That isolates what the bridge actually
contributes, and it fails if the coupling is removed. The main test's comment was
corrected too — it claimed the bridge as the sole cause of something both mechanisms
produce.

### Verification

Ten new headless tests: a trichord decays fast then slowly; a single string decays at one
rate (nothing to be out of phase with); the bridge deepens the double decay beyond
detuning alone; a unison beats, with the envelope measurably *rising* again, which no
single decaying exponential ever does; a damper stops the note within a few tens of
milliseconds and an undamped one keeps ringing; adding strings does not shift the pitch,
since the detuning is spread symmetrically about it; a coupled unison cannot grow over 20
seconds at maximum coupling; the string counts match the instrument; and inharmonicity is
U-shaped.

`GuitarString`'s pitch floor also came down from 30Hz to 25Hz — a piano's bottom A is
27.5Hz, and a string that cannot reach its own lowest note is not a limit anyone would
think to look for.

662 headless tests, 173 GUI tests and every bounce check pass, `rmsDry` unchanged.
`PianoNote` has no caller yet — step 3 (`PianoNode`: the voice pool, dampers, stealing,
and the worst-case CPU measurement) is next.

### Step 3 as built: the voice pool, and what the measurement changed

`engine::PianoNode` is a pool of `PianoNote`s built like `GuitarNode` — atomics read once
per block, MIDI rendered in spans so a note lands on the sample it was scheduled for —
with the differences the instrument dictates: **note-off damps** (the normal end of a
piano note, where a guitar ignores note-offs on purpose), a voice is a *key* rather than a
string, decay time falls steeply with pitch (a low A sustains for the best part of a
minute, a top C for a second), the keyboard is panned low-left to high-right, and the top
octave has **no dampers at all** — those notes ring on past the key release, which is a
small thing and audible.

**The CPU question the plan flagged is answered, and the answer was "not a problem".**
24 voices of bass — the longest strings and deepest stiffness cascades — rendered at 32x
realtime. On that evidence the pool was raised to **48**, because that is what the sustain
pedal needs and the measurement says it is affordable; 48 voices measures 13.7x realtime.
This is the one number in the piano that was never going to be settled by listening.

### Three bugs, and two of them were older than the piano

**A five-note chord peaked at 2.9.** A piano is played in chords far more than a guitar
is, so its headroom has to assume them; `kOutputScale` does the same job GuitarNode's 0.4
does for six strings.

**Decay time did not mean what it said at the top of the range.** `updateLoopGain` solved
for the requested T60 assuming the rest of the loop was lossless, but the damping filter
is in the loop too. Its per-pass loss is a fraction of a percent — negligible for a
guitar — but a 2.6kHz note goes round thousands of times a second, so it compounded into
a note that asked for three seconds and got a tenth of one. `setDecaySeconds` documents a
time that "holds across the range", and it did not. It now divides out the filter's gain
at the fundamental.

**And that was not enough, because the filter was damping the fundamental itself.** Up
there the fundamental sits where the filter cuts, so the compensation above asked for a
loop gain over unity and got clamped — the note still died. The damping is now *capped*
so the filter's gain at the fundamental stays above 0.9995, solved in closed form. Below
about 1kHz the cap never binds, so nothing in the guitar's range changes; above it, the
top of a piano keeps the brightness a short string actually has.

Both of those were latent in `GuitarString` from the beginning. Neither was reachable
from a guitar, whose highest note is half a piano's top C — building an instrument that
goes higher is what exposed them.

**Then the treble blew up to NaN.** Making the loop nearly lossless is what surfaced the
third: the bridge coupling injected the *broadband* string sum as feedback, and a delay
turns negative feedback positive at every frequency landing half a period out. The loop's
own damping had been swamping that, and once it stopped, a treble note grew each pass. The
bridge now **lowpasses what it passes back at ~320Hz** — which is the physics rather than
a patch, since a real bridge is mass-controlled and barely responds above a few hundred
Hz. It is also why the top octave couples weakly, rings on, and needs no dampers.

### Verification

Four new bounce checks: the piano sounds and stays in range; a released chord is stopped
by its dampers; a note in the top octave rings on past its release; and full polyphony
holds above 5x realtime, so a piano track leaves room for the rest of a project. 662
headless tests, 173 GUI tests and every existing check pass, `rmsDry` and the guitar's
sentinels included — the string changes were measured to leave the guitar's range
untouched rather than assumed to.

`PianoNode` still has no caller: step 4 is the sustain pedal (the new MIDI-CC surface,
and the largest non-DSP piece), then step 5 wires it up as a track type.

### Step 4 as built: the sustain pedal, and the soundboard

The largest non-DSP piece, as expected: `Pattern` had notes and nothing else, so a pedal
had nowhere to live. It now carries `PedalEvent`s (beat, down) — the first performance
data in this engine that is not a note — which `PatternPlayback` emits as ordinary
**CC64**, so anything that understands MIDI understands them, including a hosted plugin
that would otherwise need a private channel. Serialization follows (`PEDALS`/`PEDAL`,
format v41), and `flush` lifts the pedal **unconditionally**: sending it when it was
never pressed costs one ignored message, while a stuck pedal would leave every later note
ringing forever with no visible cause.

`PianoNode` holds a damper state per voice — `heldByKey` — separate from whether the
voice is claimed. With the pedal down a release marks the key up but the damper never
reaches the string; lifting the pedal damps everything no longer held, including notes
released minutes earlier. That is exactly what a pianist hears on the lift, and it is why
a pedal cannot be modelled as a longer decay.

**And the soundboard.** With the dampers up, every ringing string is driven by what the
board is doing, so a struck chord makes the rest of the instrument answer. This falls out
of the model rather than being faked with reverb, and it is the most convincing thing a
modelled piano does.

### Two runaways and a worthless test

**The soundboard exploded.** Summing every voice's bridge motion made the feedback gain
proportional to how many keys were down: ten voices ringing meant ten times the loop gain
a single note implied, and a pedalled chord grew to a peak of **1327** before collapsing.
It now uses the board's *average* motion, which makes the loop gain independent of
polyphony — the only form of it that can be reasoned about at all. The same chord now
peaks at 0.22.

**And the stability check passed anyway.** It asserted only that the late peak was below
the early one, which a signal that explodes and then collapses satisfies perfectly. The
check is now on the absolute level, so a runaway fails it.

**The resonance check was measuring the wrong thing entirely.** It compared a *released*
note pedalled against unpedalled — which measures the pedal holding the note, something
`pianoPedalSustains` already covers, and it would have passed with the soundboard gain set
to zero. It now holds both keys down for the whole render, so the notes ring either way
and sustain cannot account for any difference; what is left is the soundboard. The
measured difference is 26% of the note's own level.

That is three tests in this feature that passed for reasons unrelated to what they were
named after. The pattern is consistent enough to be worth stating: a check that compares
"feature on" against "feature off" measures whatever *else* differs between those two
renders, and on this instrument something else usually does.

### A limit worth recording

The sympathetic path only drives strings that are **already ringing**, because silent
voices are skipped for cost. A real pedal-down piano also resonates strings that were
never struck, and doing that would mean keeping every undamped voice running. That is a
real difference, it is deferred rather than overlooked, and the test above measures what
was actually built rather than what the feature is named after.

### Verification

Four new bounce checks — the pedal sustains a released chord (tail ten times the
unpedalled one), lifting it damps everything, the soundboard measurably couples held
notes, and a big pedalled chord stays bounded for twelve seconds — plus two serialization
round-trips. 664 headless tests, 173 GUI tests, every existing check unchanged.

Step 5 remains: wiring it up as a track type, with a pane, presets and starter content.
