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
| 🔁 Removed | Drums/guitar/piano, generative loops (buses, sends, sidechain, tempo changes and warp are coming back: Phase 5) |

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
✅ Compressor · ✅ Gate · ✅ Normalize (Edit > Normalize: peak, DC offset removal, channels together or each on its own, over a selection or the clip; the plain whole-clip case still just sets clip gain) · ✅ Limiter (the Maximizer as a chain effect) · ✅ Amplify (`src/engine/UtilityEffects.h`) · ✅ **Loudness normalization (LUFS/LU, EBU R128)** (Edit > Normalize Loudness: clip gain to a LUFS target, held under -1 dBTP; `src/engine/Loudness.h`) · ✅ **Auto Duck** (Edit > Auto Duck: dips the selected tracks under the lowest selected one, as editable clip volume curves) · ✅ Expander ·
✅ **Multiband compressor** (three bands, Linkwitz-Riley crossovers; `src/engine/DynamicsDsp.h`) · ✅ **De-esser** (split-band, Linkwitz-Riley crossover) · ✅ Dynamics processor with a
drawable transfer curve (Audition): up to six points, peak or RMS detector, drag the curve in the effect panel (`src/engine/DynamicsProcessor.h`)

**Fades**
✅ Fade in/out (linear) · ✅ Adjustable fade curves (clip fades: linear, equal power, S-curve, exponential, logarithmic) · ✅ **Studio fade out** ·
✅ **Crossfade clips** (Edit > Crossfade Clips: neighbours overlap from their hidden audio across the time selection; overlapping clips on a track now mix) · ✅ Crossfade tracks (Edit > Crossfade Tracks: the upper of two selected audio tracks hands over to the lower across the time selection, equal power or equal gain, as clip volume curves) · ✅ Automatic crossfades on overlap (REAPER; View > Automatic Crossfades, on by default: moving, resizing or trimming an audio clip over its neighbour fades both across the overlap, and the fades go if the overlap does)

**Pitch and time**
✅ Change speed · ✅ Change pitch · ✅ **Change tempo** (Edit > Change Tempo, on the whole clip) ·
✅ Paulstretch (`src/engine/Paulstretch.h`) · ✅ Sliding stretch (Edit > Sliding Stretch: tempo and pitch moving from one amount to another across the clip, by Signalsmith Stretch) · ✅ **Formant-preserving
pitch shift** (Speed and Pitch: Voice character, Stays put) · ✅ Better stretch quality: Signalsmith Stretch (MIT) now does
Change Tempo and Speed and Pitch's shift, the phase vocoder kept for clips too short for it; Rubber Band left out, being GPL
(`src/engine/HqStretch.h`, `cmake/stretch.cmake`) · ✅ Pitch correction / tuner (REAPER ReaTune: Edit > Pitch Correction to a key and scale, with strength and speed; Analyze > Detect Pitch; YIN in `src/engine/PitchDetection.h`)

**EQ and filters**
✅ LP/HP/BP · ✅ 3-band EQ · ✅ Bass and treble · ✅ **Graphic EQ** (10 bands ✅; 31 third-octave bands ✅, `src/engine/ThirdOctaveEq.h`) · ✅ **Parametric EQ** with a drawable curve (Audacity Filter Curve, REAPER ReaEQ): six bands, each a bell, shelf, notch or cut; drag the points on its curve (`src/engine/ParametricEq.h`) · ✅ Notch · ✅ Shelf · ✅ **Match EQ** (fit one clip's spectrum to another's: Edit > Set as Match EQ Reference, then Match EQ to Reference adds a 31-band EQ; `src/engine/MatchEq.h`)

**Noise removal and repair** (Audacity *and* Audition)
✅ Noise reduction · ✅ **Click/pop removal** · ✅ **Clip fix / DeClipper** · ✅ Repair (interpolate a short
region) · ✅ **DeHummer** (50/60 Hz and harmonics) (Edit menu, on the audio editor's selection; `src/engine/Repair.h`) · ✅ **Adaptive noise reduction** (no noise print
needed: minimum statistics, following noise that changes; Edit > Adaptive Noise Reduction; `src/engine/AdaptiveNoiseReduction.h`) · ✅ **DeReverb** (Edit > DeReverb: late reverb estimated from the room's reverb time and subtracted; `src/engine/Dereverb.h`) · ✅ DeCrackle (Edit > DeCrackle) · ✅ DC offset removal

**Delay, reverb and modulation**
✅ Delay · ✅ Reverb · ✅ Tremolo · ✅ Chorus · ✅ Drive (Audacity has 11 distortion types) · ✅ Echo (multitap, with ping-pong) ·
✅ **Convolution reverb** with impulse-response loading (partitioned FFT convolution, a built-in hall until a file is loaded; `src/engine/Convolver.h`, `src/engine/ConvolutionEffect.h`) · ✅ Phaser ·
✅ Flanger (`src/engine/ToneDsp.h`) · ✅ Wah-wah (auto-wah) · ✅ Vocoder (left channel through the right, a sawtooth or noise; 4-32 bands, carrier flattened; `src/engine/Vocoder.h`) · ✅ Ring modulator (`src/engine/DynamicsDsp.h`)

**Stereo and special**
✅ Widener (mastering, and Stereo Tools' width on any track) · ✅ Invert (either channel or both) · ✅ Repeat · ✅ **Truncate silence** (across the time selection's tracks, without rewriting audio) · ✅ **Channel mixer / mid-side** (Stereo Tools: width, balance, mono, swap; the Channel Mixer effect: a 2x2 matrix with mid/side encode, decode, or both around it; `src/engine/ChannelMixer.h`)
· ✅ Center channel extractor / vocal reduction (Audition; Edit > Vocal Reduction and Isolation: remove or isolate the centre over a band; `src/engine/CenterChannel.h`) · ✅ Stereo-to-mono downmix (Stereo Tools' Mono)

**Generators**
✅ Tone (sine/square/saw/triangle) · ✅ Chirp (linear or logarithmic) · ✅ **Noise** (white/pink/brown) · ✅ Silence · ✅ DTMF
(Generate menu: into the time selection, at the playhead, or on a new track; `src/engine/Generators.h`) · ✅ Rhythm track / click track (Generate > Rhythm Track: tempo, beats per bar, bars) · ✅ Pluck (Karplus-Strong, allpass-tuned) · ✅ **Room tone fill** (Generate > Capture Room Tone, then Room Tone: noise synthesized
with the captured room's spectrum and level into the time selection; `src/engine/RoomTone.h`)

**Analyzers**
✅ Plot spectrum · ✅ **Find clipping** (a marker range over each run) · ✅ **Measure RMS / amplitude statistics**
(Audition: peak, RMS, DC offset, dynamic range) · ✅ **Contrast** (WCAG foreground/background: Analyze > Set Contrast Background, then Contrast) ·
✅ **Label sounds / silence finder** (Analyze menu; `src/engine/AmplitudeAnalysis.h`) · ✅ **Beat finder** (spectral-flux onsets as numbered point markers, with a tempo estimate; `src/engine/OnsetDetection.h`) · ✅ **Loudness meter** (momentary, short-term,
integrated LUFS, true peak, LRA): a selection or clip measured into the Analyser (Analyze > Measure Loudness), and live on the master bus in the Master pane, with a reset · ✅ **Phase correlation meter / vectorscope** (live on the master bus in the Master pane; `src/engine/StereoScope.h`) · ✅ Oscilloscope (click the vectorscope)

### Phase 3 — Spectral editing

- ✅ **Spectrogram track view**: the audio editor as a spectrogram ✅ (View > Spectrogram: log frequency
  scale, 2048-point Hann window, built with the waveform's peaks a chunk at a time;
  `src/engine/Spectrogram.h`, `src/app/SpectrogramImage.h`); linear and mel scales ✅ (View > Spectrogram Scale), a configurable window ✅ (View > Spectrogram Settings: 256–16384 points; Hann, Hamming, Blackman-Harris or rectangular; display gain and range), a
  split waveform/spectrogram view ✅ (View > Waveform and Spectrogram: each half keeps its own gestures), spectrograms in the arrangement's lanes ✅ (View > Spectrograms in Tracks; analysed in the background, `src/app/SpectrogramCache.h`)
- ✅ **Spectral selection**: a time × frequency box ✅ (drag diagonally on the spectrogram); lasso ✅ (Ctrl+Shift-drag) and a harmonic brush ✅ (Ctrl+Alt-drag: paints a note and its overtones) as in Audition; Spectral Delete, Gain and Repair work on any of them
- 🟡 Spectral delete ✅ · spectral gain ✅ (Edit menu, on the box; `src/engine/SpectralEdit.h`) · spectral parametric EQ ✅ · spectral shelves ✅ (Audacity; Edit > Spectral EQ / Spectral Shelf)
- ✅ **Spot healing brush**: paint over a cough, click or phone ring and have it inpainted from the
  surrounding time and frequency content (Audition). A box version ✅ (Edit > Spectral Repair: each bin's
  level drawn across the box from its average either side, the phase kept); a freehand brush ✅ (Ctrl-drag on the spectrogram, Cmd on a Mac, then Spectral Repair heals only what was painted)
- ✅ Non-destructive spectral edits stored on the clip (REAPER): Edit > Spectral > Add Clip Spectral Edit keeps a box's gain on the clip, outlined on the spectrogram; playback and export read the file with them applied, rendered once to a cache (`src/app/SpectralRender.h`); Remove Clip Spectral Edits takes them off

### Phase 4 — Recording parity

- ✅ Count-in, metronome, monitoring · ✅ **Punch in/out** with pre-roll and crossfade (Transport > Punch Recording, over the time selection) ·
  ✅ **Loop recording into takes** (Loop on and a time selection: each pass round it is a take of one clip) · ✅ **Record several inputs at once to several tracks** (arm with each mixer strip's R; right-click it for the track's input) ·
  ✅ **Append record** (continue at the end of the track, as Audacity does: Transport > Record at End of Track)
- ✅ **Sound-activated recording** and **timer record** (Audacity): Transport > Sound-Activated Recording... (a threshold to start on, a silence to stop on) and Timer Record...
- ✅ Record formats: 16-bit, 24-bit and 32-bit float WAV (RF64 past 4 GB), mono or stereo, from any input or pair (File > Recording Format...) · ✅ input and mono/stereo per track (right-click a mixer strip's R)
- ✅ **Latency compensation** for recordings (measured round-trip, applied as an offset): the device's reported round trip, or one measured with a click through a loopback cable (File > Measure Recording Latency...), plus a manual adjustment (File > Recording Latency...), applied to every recording and loop take
- ✅ Input level meter with a peak hold and a clip indicator on every armed track: an armed track's mixer strip meters its own input, with a clip light that stays lit until clicked, and the Transport panel meters the main take's
- ✅ Arm and disarm tracks while playing (REAPER): arming a track during a take joins it from there, disarming one ends just its part
- ✅ **Retroactive recording**: Transport > Keep Recent Input holds the last two minutes of input while playing, and Save Recent Input puts what came in since playback last started on the track where it was played (REAPER does this for MIDI; nobody does it well for audio)

### Phase 5 — Multitrack and mixing parity (Audition, REAPER)

- ✅ **Take lanes and swipe comping** with A/B comparison (REAPER, Audition): ✅ takes on a clip (Combine Overlapping Clips into Takes), ✅ switching takes to compare them and comping a time selection to a take (right-click the clip) · ✅ take lanes drawn in the clip (View > Show Take Lanes: a row per take under a header; click a row to hear that take, drag along one to comp across every piece it crosses; `src/app/TakeLanes.h`, `takeedit::compTrackRange`) · ✅ recording takes by looping (Phase 4)
- ✅ **Razor/range edits** across tracks (REAPER): Ctrl+Shift-drag (Cmd on a Mac) adds an area on each lane crossed, keeping earlier ones, so each track can have its own stretches; Cut, Copy, Delete and Silence act on all of them without closing gaps, Paste puts them back in the same shape, and a drag inside one moves them in time and across lanes (`src/model/RazorEdits.h`)
- ✅ **Track edit groups** ✅ (a track's gear menu > Edit Group: time selections and razor areas take in the group, mute, solo and faders follow, faders by as much, and moving a clip moves the clips lined up with it; `src/model/TrackGroups.h`) and **folder tracks** ✅ (organization only, no routing: gear menu > Put in Folder Above / Take Out of Folder; a folder's triangle collapses it, and mute and solo on it take its tracks along; `src/model/Folders.h`)
- ✅ **Automation for any effect or plugin parameter** ✅ for every built-in effect parameter and every automatable parameter of a loaded plugin (pick it in the Automation pane; the lanes live on the effect, so they move with it) · ✅ curve shapes (right-click a point: Linear, Hold, Fast Start, Slow Start, S-Curve) · ✅ automation modes (the master panel's Automation picker sets the mix's; each mixer strip's mode button gives a track its own, saved with the project)
  (read, touch, latch, write)
- ✅ **Plugin delay compensation**: every track is delayed to meet the latest one (plugins and the limiter report their latency), and exports are trimmed so they start on time · ✅ latency of a clip's own effects (the clip is read ahead by it) · ✅ recorded input (Phase 4)
- ✅ Clip-level effects (an effect chain on one clip rather than the whole track): the effects panels Track FX / Clip FX switch; played live, before the tracks chain · ✅ plugins on a clip · ✅ clip effect tails past the clip's end (eight seconds)
- 🟡 **Hosting LV2** ✅ (JUCE's own LV2 host; its lilv is ISC-licensed) and **CLAP** ⬜ (researched: JUCE
  8.0.14 hosts VST3, AU and LV2 only, and `clap-juce-extensions` is for *making* CLAP plugins, not hosting
  them, so hosting CLAP means a host of our own on the MIT `clap` headers, or waiting for JUCE), plus a
  **plugin manager** ✅ (File > Plugin Manager: turn plugins off, forget them, unblock them, scan) with a
  blocklist ✅ and crash-safe scanning ✅ (each plugin is probed in a copy of the app, `src/app/PluginProbe.h`;
  one that crashes, hangs or won't load is blocklisted)
- ✅ **Buses, sends, sidechain, tempo changes, warp**: cut in the strip-down, and brought back (decided
  2026-10-01). ✅ **Bus tracks and sends** (Edit > Add Bus Track, or Add Bus in the mixer: a mixer strip's
  Out routes the track to a bus, Sends adds sends at their own level, pre or post fader; buses feed
  buses, never in a loop; solo follows the routing and delay compensation counts each bus on the way
  out; `src/model/Routing.h`, `src/engine/MixRouting.h`; the headless bounce tool doesn't route yet) ·
  ✅ **sidechain** (a compressor's or gate's Key button in the effects panel: it listens to another track's
  output as it leaves its fader, rendered first; `FXKEY`) · ✅ **tempo changes** (right-click the ruler to add,
  edit, ramp or remove one, drag its flag to move it; the tempo control edits the tempo in force at the
  playhead; audio keeps its time and instrument parts their beats; clip edits, fades, the seconds grid and
  label import/export measure through the map, `model::BeatClock`; snapping to the seconds grid still steps
  at the starting tempo) · ✅ **warp** (right-click an audio clip: Detect Clip Tempo or Set Clip Tempo, then Warp
  to Song Tempo; the clip keeps its beats and its audio is stretched, pitch kept, by Signalsmith Stretch, following
  tempo changes; one stretch per clip, from the tempo where it starts; `src/model/Warp.h`,
  `src/engine/TempoDetect.h`)

### Phase 6 — Audition's "finishing" workflows

- ✅ **Essential Sound panel**: tag a clip as Dialogue, Music, SFX or Ambience to get a simple task
  panel (loudness match, repair, clarity, ducking) that drives the real effects underneath: the Essential Sound
  pane (beside Mastering); each task is an amount that builds marked slots at the front of the clip's own effects
  (rumble, noise, sibilance, clarity, dynamics, width), Match All matches every clip with the tag, and Music and
  Ambience duck under the Dialogue clips as editable volume curves (`src/model/EssentialSound.h`)
- ✅ **Match loudness across clips** (Edit > Match Loudness: every audio clip the time selection touches, or the
  selected track's, each measured and its clip gain set to one LUFS target, true peak held under -1 dBTP; one undo step;
  `src/app/LoudnessMatch.h`)
- ✅ **Auto-ducking** of music under dialogue, as clip volume curves rather than baked in (Edit > Auto Duck)
- ✅ **Diagnostics panel**: scan a file and list clicks, clipping, silence and DC offset, each with a
  fix and a select button (Audition's DeClicker/DeClipper diagnostics): Analyze > Diagnostics fills the Diagnostics
  pane; Select picks a problem in the audio editor, Fix repairs it with Click Removal, Clip Fix, Delete or DC
  removal, Fix All does every one of its kind (`src/engine/Diagnostics.h`, `src/app/DiagnosticsPane.h`)
- ✅ **Batch process**: run an effect chain or preset over a folder of files (File > Batch Process: a folder,
  a chain built in the Apply Effects dialog, a loudness target and a format; new files are written, the originals
  untouched; on a background job, or one file at a time when the chain hosts a plugin; `src/app/BatchProcess.h`)
- ✅ Media browser with **preview** (✅ file browser and preview exist; ✅ metadata columns: rate, channels,
  bit depth and title, read from each header once; ✅ favorites: star a file, and Places > Favorites lists every
  starred file wherever it is)
- ✅ **Favorites**: one-click saved actions or effect settings (Audition): Save as Favorite in Apply Effects, or
  Favorites > Save Selected Track's Effects; the Favorites menu applies one to the selection (`src/model/Favorites.h`)

### Phase 7 — Workflow, customization and accessibility (Audacity, REAPER)

- ✅ **Macros**: record or build a list of commands and effects with settings, then run it on the
  selection or a batch of files (Audacity Macros, REAPER Actions): a new Tools menu runs them; Record Macro keeps
  the commands and applied effects as you work; the Macros window builds, edits and reorders steps; a macro of
  effects runs over a folder through Batch Process. A step is a command that acts at once (one that asks first
  can't be a step) or an effect chain with its settings (`src/app/Macros.h`, `src/app/MacrosDialog.h`)
- ✅ **Customizable keyboard shortcuts**, with importable and exportable sets: File > Keyboard Shortcuts;
  saved by command name with portable key names ("Cmd+Shift+Left") so a set survives new commands and moves
  between Windows and macOS (`src/app/ShortcutSets.h`, `src/app/KeyboardShortcutsDialog.h`)
- ✅ **Command palette** (search every command by name): View > Command Palette (Ctrl+Shift+P) lists every
  command, pane, layout and Favorite; fuzzy matching on the name or category, recently run ones first, greyed
  when unavailable, with each one's shortcut (`src/app/CommandPalette.h`)
- ✅ **Scripting**: embedded Lua with the command registry exposed (REAPER ReaScript, Audacity mod-script-pipe):
  Lua 5.4 (MIT, `cmake/lua.cmake`) in a Script pane with a Lua editor, and Tools > Run Script for .lua files.
  The `soundsplice` table runs commands and macros by name, reads and sets tracks' volume, pan, mute, solo and
  name, and the playhead and time selection in seconds. Sandboxed (no io, os, package, debug or file loading) and
  stopped if it runs too long (`src/app/Scripting.h`, `src/app/ScriptPane.h`)
- ✅ **Accessibility**: full keyboard operation of tracks, clips and selections; screen-reader
  announcements via JUCE's accessibility API; high-contrast theme (Audacity's strong suit): View > Keyboard
  Navigation - previous/next track (Alt+Up/Down), previous/next clip (Ctrl+Alt+Left/Right, moving the
  playhead), nudge a clip (Ctrl+Shift+Left/Right), selection edges at the playhead ([ and ]), Where Am I
  (Alt+W) - each said through the screen reader, as is every status message; panes carry their names; the
  High Contrast theme and focus rings (`src/app/MainComponent_Keyboard.cpp`)
- ✅ Themes and custom colors; saved screensets (✅ layouts exist): Preferences > Display picks Dark,
  Midnight, Grey or High Contrast, an accent (a list or any colour), and keyboard focus rings; panes take
  their surfaces from the theme by role (`src/app/Theme.h`). All are dark: the panes draw light text of their
  own. View > Layout saves the current arrangement under a name and puts it back (`src/app/Screensets.h`)
- ✅ Preferences dialog (devices, formats, editing defaults, paths, cache): File > Preferences (Ctrl+,), tabs
  for devices, recording format and latency, editing and display, folders (recordings and edits now
  configurable) and the cache, and keyboard. Most rows run the command the menus already have, so there is one
  copy of each setting (`src/app/PreferencesDialog.h`)
- ✅ **Project templates** (podcast, audiobook, music, voice-over): File > New from Template; each is named,
  routed tracks with effects ready (the voice-over's music bed ducks under the voice by sidechain). File > Save as
  Template keeps any project's tracks, routing and effects without its audio (`src/model/Templates.h`)
- ✅ Headless CLI: extend `soundsplice_bounce` into `soundsplice-cli` for convert, render, apply macro
  and analyze: a tool of its own beside the bounce smoke test (`tools/cli`). `convert` (format, rate by the
  sinc resampler, bit depth), `analyze` (R128 loudness, true peak, peak, RMS, DC, dynamic range), `apply` (a
  saved macro of effects, or one from a file, over files or folders, with a loudness target), `macros`, and
  `render`, which runs the app with `--render` - no window, no audio device, nothing autosaved - so a project
  renders through the app's own export rather than a second mixer

### Phase 8 — Render and export parity (REAPER, Audition)

- ✅ WAV/AIFF/FLAC/Ogg/MP3, mix or stems, dither · ✅ **Opus** (resampled to 48 kHz, 64-256 kbps) and
  **WavPack** (16/24/32-bit float, four compression levels), each written by its own BSD library with tags and
  cover (`src/engine/AudioFormats.cpp`) · ✅ RF64 (JUCE's WAV writer switches to it past 4 GB) · ⬜ M4A/AAC (no
  BSD-licensed encoder), BW64, W64
- 🟡 **Export multiple**: one file per label, region or track, with **filename wildcards**: Export Audio's Range
  writes one file per marker range, named by a pattern ($project $region $index $date), with stems per range
  when asked; per track is stems (`src/app/ExportNaming.h`). One file per point label ⬜
- ✅ **Metadata**: ID3, Vorbis comments, BWF/iXML, cover art: File > Project Info (kept in the project); Export
  Audio's Tags writes ID3v2.4 with APIC into MP3, Vorbis comments and a PICTURE block into FLAC, Vorbis comments
  into Ogg, and RIFF INFO plus BWF bext into WAV (`src/engine/ExportTags.h`). iXML ⬜
- ✅ **Loudness-normalize on export** and a true-peak limiter: Export Audio's Loudness (-14 to -24 LUFS, true
  peak under -1 dBTP; the limiter is run until the true peak fits); stems get the mix's gain so they still sum;
  `soundsplice-cli render --loudness` (`src/engine/ExportLoudness.h`)
- ⬜ **Render queue** and saved render presets
- ⬜ **Render statistics report**: peak, LUFS over time, and a clip list, as HTML (REAPER)
- ⬜ High-quality sample-rate conversion (r8brain-free is MIT) · noise-shaped dither
- ✅ Export selection only (Export Audio > Range) · ⬜ CD image (CUE/BIN) from labels
- 🟡 Chapter markers for podcast files (MP3 CHAP, M4A chapters): markers become MP3 CHAP/CTOC chapters and
  FLAC CHAPTERnnn comments, timed from each file's own start ✅ · M4A ⬜ (no AAC encoder)

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
