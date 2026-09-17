# SoundSplice — Feature Roadmap

How SoundSplice gets from where it is today to feature parity with **Audacity**, **Adobe
Audition** and **REAPER**, and past them. `PLAN.md` is Looper-Audio's historical plan; this file
is the current one.

Sources surveyed (September 2026): the Audacity features page and its manual's index of effects,
generators and analyzers; Adobe Audition's feature summary on GetApp and its user guide (Essential
Sound, diagnostics, restoration); and REAPER's home and `about.php` pages.

Legend: ✅ have · 🟡 partial · ⬜ missing · 🔁 removed on purpose (decide before re-adding)

---

## 1. Where SoundSplice stands

What already exists, so the plan doesn't rebuild it:

| Area | Today |
| --- | --- |
| Editing | Waveform editor for **one clip at a time**: cut, copy, paste, delete, trim, split, silence, fade in/out, reverse, clip gain, normalize (by peak, as clip gain), speed/pitch, zero-crossing search (`audioedits::nearestZeroCrossing`, not yet a UI option) |
| Repair | Spectral noise reduction with a noise print |
| Effects (real-time) | Filter (LP/HP/BP), 3-band EQ, delay, reverb, drive, compressor, gate, tremolo, chorus, wobble; VST3/AU hosting |
| Mastering | EQ, exciter, widener, reverb, maximizer, with presets |
| Analysis | Spectrum of a selection |
| Mix | Gain/pan/mute/solo, meters, gain/pan automation, master automation |
| Record | Audio input with count-in, metronome and monitoring; MIDI recording |
| I/O | Import WAV/AIFF/FLAC/Ogg/MP3; export the same five, mix or stems, with dither |
| Workspace | Dockable panes, layouts, snapshot undo/redo |
| 🔁 Removed | Buses, sends, sidechain, tempo changes, warp, drums/guitar/piano, generative loops |

### Architectural limits that block parity

These come up again and again below, so they go first (Phase 0):

1. **Time is measured in beats.** `Clip::startBeats` and `lengthBeats` are musical time. An audio
   editor works in seconds and samples; beats should be a display and snap option, not the storage
   unit.
2. **Clips have no source offset.** A clip plays its file from sample 0, so a trim, split or slip
   has to write a new file. Audacity, Audition and REAPER all trim clips without touching the file.
   Split halves, crossfades, takes and comping all depend on an offset.
3. **Edits load and rewrite whole files.** `editSelection` reads every channel into
   `std::vector<float>` and writes a new file into *SoundSplice Edits*. That is fine for a
   three-minute song, but it's slow and memory-hungry for a two-hour podcast. Nothing ever cleans
   the folder up, and projects aren't self-contained.
4. **Built-in effects are real-time chain slots only.** Offline operations (normalize, noise
   reduction, speed/pitch) are each wired up by hand in `MainComponent.cpp`, which is 6.3k lines.
   Every new Audacity-style effect would add another bespoke dialog.
5. **The effect chain is fixed to one of each kind** (see the comment on `Track::firstEffect`).
6. **Selection is per clip.** There's no time selection across tracks, and no labels or markers
   to select *by*.

---

## 2. Phases

Each phase ships on its own and leaves the app better than it found it. Phases 0–3 are **Audacity
parity**, 4–6 add **Audition parity**, 7–8 cover what's relevant from **REAPER**, and 9 goes past
all three.

### Phase 0 — Foundations (unblocks everything)

| # | Work | Why |
| --- | --- | --- |
| 0.1 | 🟡 **Time base in seconds/samples** for audio clips, keeping beats for MIDI and the Session view ✅ (audio tracks keep their time when the tempo changes; `src/model/Timebase.h`). Ruler toggles between h:m:s ✅, samples ✅, timecode frames at 24/25/30 fps ✅ and bars/beats ✅ (View > Time Format, with the grid and snapping). Clip storage stays in beats while the song has one tempo, where the two are equivalent. | Sample-accurate editing, and podcast/voice workflows where tempo means nothing |
| 0.2 | ✅ **`Clip::sourceOffsetSeconds`** (non-destructive trim, split and left-edge drag; see `src/app/ClipWindow.h`) plus per-clip fade-in/out lengths and shapes (`src/engine/ClipFade.h`; drag a clip's top corners, right-click for shapes), all non-destructive | Trim/split/slip without writing files; the basis for crossfades and takes |
| 0.3 | ✅ **Project audio folder**: a saved project's recordings and edits live in "&lt;Name&gt; Audio" beside it, referred to by relative path, collected on save and cleaned up when unused (`src/app/ProjectMedia.h`). **Block-based audio store**: an edited clip plays a sample sequence (a `.sseq` list of spans over audio files, `src/engine/SampleSequence.h`) read through a registered `AudioFormat` (`src/engine/SequenceAudioFormat.h`). An edit reads only the selected samples and writes only its result as new float blocks; everything else, including the imported file, stays shared and untouched, so undo steps cost almost nothing. The editor's waveform peaks are built a chunk at a time, and clips a minute or longer play from disk (`src/engine/ClipStream.h`): a loader thread pages audio in ahead of every playing position and lets it go behind, while exports load whatever they need on the spot | Handles long files, keeps projects self-contained, and makes edit files collectable |
| 0.4 | ✅ **Unified `Processor` interface**: one effect definition with parameter descriptors (`src/model/EffectParams.h`) that runs real-time in the chain, offline on a selection, and as a live preview (Apply Effects' Preview button; `src/engine/AuditionPlayer.h`). Built-ins and hosted plugins both do: plugins can be added, edited, previewed and applied in Apply Effects, rendered in blocks with latency compensation. | Every Phase 2 effect then costs about one DSP file, with no new dialog or wiring |
| 0.5 | ✅ **Auto-generated effect UI** from those descriptors (the Track FX panel and Apply Effects dialog), plus **user presets** and factory presets per effect (`src/model/EffectPresets.h`; the Presets button above an effect's controls) | Audacity, Audition and REAPER all have presets on every effect |
| 0.6 | ✅ **Variable-length effect chain** (several of the same kind, reorderable). The engine chain already addressed slots by position; only a stale comment said otherwise. | Needed by 0.4 and by mastering chains |
| 0.7 | ✅ **Split `MainComponent.cpp`** into command modules (`MainComponent_*.cpp`, one per area) behind a command registry (`src/app/CommandTable.h`, a `juce::ApplicationCommandManager` for menus and keys) | Keeps the file from doubling; the registry also drives macros (7.1) and the command palette (7.3) |
| 0.8 | ✅ **Auto-save and crash recovery** (unsaved changes written every 30s beside the app settings; offered back at the next launch; see `src/app/Autosave.h`) | Table stakes in all three apps, and more urgent once edits stop writing whole new files |

### Phase 1 — Editing parity (Audacity core)

- ✅ **Time selection across tracks**, including the empty space between clips ✅ (drag across empty
  lanes, or Shift-drag over clips; snaps like a clip edge); Cut, Copy, Paste, Delete and Silence apply to
  the audio clips on every selected track, trimming and splitting without rewriting audio ✅
  (`src/model/TimeSelection.h`); effects on a time selection ✅ (Edit > Apply Effects renders into each clip it covers, with the
  same edge blending and preview as the audio editor); instrument clips ✅ (a piece of one keeps the
  notes that start in it, from its loop as played)
- ✅ **Label tracks** (Audacity) / **markers and ranges** (Audition, REAPER): add at the playhead
  (any time, playing or not) or from the audio selection, rename and delete, jump between, export
  and import as Audacity label text (Markers menu; `src/model/Markers.h`); clip edges and time
  selections snap to them; click a range to select it on every track, double-click a lane to select
  between the markers either side; drag one along the ruler to move it
- ✅ **Snap options**: grid, markers and the playhead, clip edges (View menu; magnets win over the
  grid within a few pixels, and a moved clip snaps by whichever end is nearer; `src/app/SnapTargets.h`),
  and zero crossings (Edit > Find Zero Crossings, Z, moves the time selection's edges onto them)
- 🟡 Edits on the arrangement itself, not just in the editor pane: split at the playhead ✅ (Ctrl+I),
  **join clips** ✅ (Ctrl+J; clips that carry straight on from each other, so a split can be undone
  on its own), ripple delete ✅ (Delete on a time selection), **duplicate selection** ✅
  (`src/model/ArrangementEdits.h`), **detach at silences** ✅ (Edit menu; threshold and minimum
  length, within the time selection if any; `src/engine/SilenceDetection.h`), and **paste as new clip** ✅
  (a time selection's Paste always lands as clips of their own)
- ✅ Trim and **slip** a clip's contents inside its bounds (drag an edge to trim; Ctrl-drag, Cmd on a Mac,
  to slide the audio inside the clip without moving it; `slipClip` in `src/app/ClipWindow.h`)
- ✅ **Envelope tool** ✅ (a volume curve drawn per clip, kept in file time so trims and splits keep
  it on its audio; View > Show Clip Volume Curves, then click to add, drag to move, Alt-click to
  remove; `src/engine/ClipEnvelope.h`) and **draw tool** ✅ (redraw samples when zoomed to sample
  level, to fix clicks by hand: Alt-drag in the audio editor once the samples show as a line;
  `src/app/SampleDraw.h`)
- ✅ **Scrub and seek** playback ✅ (drag along the ruler: it plays from under the mouse while held,
  and seeks while playing), **play-at-speed** (transport varispeed) ✅ (Transport > Play Faster / Slower,
  0.25x to 4x with the pitch, as on tape; `src/engine/Varispeed.h`), and **loop the selection** ✅
  (Loop plays the time selection when there is one)
- 🟡 Zoom: to selection ✅ (Ctrl+E), fit project ✅ (Ctrl+F; View menu, `src/app/TimelineZoom.h`),
  fit vertically ✅ (Ctrl+Shift+F), sample-level zoom ✅ (zoomed in past the
  peaks, the audio editor draws the samples themselves, joined and marked; `src/app/SampleDetail.h`), a vertical dB/linear scale ✅
  (View > Waveform dB Scale; `src/app/WaveformScale.h`), and waveform vs. **RMS overlay**
  display ✅ (the audio editor draws RMS in a lighter band inside the peaks)
- ✅ Track channel ops: **split stereo to mono** ✅ and swap channels ✅ (Edit menu; each clip plays
  a chosen channel, so nothing is rendered; `src/model/TrackChannels.h`), **make stereo from two monos** ✅
  (split halves join back as they were; any other pair renders to one stereo file),
  **mix and render to a new track** ✅
  (the tracks' pre-master mix over the time selection, on the render thread), and per-track **resample** ✅
  (Edit > Resample Track: a windowed-sinc copy of each file at the new rate, clips keep their timing;
  `src/engine/Resample.h`)
- ✅ **Multiple open files** in the editor, as a list (Audition's Files panel): every audio clip shown in the
  Audio editor joins the Open Files pane; click one to edit it, close one or all, and step through them with
  Ctrl+PageUp/PageDown and Ctrl+W (View menu; `src/app/OpenFiles.h`, `src/app/OpenFilesPane.h`)
- 🟡 Import: **Opus** ✅ and **WavPack** ✅ (BSD-licensed opusfile and libwavpack; `cmake/codecs.cmake`,
  `src/engine/AudioFormats.cpp`), **CAF** ✅ (PCM, U-law and A-law; AAC and ALAC inside CAF read on macOS only),
  **RF64/BW64 and W64** ✅ (`src/engine/PcmContainers.h`), **M4A/AAC** ⬜ (no BSD-licensed decoder: needs a licensing decision), **raw PCM** ✅ (File > Import Raw Data: 8/16/24/32-bit,
  float, U-law and A-law, either byte order, a header to skip; `src/engine/RawPcm.h`), and audio pulled from video ⬜
  (optional FFmpeg module, as Audacity does)

### Phase 2 — Effects, generators and analyzers parity

Built on 0.4, so each line is mostly DSP plus tests. Anything marked ✅ as real-time also needs
offline apply and preview.

**Volume and dynamics**
✅ Compressor · ✅ Gate · 🟡 Normalize (peak only) · ✅ Limiter (the Maximizer as a chain effect) · ✅ Amplify (`src/engine/UtilityEffects.h`) · ✅ **Loudness normalization (LUFS/LU, EBU R128)** (Edit > Normalize Loudness: clip gain to a LUFS target, held under -1 dBTP; `src/engine/Loudness.h`) · ⬜ **Auto Duck** · ⬜ Expander ·
⬜ **Multiband compressor** (REAPER ReaXComp, Audition) · ⬜ **De-esser** · ⬜ Dynamics processor with a
drawable transfer curve (Audition)

**Fades**
✅ Fade in/out (linear) · ✅ Adjustable fade curves (clip fades: linear, equal power, S-curve, exponential, logarithmic) · ✅ **Studio fade out** ·
✅ **Crossfade clips** (Edit > Crossfade Clips: neighbours overlap from their hidden audio across the time selection; overlapping clips on a track now mix) · ⬜ Crossfade tracks · ⬜ Automatic crossfades on overlap (REAPER)

**Pitch and time**
✅ Change speed · ✅ Change pitch · ✅ **Change tempo** (Edit > Change Tempo, on the whole clip) ·
⬜ Paulstretch · ⬜ Sliding stretch (pitch or tempo varying over the selection) · ⬜ **Formant-preserving
pitch shift** · ⬜ Better stretch quality: evaluate Rubber Band (GPL/commercial) and Signalsmith Stretch
(MIT) against the current phase vocoder · ⬜ Pitch correction / tuner (REAPER ReaTune)

**EQ and filters**
✅ LP/HP/BP · ✅ 3-band EQ · ✅ Bass and treble · ⬜ **Graphic EQ** (10/31 band) · ⬜ **Parametric EQ with
unlimited bands** and a drawable curve (Audacity Filter Curve, REAPER ReaEQ) · ⬜ Notch · ⬜ Shelf
(`ShelfPeakFilter` exists) · ⬜ **Match EQ** (fit one clip's spectrum to another's)

**Noise removal and repair** (Audacity *and* Audition)
✅ Noise reduction · ✅ **Click/pop removal** · ✅ **Clip fix / DeClipper** · ✅ Repair (interpolate a short
region) · ✅ **DeHummer** (50/60 Hz and harmonics) (Edit menu, on the audio editor's selection; `src/engine/Repair.h`) · ⬜ **Adaptive noise reduction** (no noise print
needed) · ⬜ **DeReverb** · ⬜ DeCrackle · ✅ DC offset removal

**Delay, reverb and modulation**
✅ Delay · ✅ Reverb · ✅ Tremolo · ✅ Chorus · ✅ Drive (Audacity has 11 distortion types) · ⬜ Echo (multitap) ·
⬜ **Convolution reverb** with impulse-response loading (`CabinetIr` is a starting point) · ✅ Phaser ·
✅ Flanger (`src/engine/ToneDsp.h`) · ⬜ Wah-wah · ⬜ Vocoder · ⬜ Ring modulator

**Stereo and special**
✅ Widener (mastering, and Stereo Tools' width on any track) · ✅ Invert (either channel or both) · ✅ Repeat · ✅ **Truncate silence** (across the time selection's tracks, without rewriting audio) · 🟡 **Channel mixer / mid-side** (Stereo Tools: width, balance, mono, swap)
· ⬜ Center channel extractor / vocal reduction (Audition) · ✅ Stereo-to-mono downmix (Stereo Tools' Mono)

**Generators**
✅ Tone (sine/square/saw/triangle) · ✅ Chirp (linear or logarithmic) · ✅ **Noise** (white/pink/brown) · ✅ Silence · ✅ DTMF
(Generate menu: into the time selection, at the playhead, or on a new track; `src/engine/Generators.h`) · ⬜ Rhythm track
/ click track (the metronome can render it) · ⬜ Pluck · ⬜ **Room tone fill** (synthesize or loop
captured room tone into gaps)

**Analyzers**
✅ Plot spectrum · ✅ **Find clipping** (a marker range over each run) · ✅ **Measure RMS / amplitude statistics**
(Audition: peak, RMS, DC offset, dynamic range) · ⬜ **Contrast** (WCAG foreground/background) ·
✅ **Label sounds / silence finder** (Analyze menu; `src/engine/AmplitudeAnalysis.h`) · ⬜ **Beat finder** · ✅ **Loudness meter** (momentary, short-term,
integrated LUFS, true peak, LRA): a selection or clip measured into the Analyser (Analyze > Measure Loudness), and live on the master bus in the Master pane, with a reset · ⬜ **Phase correlation meter / vectorscope** · ⬜ Oscilloscope

### Phase 3 — Spectral editing

- ⬜ **Spectrogram track view**: linear, log or mel scale, a configurable window, and a split
  waveform/spectrogram view (Audacity, Audition, REAPER)
- ⬜ **Spectral selection** (time × frequency box, lasso, and a harmonic brush as in Audition)
- ⬜ Spectral delete · spectral parametric EQ · spectral shelves (Audacity)
- ⬜ **Spot healing brush**: paint over a cough, click or phone ring and have it inpainted from the
  surrounding time and frequency content (Audition)
- ⬜ Non-destructive spectral edits stored on the clip (REAPER)

### Phase 4 — Recording parity

- ✅ Count-in, metronome, monitoring · ⬜ **Punch in/out** with pre-roll and crossfade ·
  ⬜ **Loop recording into takes** · ⬜ **Record several inputs at once to several tracks** ·
  ⬜ **Append record** (continue at the end of the track, as Audacity does)
- ⬜ **Sound-activated recording** and **timer record** (Audacity)
- ⬜ Record formats: 24-bit and 32-bit float WAV/RF64, mono vs. stereo per track, input-channel mapping
- ⬜ **Latency compensation** for recordings (measured round-trip, applied as an offset)
- ⬜ Input level meter with a peak hold and a clip indicator on every armed track
- ⬜ Arm and disarm tracks while playing (REAPER)
- ⬜ **Retroactive recording**: always keep the last N minutes of input in a buffer, so a take you
  forgot to record can be saved afterward (REAPER does this for MIDI; nobody does it well for audio)

### Phase 5 — Multitrack and mixing parity (Audition, REAPER)

- ⬜ **Take lanes and swipe comping** with A/B comparison (REAPER, Audition)
- ⬜ **Razor/range edits** across tracks (REAPER)
- ⬜ **Track edit groups** and **folder tracks** (folders as organization only; see the 🔁 note below)
- ⬜ **Automation for any effect or plugin parameter**, with curve shapes and automation modes
  (read, touch, latch, write)
- ⬜ **Plugin delay compensation**
- ⬜ Clip-level effects (an effect chain on one clip rather than the whole track)
- ⬜ **Hosting LV2 and CLAP** (CLAP needs `clap-juce-extensions` or a newer JUCE; research first),
  plus a plugin manager with enable/disable, a blocklist and crash-safe scanning
- 🔁 **Buses, sends, sidechain, tempo changes, warp** exist in REAPER and Audition but were cut from
  SoundSplice on purpose. Parity would mean re-adding them, so decide per feature before Phase 5
  starts. Auto Duck (Phase 2) covers the most common sidechain use without them.

### Phase 6 — Audition's "finishing" workflows

- ⬜ **Essential Sound panel**: tag a clip as Dialogue, Music, SFX or Ambience to get a simple task
  panel (loudness match, repair, clarity, ducking) that drives the real effects underneath
- ⬜ **Match loudness across clips** (non-destructive clip gain to a LUFS target)
- ⬜ **Auto-ducking** of music under dialogue, generating editable automation rather than baking it in
- ⬜ **Diagnostics panel**: scan a file and list clicks, clipping, silence and DC offset, each with a
  fix and a select button (Audition's DeClicker/DeClipper diagnostics)
- ⬜ **Batch process**: run an effect chain or preset over a folder of files
- ⬜ Media browser with **preview** (✅ file browser and preview exist; add metadata columns and
  favorites)
- ⬜ **Favorites**: one-click saved actions or effect settings (Audition)

### Phase 7 — Workflow, customization and accessibility (Audacity, REAPER)

- ⬜ **Macros**: record or build a list of commands and effects with settings, then run it on the
  selection or a batch of files (Audacity Macros, REAPER Actions)
- ⬜ **Customizable keyboard shortcuts**, with importable and exportable sets
- ⬜ **Command palette** (search every command by name)
- ⬜ **Scripting**: embedded Lua with the command registry exposed (REAPER ReaScript, Audacity mod-script-pipe)
- ⬜ **Accessibility**: full keyboard operation of tracks, clips and selections; screen-reader
  announcements via JUCE's accessibility API; high-contrast theme (Audacity's strong suit)
- ⬜ Themes and custom colors; saved screensets (✅ layouts exist)
- ⬜ Preferences dialog (devices, formats, editing defaults, paths, cache)
- ⬜ **Project templates** (podcast, audiobook, music, voice-over)
- ⬜ Headless CLI: extend `soundsplice_bounce` into `soundsplice-cli` for convert, render, apply macro
  and analyze

### Phase 8 — Render and export parity (REAPER, Audition)

- ✅ WAV/AIFF/FLAC/Ogg/MP3, mix or stems, dither · ⬜ **Opus, WavPack, M4A/AAC, RF64/BW64**
- ⬜ **Export multiple**: one file per label, region or track, with **filename wildcards**
- ⬜ **Metadata**: ID3, Vorbis comments, BWF/iXML, cover art
- ⬜ **Loudness-normalize on export** and a true-peak limiter
- ⬜ **Render queue** and saved render presets
- ⬜ **Render statistics report**: peak, LUFS over time, and a clip list, as HTML (REAPER)
- ⬜ High-quality sample-rate conversion (r8brain-free is MIT) · noise-shaped dither
- ⬜ Export selection only · ⬜ CD image (CUE/BIN) from labels
- ⬜ Chapter markers for podcast files (MP3 CHAP, M4A chapters)

### Phase 9 — Beyond the three (differentiators)

Things none of the three apps do well, or that would make SoundSplice the obvious choice for
voice, podcast and restoration work:

1. **Local speech transcription** (whisper.cpp): word-timed transcript as a label track (Audacity 3.4+
   does this through OpenVINO; bundling it makes it one click)
2. **Edit audio by editing text**: delete words in the transcript to cut the audio, with automatic
   micro-crossfades and room-tone fill. Descript does this; no open desktop editor does.
3. **Filler-word and long-pause removal** ("um", "uh", gaps over N ms), reviewable before it's applied
4. **Stem separation** (vocals/drums/bass/other) via a local ONNX model (Demucs-class)
5. **AI speech enhancement / noise suppression** (DeepFilterNet or RNNoise-class) alongside the
   classic noise print
6. **Delivery-spec checker**: pick ACX/Audible, Spotify, Apple Podcasts, YouTube or EBU R128
   broadcast; get pass/fail on loudness, true peak, noise floor and head/tail silence, plus a
   one-click "make it pass" chain
7. **Loudness-matched A/B against a reference track**, so louder never passes for better
8. **Preview-before-apply everywhere**, with a bypass toggle and a difference ("what was removed")
   solo for every offline effect, not just some
9. **Visual history panel**: jump to any undo step, compare it with the current state, and branch
   from it instead of losing redo
10. **Multichannel/ambisonic import and export** (REAPER-grade channel counts) for game and
    immersive audio
11. **Video track for sync**, showing a reference video for dubbing and podcast video (REAPER,
    Audition). Late and optional, since it's a big dependency.
12. **Keep the Session view** as a sketchpad for musicians, which none of the three editors has

---

## 3. Suggested order and first slices

```
Phase 0 ──► Phase 1 ──► Phase 2 (in parallel slices) ──► Phase 3
                 │                                       │
                 └──► Phase 4 (recording)                └──► Phase 6
Phase 7 items can land at any point once 0.7 (command registry) exists.
Phase 8 export items are mostly independent: pick them up whenever.
Phase 9 needs 1 (labels) and 0.4 (Processor); transcription first, text editing second.
```

The first five pull requests:

1. ✅ `Clip::sourceOffsetSeconds` plus non-destructive trim and split (0.2), with serialization and tests
2. The `Processor` interface and generated UI, porting Normalize and Reverse onto it (0.4, 0.5)
3. Seconds time base and ruler formats (0.1)
4. Label tracks, with selection by label (Phase 1)
5. Loudness meter plus loudness normalization, sharing one EBU R128 implementation (Phase 2)

Each effect in Phase 2 follows one checklist: DSP header in `src/engine/` → a Catch2 test pinning
its response → `Processor` descriptor → presets → an entry in the effect menu. Most need no UI code.

## 4. Licensing notes

The code is MIT. Watch for: **Rubber Band** (GPL, or a commercial license), **FFmpeg** (LGPL if
dynamically linked and built without GPL parts), **LAME** (LGPL, already used), and **Élastique**
(commercial). MIT/BSD options: Signalsmith Stretch, r8brain-free, whisper.cpp, ONNX Runtime,
libebur128, and the Opus codec.
