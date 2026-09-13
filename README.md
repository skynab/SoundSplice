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
> **The dockable workspace now remembers its layout, and the file browser has real bookmarks** —
> the last two items on the list. Both are app-level preferences (not song data), stored in a
> `juce::PropertiesFile` next to the app's other settings (separate from `.looper` project saves).
> Moving a panel between dock regions, or quitting the app, writes each of the four panels' current
> region and each region's active tab; on the next launch, every panel is asked "where were you
> saved" and silently stays in its constructor-assigned default if there's no answer — a missing
> settings file, a stale value, or a future panel added later all fail safe with no corruption
> handling needed. The file browser's "Places" now has a **+** button (opens a folder picker) and
> **right-click a bookmark to remove it**, alongside the existing Home/Recordings buttons; bookmarks
> persist the same way. Neither can be verified headlessly — no engine/model impact, so all unit
> tests and the full bounce-tool check suite are unchanged — try moving a panel or adding a bookmark,
> then quitting and relaunching, to confirm both come back where you left them.
>
> That closes out every item from the docking-system and file-management-pane plans.
>
> **First item of the next plan is done: the piano roll now labels its own rows.** Every row shows
> its pitch ("C4", "C#4", ...) in a left-hand gutter, the same way the arrangement view already
> names its track lanes — previously there was no way to tell which row was which pitch besides
> counting black/white key shading. Two small pure-math helpers moved out to reusable, tested
> homes rather than staying as private, untested `PianoRoll` details: `engine::MidiNote.h` gained
> `midiNoteName()` and `isBlackKey()`, and the row↔pitch conversion math itself moved into a new
> `PianoRollGeometry` (mirroring `TimelineGeometry`) — all unit-tested headless for the first time
> (7 new test cases, 57 total). Hover-row highlighting and heavier octave-boundary lines came along
> for free; scrolling/zoom and drag-to-resize notes were deliberately left out of this pass. Pure UI
> change, zero engine/model impact — the bounce tool's full check suite, including
> `rmsDry=0.149266`, is unchanged. The rendering itself needs a live look to confirm it reads well.
>
> **MIDI import/export is done too** — `File > Import MIDI...` / `File > Export MIDI...`, built on
> JUCE's own `juce::MidiFile` (no new dependency). Import creates one `Instrument` track per
> imported MIDI track that actually has notes (tick positions convert to beats via the file's own
> ticks-per-quarter-note, note on/off pairs via JUCE's built-in pairing); export flattens each
> track's clips onto one continuous sequence, including a tempo event from the song's BPM. The
> engine has a single global tempo, not a tempo map — so a source file's *first* tempo event sets
> the imported BPM, and any further tempo changes are counted and reported ("Imported 2 track(s) at
> 128.0 BPM (3 further tempo change(s) not imported)") rather than silently dropped or misapplied.
> Verified by a new `midiRoundTripWorks` bounce-tool check: export a three-note song, re-import it,
> confirm the tempo and every note's beat/pitch/velocity survived — passed first try. All 57 unit
> tests and the bounce tool's full check suite, including `rmsDry=0.149266`, are unchanged.
>
> **File manager 2.0 is done** — the file-browser pane now has a sortable, color-coded detail grid
> (`FileGrid`, `juce::TableListBox`) beneath the existing folder tree: Name/Type/Size/Modified/
> Duration, colored by type (audio/MIDI/project/other), showing whichever folder was last clicked.
> The existing tree keeps its drag-into-arrangement behavior completely unchanged — rather than
> splitting it into a folders-only tree with a separate drag-capable grid, the grid was added as a
> detail companion underneath it, so there's still exactly one drag mechanism to trust. Right-click
> either one for **New Folder / Rename / Delete** (Delete asks for confirmation first — a real,
> irreversible filesystem operation). Projects can now set a **root folder**
> (`model::Song::projectRootFolder`, format bumped to `LOOPER 10`) via **File > Set Project Root
> Folder...**, which then shows as an always-present "Places" entry. Caught in review before it
> shipped: an ordering bug where hiding that button *before* `addAndMakeVisible` did nothing, since
> `addAndMakeVisible` unconditionally forces visibility true — fixed by hiding it after instead. No
> engine impact — all 57 unit tests and the bounce tool's full check suite, including
> `rmsDry=0.149266`, are unchanged. The grid, dialogs, and folder operations are all JUCE-dependent
> and need a live try, Delete especially since it's irreversible.
>
> **Drum kits are done — the last item, and the whole plan is now complete.** A new **Drum** track
> type gives each row its own independent one-shot sample (Kick/Snare/Hat/Other by default,
> `model::addTrack` auto-populates them) instead of every note sharing one melodic synth timbre.
> `engine::DrumKitNode` is built the same way the existing synth wraps `juce::Synthesiser` —
> reusing its polyphony rather than a bespoke voice pool — with a custom `DrumSampleVoice` that
> plays a pad's assigned sample once to the end regardless of the note's length, *ignoring
> note-off* (confirmed against JUCE's own `Synthesiser::noteOff` before relying on it: a normal
> note-off can't be told apart from `allowTailOff` alone without checking how JUCE calls it — a
> hard stop is the only thing that cuts a hit short). A new **Add Drum** button creates one; a new
> drum-kit editor strip (above the piano roll, Drum tracks only) lists each pad with a "Load..."
> button and accepts a file dragged straight from the file browser to reassign it; the piano roll's
> gutter (from a few steps back) now shows pad names instead of pitch names in this mode.
> Serialization bumped to `LOOPER 11`.
>
> Caught in review, not fully root-caused: `DrumKitEditor`'s *implicit* default constructor was
> rejected by the compiler as a member — the same `Component`+`DragAndDropTarget` multiple
> inheritance works fine elsewhere without an explicit one (e.g. `ArrangementView`), so this wasn't
> simply "that shape needs one"; adding `DrumKitEditor() = default;` fixed it, noted here in case it
> recurs. Verified by a new `drumKitWorks` bounce-tool check — kick/snare hits sound at the right
> times, and a third, deliberately *unassigned* pad whose note fires produces nothing — passed on
> the first run. All 57 unit tests (the serialization round-trip now also covers a Drum track) and
> the bounce tool's full check suite, including `rmsDry=0.149266`, are unchanged.
>
> That's every item from the MIDI-import/file-manager/piano-roll/drum-kit plan. See
> [`docs/PLAN.md`](docs/PLAN.md) for the full history and what might come next.
>
> **Generative sound loops — the first slice of the long-planned AI/generative layer (§10) is
> in.** A new **Generate Loop...** button next to **Add Clip** in the Arrange toolbar opens a
> small dialog and drops a real, editable MIDI clip onto the selected track: a scale-constrained
> melody (pick a root note and one of six scales — Major, Natural Minor, Major/Minor Pentatonic,
> Dorian, Mixolydian) for Instrument/Guitar tracks, or a Euclidean-rhythm drum loop — using
> whichever samples are actually assigned to that track's kit, not the factory defaults — for
> Drum tracks. Everything is deterministic and seeded rather than ML-based, per §10's own
> sequencing ("non-ML first... no model weights"): the same seed always reproduces the same loop,
> and a fresh seed is picked on every click, so getting a different take is Undo + **Generate
> Loop...** again — the same candidate workflow **Add Clip** already gives for a blank clip. Two
> new headless, JUCE-free modules back it — `engine::Scale` (scale membership/snapping/degree
> math) and `engine::GenerativeLoop` (a Euclidean-rhythm engine plus the drum/melodic generators
> built on it) — both fully unit-tested, plus a new bounce-tool check verifying the generated
> patterns actually render to audible sound through the normal synth/drum paths. All 322 headless
> tests and the bounce tool's full check suite, including `rmsDry=0.149266`, are unchanged. See
> [`docs/PLAN.md`](docs/PLAN.md) §25 for the design write-up, including why the drum loop's snare
> stays pinned to the backbeat while its kick and hat vary with the seed.
>
> **Generate Loop... now has a Genre picker.** One more combo box — None, House, Techno, Hip-Hop,
> Trap, Ambient, Lo-Fi — biases the generated pattern's density and swing feel (a genre replaces
> the Density combo's value outright when one is picked), and for Instrument tracks *also* swaps
> in a matching, purpose-tuned synth sound (reusing the existing preset-loading machinery, so it's
> one undo step for both the new clip and the sound change). Guitar tracks get the rhythm bias but
> keep their own tone (they're not driven by a synth at all — see §21); Drum tracks get the rhythm
> bias only, keeping whichever kit is already assigned. Swing is now a genuine parameter on both
> generators, not just a genre side-effect — and writing its own tests caught a real bug before it
> shipped: a note swung later near a pattern's *end* could claim to sound past the pattern's own
> length, now clamped. All 334 headless tests pass and the bounce tool's full suite, including
> `rmsDry=0.149266` and `generativeLoopWorks`, is unchanged. See [`docs/PLAN.md`](docs/PLAN.md)
> §25 ("Genre selection") for the full write-up.
>
> **New synth DSP: a filter envelope, a sub-oscillator, and unison** — the synth engine could only
> ever produce a static, single-oscillator timbre until now. A second, independent envelope can
> sweep the filter cutoff over a note (the "pluck"/"sweep" a Moog-style bass or a synthwave lead
> both depend on), a fixed sub-oscillator adds low-end weight, and up to 7 detuned copies of the
> main oscillator can stack into a wide unison lead. Every existing project is byte-for-byte
> unaffected — `SynthVoice` takes a fast path straight to its original, untouched code whenever
> none of the three new fields are in use, which is every project saved before this landed. Two new
> factory presets show it off: **"Analog Pluck Bass"** (Presets list) — a filtered-down pluck bass
> in the vein of Mutemath's analog synth-bass tone — and **"Cyberpunk Stack"**, reachable by picking
> the new **Synthwave** genre in **Generate Loop...**, a detuned saw-stack lead for
> cyberpunk/synthwave/retrowave-style tracks. A 7th scale, **Phrygian**, joins the Scale dropdown
> for the same "dark"/unsettled melodic color. All 334 headless tests pass; the bounce tool's full
> suite — including `rmsDry=0.149266`, `rmsFiltered=0.103703`, and three new checks proving the new
> DSP actually changes the sound — is unchanged. See [`docs/PLAN.md`](docs/PLAN.md) §26 for the
> full write-up, including why a fast path rather than one unified render loop.
>
> **MIDI recording works — you can play a part in.** `§2` of the plan has promised
> "multi-track audio **+ MIDI** recording" from the start and only the audio half was
> real: incoming MIDI was played through the armed track and never captured, so every
> note in every project had to be drawn with the mouse or generated. Now **Record works
> out what to capture from the armed track's type *and* what is actually plugged in** —
> an Instrument/Drum/Guitar track with a controller connected records MIDI into a new
> clip on that track, positioned where capture actually began (after any count-in) and
> committed as one undoable step, opened in the piano roll the way **Add Clip** opens
> the clip it makes; an Audio track records audio through the existing, untouched path;
> and a synth track with *no* controller attached falls back to a microphone take on a
> new track (what the app did before MIDI recording existed) and says so in the status
> bar rather than silently capturing nothing. No new UI and no mode to forget the state
> of: one Record button that explains itself.
>
> Routing on the armed track's type *alone* was the first attempt and it shipped broken
> — with the default Instrument track selected, Record chose MIDI and captured nothing
> at all on a machine with only a microphone, so the button appeared dead. The fix
> separated the two questions that had been conflated ("what can this track hold?" and
> "what is there to record from?") into `app::RecordSourceChoice`, a JUCE-free decision
> table with every row tested. Fixed alongside it: MIDI inputs were enumerated **once**
> at startup, so a controller plugged in after launch was invisible for the whole
> session — it could not record, and could not even play. They are now re-scanned on a
> slow cadence and again whenever a take is armed.
>
> **Record now asks for microphone permission itself**, instead of telling you to go
> and enable it. macOS shows its permission prompt only once, when an app first opens
> an input — which here happened at launch, before there was any reason to care — and
> after that it never asks again, so a dismissed prompt left recording permanently
> broken with only a "check System Settings, then restart" message to explain it.
> Pressing Record now settles the permission first: never asked → the system prompt
> appears *now*, and on a grant the take starts by itself; already denied → a dialog
> with an **Open Settings** button that opens the Privacy & Security microphone list
> directly; granted after launch → the audio input is simply re-opened
> (`AudioEngine::reopenAudioInput`), so restarting the app has stopped being part of
> the fix. A MIDI take never triggers any of this — a controller needs no microphone.
> Implemented as a small AVFoundation shim (`app::MicrophonePermission`) because JUCE
> has no macOS equivalent; on Windows and Linux it reports "not required", since
> neither has a per-app gate an app can query.
>
> Two new JUCE-free, headless-tested modules back it, following the same "pull the math
> out where it can be tested" rule as `SequencerMath`/`PianoRollGeometry`/`Scale`:
> `engine::MidiCapture` (note pairing — velocity-0 note-ons treated as note-offs, notes
> still held at the end of the take clamped rather than dropped, FIFO retrigger, unmatched
> note-offs ignored) and `engine::MidiRecorder` (the capture state machine, deliberately
> mirroring `AudioRecorder`'s arm/disarm/lead-in/finished lifecycle, with a pre-allocated
> `rt::SpscRingBuffer` drained on the message thread every timer tick so the ring stays
> small and the take stays unbounded — and overflow *counted*, never swallowed).
>
> The bounce tool earned its keep here: the new `midiRecordingWorks` check failed on its
> first run and caught a real bug, not a test artifact. Skipping a count-in by dropping
> whole blocks — what `AudioRecorder` does — discards the block the lead-in ends *inside*,
> and that block holds the downbeat the count-in was counting to, i.e. exactly where a
> player puts their first note. Capture now starts mid-block at the exact sample the
> count-in ended on. All 562 headless tests pass and the full bounce suite, including
> `rmsDry=0.149266` and `rmsFiltered=0.103702`, is unchanged — nothing here touches the
> signal path. See [`docs/PLAN.md`](docs/PLAN.md) §27. **What can't be verified
> headlessly: whether a real hardware controller reaches the callback and records in
> time — try it live.**
>
> **Loops now follow the project tempo.** An imported loop used to play at whatever
> tempo it was recorded at, against everything else — `AudioFilePlayerNode` deliberately
> kept its read position in samples, and the only fix was `applySpeedAndPitch`, which
> destructively rewrote the samples at a ratio you worked out yourself. Now importing a
> file **detects its tempo** (`engine::TempoDetect` — spectral-flux onsets,
> autocorrelation, explicit octave resolution) and warps it to the project, reporting
> what it did: *"Imported: break.wav (174.0 BPM, warped to 120.0)"*. **Edit** gains
> *Warp Clip to Project Tempo*, *Detect Clip Tempo...* and *Set Project Tempo from Clip*.
> Warping is non-destructive and pitch-preserving, and the stretch is **pre-rendered on
> the message thread** — which meant `AudioFilePlayerNode` needed no changes at all: a
> warped clip arrives as an ordinary buffer that is simply the right length. Recorded
> takes are never analysed or warped (a take is at the project tempo by definition).
> Detection is only acted on when confident *and* the tempo differs; anything less is
> stored and offered rather than silently applied. See [`docs/PLAN.md`](docs/PLAN.md) §29.
>
> **Sidechain compression works** — a compressor can now take its detector from another
> track, so a bass can duck to a kick. `Compressor::gainFor` already took an explicit
> detector and returned a gain rather than applying it, so no DSP changed; what was
> missing was routing, and every track already rendered into its own scratch buffer, so
> a track's isolated signal only had to be made readable. Pick the source from the
> **Sidechain** box on a compressor slot in the Track FX pane. The document stores a
> track *id*, not an index, so deleting or reordering tracks can't silently re-point it;
> render order only changes when a sidechain exists, so a project without one still
> renders bit-identically (`rmsDry=0.149266` unchanged); and because `processBlock` is
> shared by playback and export, bounces duck exactly as playback does. See
> [`docs/PLAN.md`](docs/PLAN.md) §30.
>
> **Group buses** close the routing gap sidechaining left open — a drum bus you can
> compress as a unit, one fader for a whole group. **Add Bus** in the mixer creates one;
> each track strip gains an **Out** picker to route into it. The design decision that
> made it small: a bus *is* a track (`TrackType::Bus`) that receives other tracks'
> output instead of generating any — so the mixer strip, Track FX pane, selection, undo,
> automation, metering and serialization all work unchanged, and putting a compressor on
> the drum bus needed no new UI at all. A bus is never silenced by another track's solo
> (soloing a kick has to keep playing *through* the drum bus), though its own mute mutes
> the whole group; routing is stored as a track id so deleting or reordering can't
> silently re-route audio; and a routing left pointing at something that is no longer a
> bus falls back to the master. See [`docs/PLAN.md`](docs/PLAN.md) §31.
>
> **Automation is finally visible and editable.** It has been real in the engine for a
> long time — written by touching a fader with **Rec Auto** armed, ramped
> sample-accurately, exported correctly — and completely invisible: `AutomationLane`
> could only ever *gain* points, so a fader move that went slightly wrong could only be
> fixed by clearing the lane and performing it again. A new **Automation** panel draws
> the curve for the selected track's Volume, Pan or Send, sharing the arrangement's
> zoom and playhead so breakpoints line up with the bars. Click empty space to add a
> point (and keep dragging it), drag to move, right-click to remove; one undo step per
> gesture. It is a pane rather than a row in the arrangement on purpose — that view
> already carries three drag protocols whose separation is load-bearing, and a fourth
> sharing a coordinate space with clip dragging is how a click that meant "add a
> breakpoint" ends up moving a clip. Nine new GUI tests drive the real component through
> real mouse events. See [`docs/PLAN.md`](docs/PLAN.md) §32.
>
> **The guitar sounds better — phases 1 and 2 of [§33](docs/PLAN.md).** Four things the
> instrument never had: **velocity now shapes timbre** (picking harder is brighter, not
> just louder — every note used to have an identical spectrum, which is most of why a
> programmed part sounded machine-gunned); **the strings couple at the bridge**, so
> struck strings drive the others and chords bloom instead of stacking as six
> independent notes; **stereo width** by panning the strings, mono-compatible by
> construction since they are distinct signals rather than delayed copies; and **string
> stiffness**, which stretches the partials progressively sharp the way a real wound
> string does — a genuine component of drop-tuned growl. All four are sliders on the
> fretboard pane, and setting them to zero returns the instrument to exactly what it
> was. Four of the five bugs found along the way were caught by tests rather than by
> ear, including bridge coupling that transferred *precisely zero* energy and a pick
> jitter that turned out to be a tonal change rather than a variation.
>
> **The amp and cabinet followed.** `DriveEffect` can now be a **two- or three-stage
> cascade** rather than one clipper, with a coupling network between stages — the bass
> is rolled off *before the next clipper sees it*, which is what makes a high-gain amp
> tight instead of muddy. And `CabinetSim` can **convolve a synthesised impulse
> response** instead of filtering: nothing is shipped (it is built at prepare time from
> the cabinet's own filters), and it is short enough to convolve directly, so there is
> no partitioning and **no latency** — which on a guitar someone is playing is the one
> cost you cannot pay. What it buys is the thing a filter cannot express: the same sound
> arriving twice, off the baffle edge and the back of the box, combing with itself. Both
> are opt-in (`stages`, `cabinetIr`), and the Modern Metal preset takes both. Two more
> wrong ideas died to measurement here — a cascade that was supposed to compress more
> and turned out to be ten times *cleaner*, and an IR level-matched on DC gain that
> pushed the preset into clipping.

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
