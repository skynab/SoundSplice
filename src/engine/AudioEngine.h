#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "rt/SpscRingBuffer.h"

#include "engine/AudioClipSlot.h"
#include "engine/AudioFilePlayerNode.h"
#include "engine/DrumKitNode.h"
#include "engine/AudioRecorder.h"
#include "engine/MidiRecorder.h"
#include "engine/TempoDetect.h"
#include "engine/TimeStretch.h"
#include "engine/ClipSlot.h"
#include "engine/DelayEffect.h"
#include "engine/EqEffect.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"
#include "engine/EngineCommand.h"
#include "engine/InstrumentTrack.h"
#include "engine/MasterBusNode.h"
#include "engine/MasteringProcessor.h"
#include "engine/Metronome.h"
#include "engine/PluginHost.h"
#include "engine/PluginNode.h"
#include "engine/Pattern.h"
#include "engine/Transport.h"

#include "model/Effects.h"
#include "model/GuitarSettings.h"

namespace looper::engine
{
/** One audio clip to load onto a track: a file plus its
    [startBeats, startBeats + lengthBeats) window — the AudioEngine-facing
    equivalent of ClipSlot, taking a file instead of already-decoded data.
    See AudioEngine::setTrackAudioClips. */
struct AudioClipSpec
{
    juce::File file;
    double     startBeats  = 0.0;
    double     lengthBeats = 0.0;
    float      gainDb      = 0.0f;

    /**
        Time-stretch applied before playback, in timeStretch()'s terms: 2.0 is
        twice as long, 0.5 half, 1.0 (the default) no stretching at all.

        A *ratio*, not a pair of tempos, deliberately — the engine has no idea
        what a BPM is and does not need one, exactly as it has no idea what
        automation is and takes a curve callback instead. The caller knows the
        project tempo and the clip's own; the engine only has to render.

        The stretch is applied to the decoded audio on the message thread, so
        a warped clip reaches the audio thread as an ordinary buffer that is
        simply the right length. That is why nothing in AudioFilePlayerNode
        changes for this: a phase vocoder is not a real-time operation, and
        pre-rendering means it never has to be one.
    */
    double stretchFactor = 1.0;
};

/** One drum pad to load onto a track: a note number, the file to play when
    it's triggered (File{} = no sample assigned, pad stays silent), and that
    pad's mix settings. `muted` is the *effective* mute — the caller resolves
    the kit's solo state into it (see MainComponent::syncEngineTracks), the
    same "solo overrides, mute always wins" rule tracks use, so the audio
    thread never has to scan the other pads. Defaults are a no-op, so a spec
    built without touching them behaves as it did before these existed. See
    AudioEngine::setTrackDrumKit. */
struct DrumPadSpec
{
    int        noteNumber = -1;
    juce::File file;

    float gainDb         = 0.0f;
    float pan            = 0.0f;
    float pitchSemitones = 0.0f;
    bool  muted          = false;
};

/**
    The headless audio engine. It owns the audio device and is the device
    callback. Instrument tracks live in a fixed pre-allocated pool, so the UI
    changes the "song" by activating slots and submitting clip lists — no
    real-time graph editing. The UI interacts only by posting commands, calling
    the thread-safe control methods (which use lock-free FIFOs / atomics), and
    reading published atomics.
*/
class AudioEngine final : public juce::AudioIODeviceCallback,
                          public juce::MidiInputCallback
{
public:
    static constexpr int kMaxTracks = 8;

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }
    juce::MidiKeyboardState&  keyboardState() noexcept { return keyboardState_; }

    void postCommand(const EngineCommand& command) noexcept { commandQueue_.push(command); }

    /** Decode an audio file into RAM and hand it to the global preview player
        (used by the File > Import Audio quick-preview). Message thread. */
    bool loadAudioFile(const juce::File& file);

    /** Estimates @p file's tempo (see engine::detectTempo), decoding it
        through the same cache setTrackAudioClips uses so a file already
        loaded isn't read twice. Message thread — analysis is not instant on a
        long file. Returns an unusable estimate if the file can't be read. */
    TempoEstimate detectFileTempo(const juce::File& file);

    /** Reads just @p file's header to get its duration — cheap (no sample
        decode), unlike loadAudioFile/setTrackAudioClips. Returns 0.0 if the
        file can't be read. Used to size a new clip to its actual duration
        rather than a fixed guess. Message thread. */
    double probeDurationSeconds(const juce::File& file);

    /** Replaces a track's whole audio-clip list, decoding any file not
        already cached (see decodeOrGetCached — decoded audio is cached by
        path, so calling this again with the same files, even on other
        tracks, never re-decodes them). Each clip plays only within its own
        [startBeats, startBeats + lengthBeats) window, same rule as
        setTrackClips (MIDI); give a single-clip track an effectively
        unbounded lengthBeats for the original "plays once from its start, no
        other gating" behaviour. Message thread. Returns false if any clip's
        file couldn't be read (the others still load). */
    bool setTrackAudioClips(int index, const std::vector<AudioClipSpec>& clips);

    /**
        Routes @p sourceTrackIndex's signal into @p index's compressor as its
        detector — sidechain ducking. -1 (the default) means the compressor
        listens to its own input, i.e. an ordinary compressor.

        Takes indices because that is what the engine's fixed pool is addressed
        by; the document stores a track *id* and MainComponent resolves it, so
        deleting or reordering a track can't silently re-point a sidechain at a
        different instrument. Message thread.
    */
    void setTrackSidechainSource(int index, int sourceTrackIndex);

    /** Marks a track as a group bus: it generates nothing and instead receives
        whatever other tracks route into it. Message thread. */
    void setTrackIsBus(int index, bool isBus);

    /** Routes @p index's output into the bus track at @p busTrackIndex, or -1
        for straight to the master. Message thread. */
    void setTrackOutputBus(int index, int busTrackIndex);

    /** Chooses which instrument a track's notes drive. Explicit rather than
        inferred: unlike audio clips, every note-driven instrument produces
        sound for any note it receives, so the routing has to be stated.
        Message thread. */
    void setTrackInstrument(int index, TrackInstrument instrument);

    /** Per-track guitar settings (see model::GuitarSettings / GuitarNode).
        Takes the settings struct rather than a positional float list: with the
        pickup added there are seven scalars, most in similar ranges, and a
        transposed pair would be silent at the call site.
        Message thread. */
    void setTrackGuitarSettings(int index, const model::GuitarSettings& settings);
    void setTrackGuitarTuning(int index, const std::array<int, kNumGuitarStrings>& tuning);

    /** Which note a guitar track is currently sounding on a given string, or
        -1. Lock-free readout for the fretboard. */
    int guitarNoteOnString(int index, int stringIndex) const noexcept
    {
        return (index >= 0 && index < kMaxTracks)
                   ? tracks_[(size_t) index].guitar.noteOnString(stringIndex) : -1;
    }

    /** Replaces a track's whole drum-kit pad→sample mapping, decoding any
        file not already cached (see decodeOrGetCached — same cache
        setTrackAudioClips uses, so a sample shared across pads or tracks is
        never decoded twice). A pad with no file (or one that fails to
        decode) stays silent. Message thread. */
    void setTrackDrumKit(int index, const std::vector<DrumPadSpec>& pads);

    // Metronome (thread-safe atomics). Summed in after the master chain, so
    // it never passes through the master effects or reaches the meter — and
    // the offline renderer has none at all, so it can't reach a bounce.
    void setMetronomeEnabled(bool enabled) { metronome_.setEnabled(enabled); }
    bool isMetronomeEnabled() const        { return metronome_.isEnabled(); }
    void setMetronomeLevel(float level)    { metronome_.setLevel(level); }

    /** Bars of count-in before a recording starts capturing (0 = none). The
        click sounds through the count-in whether or not the metronome is
        otherwise switched on. */
    void setCountInBars(int bars) { countInBars_ = juce::jmax(0, bars); }
    int  countInBars() const noexcept { return countInBars_; }

    // ---- recording (message thread) ----
    /** Arms the recorder. Returns false (and arms nothing) if the current
        audio device has no active input channels. Capturing only actually
        happens while the transport is playing, and only after any count-in
        (see setCountInBars) has elapsed. */
    // ---- what is actually connected (message thread) ----
    /**
        Re-enumerates MIDI inputs, registering any that have appeared and
        dropping any that have gone. Returns true if the set changed.

        This used to happen once, in the constructor — so a controller plugged
        in after launch was never routed anywhere: it could not play, let alone
        record, and nothing said why. Called on a slow cadence from the UI
        timer and again whenever a take is armed.
    */
    bool refreshMidiInputs();

    /** True if any MIDI input device is currently registered. What makes
        "record MIDI or record audio?" answerable from what is really plugged
        in rather than from the armed track's type alone. */
    bool hasMidiInput() const;

    /** True if the open audio device actually has input channels — i.e. there
        is something to record audio *from*. Distinct from inputOpenError(),
        which says why opening one failed. */
    bool hasAudioInput() const;

    /**
        Re-opens the audio device asking for input again, and returns true if
        it now has some.

        Exists because the input is opened once at startup, and a permission
        granted *after* that is invisible until something re-asks: the app
        would keep reporting "no audio input" with the microphone switched on
        in System Settings, and the only advice that worked was to restart it.
        Called after a permission grant, so recording can start immediately.

        Falls back to output-only exactly as the constructor does if the input
        still cannot be opened — playback must never be the price of asking.
    */
    bool reopenAudioInput();

    /** Why the audio input could not be opened, or empty if it did.

        Non-empty means the app fell back to output only: playback works,
        recording does not. On macOS the usual cause is a denied microphone
        permission. */
    juce::String inputOpenError() const { return inputOpenError_; }

    /** Arms a take, streamed to @p destination as it is played. Returns false
        if there is no input device or the file could not be opened. */
    bool beginRecording(const juce::File& destination);

    /** True while a count-in is still running — the transport is rolling but
        nothing is being captured yet. */
    bool isCountingIn() const noexcept { return recorder_.leadInRemaining() > 0; }
    /** Stops capturing; the take becomes readable once isRecordingFinished(). */
    void stopRecording() { recorder_.disarm(); }
    bool isRecordingFinished() const noexcept { return recorder_.isFinished(); }
    int64_t recordedSampleCount() const noexcept { return recorder_.recordedSampleCount(); }

    /** Samples lost because the disk could not keep up. Non-zero means the
        take has a gap in it — see AudioRecorder::droppedSampleCount. */
    int64_t recordedDroppedSamples() const noexcept { return recorder_.droppedSampleCount(); }

    /** Where the transport was when capture began, in samples, or -1. */
    int64_t recordedTakeStartSample() const noexcept { return recorder_.startPlayheadSamples(); }

    /** Closes the take's file and returns it; empty if nothing was captured.
        Valid only after isRecordingFinished() is observed true. */
    juce::File finishRecordedTake() { return recorder_.finishTake(); }

    // ---- MIDI recording (message thread) ----
    /**
        Arms a MIDI take: incoming notes are captured instead of only being
        played through the armed track.

        The audio counterpart of this, beginRecording, can fail (no input
        device, unopenable file) and so returns bool. This cannot: MIDI capture
        needs no device to be open and no file to exist — a controller that is
        absent simply sends nothing, which is an empty take rather than an
        error. Count-in is shared with the audio path (see setCountInBars).
    */
    void beginMidiRecording();

    /** True while a MIDI take's count-in is still running. */
    bool isMidiCountingIn() const noexcept { return midiRecorder_.leadInRemaining() > 0; }
    void stopMidiRecording() { midiRecorder_.disarm(); }
    bool isMidiRecordingFinished() const noexcept { return midiRecorder_.isFinished(); }
    int64_t midiRecordedEventCount() const noexcept { return midiRecorder_.capturedEventCount(); }

    /** Events lost because the message thread stopped draining. Non-zero means
        the take is missing notes — see MidiRecorder::droppedEventCount. */
    int64_t midiRecordedDroppedEvents() const noexcept { return midiRecorder_.droppedEventCount(); }

    /** Where the transport was when MIDI capture began / stopped, in samples,
        or -1. The start is what positions the clip; the end is what bounds a
        note still held when the take stopped. */
    int64_t midiTakeStartSample() const noexcept { return midiRecorder_.startPlayheadSamples(); }
    int64_t midiTakeEndSample() const noexcept { return midiRecorder_.endPlayheadSamples(); }

    /** Moves everything captured since the last call onto @p destination.
        Call on a timer during the take and once more after
        isMidiRecordingFinished(), so the ring never has to hold a whole take
        (see MidiRecorder). */
    void drainMidiTake(std::vector<RecordedMidiEvent>& destination) { midiRecorder_.drain(destination); }

    /** Dry input monitoring: input summed straight to the output, after the
        master bus. Off by default — monitoring a built-in microphone through
        speakers is a feedback loop. Message thread. */
    void setInputMonitoring(bool on) { inputMonitoring_.store(on, std::memory_order_relaxed); }
    bool isInputMonitoring() const noexcept { return inputMonitoring_.load(std::memory_order_relaxed); }
    void setInputMonitorGain(float gain) { inputMonitorGain_.store(gain, std::memory_order_relaxed); }

    // ---- multi-track control (message thread) ----
    int  maxTracks() const noexcept { return kMaxTracks; }
    void setActiveTrackCount(int count);
    /** Replaces a track's whole clip list. Each clip plays only within its own
        [startBeats, startBeats + lengthBeats) window; give a single-clip track
        an effectively unbounded lengthBeats to keep it looping indefinitely. */
    void setTrackClips(int index, const std::vector<ClipSlot>& clips);
    void setTrackMuted(int index, bool muted);
    void setTrackSolo(int index, bool solo);

    /** Hands the audio thread a new tempo map.

        The scalar tempo goes through the command queue like every other
        parameter, but a map is a vector — so it uses the same pointer swap the
        rest of the engine's variable-sized state uses (see
        Sequencer::submitClips): built here, applied on the audio thread, and
        the old one handed back to be freed in pump().

        Message thread. */
    void setTempoChanges(const std::vector<TempoChange>& changes);

    /** Whether track @p index currently produces sound in the mix: active, not
        muted, and either soloed or with nothing else soloed.

        The rule itself lives in InstrumentTrack::render — "solo overrides, mute
        always wins" — and this reports the same answer from the same atomics
        rather than restating it. Anything deciding *which* tracks to export as
        stems needs exactly this, and a second copy of the rule would be one
        more thing to drift. */
    bool trackContributesToMix(int index) const noexcept;
    void setTrackGainDb(int index, float gainDb);
    void setTrackPan(int index, float pan);
    void setTrackSendLevel(int index, float level);

    // ---- session view (message thread) ----
    /** Replaces a track's session column. Slot index is the scene. */
    void setTrackSessionSlots(int index, const std::vector<SessionSlotData>& slots);

    /** Asks a track to start @p sceneIndex at the next launch boundary. */
    void launchSessionSlot(int index, int sceneIndex);

    /** Asks a track to stop whatever session clip it's playing, handing it
        back to the arrangement. */
    void stopSessionSlot(int index);

    /** Launches a whole scene across every active track — a track with an
        empty slot in that scene stops rather than carrying on, so a scene is a
        complete statement of what should be playing. */
    void launchScene(int sceneIndex);

    /** Stops every track's session clip. */
    void stopAllSessionSlots();

    /** Which session slot a track is currently playing, or -1. Lock-free
        readout for the session grid. */
    int sessionSlotPlaying(int index) const noexcept
    {
        return (index >= 0 && index < kMaxTracks)
                   ? tracks_[(size_t) index].session.playingSlotForUI() : -1;
    }

    /** How long a launch boundary is, in beats. 0 launches immediately;
        the default of one bar is what makes launching musical. */
    void setLaunchQuantumBeats(double beats) { launchQuantumBeats_.store(beats, std::memory_order_relaxed); }
    double launchQuantumBeats() const noexcept { return launchQuantumBeats_.load(std::memory_order_relaxed); }

    /** Replaces a track's automation curves. Sample-accurate: the track
        ramps them across each block itself rather than the UI poking a
        value in every 33ms. Pass nullptr-equivalent (an empty set) to
        clear. Message thread. */
    void setTrackAutomation(int index, const TrackAutomation& curves);
    void setArmedTrack(int index);

    // Per-track synth timbre (see model::SynthSettings / SynthInstrumentNode)
    // — meaningless for a Drum track, but harmless to set regardless since
    // it's simply not read while `instrument` routes notes elsewhere.
    void setTrackSynthWaveform(int index, int waveform);
    void setTrackSynthAttackMs(int index, float ms);
    void setTrackSynthDecayMs(int index, float ms);
    void setTrackSynthSustain(int index, float level);
    void setTrackSynthReleaseMs(int index, float ms);
    void setTrackSynthFilterEnabled(int index, bool enabled);
    void setTrackSynthFilterMode(int index, int mode);
    void setTrackSynthFilterCutoff(int index, float hz);
    void setTrackSynthFilterResonance(int index, float q);
    void setTrackSynthGainDb(int index, float db);
    void setTrackSynthFilterEnvAmount(int index, float hz);
    void setTrackSynthFilterEnvAttackMs(int index, float ms);
    void setTrackSynthFilterEnvDecayMs(int index, float ms);
    void setTrackSynthFilterEnvSustain(int index, float level);
    void setTrackSynthFilterEnvReleaseMs(int index, float ms);
    void setTrackSynthSubOscEnabled(int index, bool enabled);
    void setTrackSynthSubOscLevel(int index, float level);
    void setTrackSynthUnisonVoices(int index, int voices);
    void setTrackSynthUnisonDetuneCents(int index, float cents);

    /** Replaces a track's insert chain with nodes of these kinds, in order.
        Structural only: rebuilding allocates (on this thread) and resets every
        tail in the chain, so parameter changes must go through the setters
        below instead. A no-op when the structure already matches, which is
        what keeps an unrelated document edit from glitching a delay tail. */
    /** Returns true if the chain was actually rebuilt — which destroys the
        old nodes, hosted plugins included. The caller must close anything
        pointing at them first (see MainComponent::closePluginEditors): an
        editor outliving its processor is a crash, not a glitch. */
    bool setTrackEffectChain(int index, const std::vector<EffectSlotSpec>& slots);

    /** The plugin host, for the UI's browser and scan. Message thread. */
    PluginHost& pluginHost() noexcept { return pluginHost_; }

    /** Applies one chain slot's parameters, addressed by position. Safe from
        the message thread: these are atomics inside nodes it built and still
        holds a pointer to. Addressed by index rather than by kind so a chain
        with two filters is editable at all. */
    void setTrackEffectSlotParams(int index, int slotIndex, const EffectSlotParams& params);

    /** A hosted plugin in a track's chain, for opening its editor. nullptr if
        that slot isn't a plugin (or the chain is a rebuild behind). Message
        thread. */
    PluginNode* trackPluginNode(int index, int slotIndex);

    // Master effects (thread-safe atomics; safe to call from the message thread).
    void setMasterFilterEnabled(bool enabled)  { masterFilter_.setEnabled(enabled); }
    void setMasterFilterMode(int mode)         { masterFilter_.setMode(mode); }
    void setMasterFilterCutoff(float hz)       { masterFilter_.setCutoff(hz); }
    void setMasterFilterResonance(float q)     { masterFilter_.setResonance(q); }

    void setMasterDelayEnabled(bool enabled)   { masterDelay_.setEnabled(enabled); }
    void setMasterDelayTimeMs(float ms)        { masterDelay_.setTimeMs(ms); }
    void setMasterDelayFeedback(float amount)  { masterDelay_.setFeedback(amount); }
    void setMasterDelayMix(float amount)       { masterDelay_.setMix(amount); }

    void setMasterReverbEnabled(bool enabled)  { masterReverb_.setEnabled(enabled); }
    void setMasterReverbRoomSize(float v)      { masterReverb_.setRoomSize(v); }
    void setMasterReverbDamping(float v)       { masterReverb_.setDamping(v); }
    void setMasterReverbMix(float v)           { masterReverb_.setMix(v); }

    void setMasterEqEnabled(bool enabled)      { masterEq_.setEnabled(enabled); }
    void setMasterEqBassDb(float db)           { masterEq_.setBassDb(db); }
    void setMasterEqMidDb(float db)            { masterEq_.setMidDb(db); }
    void setMasterEqTrebleDb(float db)         { masterEq_.setTrebleDb(db); }

    // Shared send bus: every track can send a pre-fader portion of its signal
    // into one always-fully-wet effect — reverb or delay, chosen by
    // setSendBusEffectType — which mixes back into the master before the
    // master's own effects chain (thread-safe atomics).
    void setSendBusEnabled(bool enabled)  { sendBusEnabled_.store(enabled, std::memory_order_relaxed); }
    /** 0 = reverb, 1 = delay. */
    void setSendBusEffectType(int type)   { sendBusEffectType_.store(type, std::memory_order_relaxed); }
    void setSendBusRoomSize(float v)      { sendBusReverb_.setRoomSize(v); }
    void setSendBusDamping(float v)       { sendBusReverb_.setDamping(v); }
    void setSendBusDelayTimeMs(float ms)  { sendBusDelay_.setTimeMs(ms); }
    void setSendBusDelayFeedback(float v) { sendBusDelay_.setFeedback(v); }
    void setSendBusReturnLevel(float v)   { sendReturnGain_.store(v, std::memory_order_relaxed); }

    /** Housekeeping to run periodically on the message thread (frees retired clips/patterns). */
    void pump() noexcept;

    /** Re-opens the output device the system currently considers default,
        if that isn't the one already open. Returns the device's name when it
        switched, or an empty string when nothing needed doing.

        This exists because JUCE opens a device *by name* and then keeps it.
        initialiseWithDefaultDevices() picks the default that was current at
        launch, so plugging in headphones afterwards changes the system
        default while the app carries on holding the built-in speakers —
        which is heard as the app ignoring the headphones entirely.

        Message thread only: it closes and re-opens the audio device. */
    juce::String followSystemDefaultOutput();

    /** The system's current default output device name, or empty if that
        can't be determined. */
    juce::String systemDefaultOutputName();


    /** Renders @p lengthBeats of the project starting at @p startBeats, offline
        and faster than real time, through **the same processBlock() the device
        callback uses**.

        That sharing is the whole point rather than an optimisation. The old
        export re-implemented the signal path by hand and had silently drifted
        from it: it omitted audio clips entirely, every clip after the first,
        every per-track effect chain, and the master EQ. Any export built as a
        second copy of the mixer will drift again the next time either side
        changes; one that calls the mixer cannot.

        Message thread only, and it **suspends the audio device for its
        duration** — the engine's mixer state is single-writer by design, so
        rendering while the device thread is also in processBlock() would be a
        data race. Playback stops for the length of the render and the
        transport is restored afterwards.

        Deliberately excludes the metronome, matching what you'd want exported
        and what the device callback already keeps outside the master bus.

        A struct rather than a growing list of positional arguments: rendering
        a stem differs from rendering the mix in two more ways, and five
        anonymous values at a call site is where transposition bugs live. */
    struct OfflineRenderOptions
    {
        double startBeats  = 0.0;
        double lengthBeats = 0.0;

        /** Renders at this rate instead of the device's; 0 means the device's.
            The whole engine is re-prepared for it, so the synths, effects and
            any hosted plugins all run natively at the export rate rather than
            the mix being resampled afterwards — which is both simpler and
            better, since the only resampler here is the linear one in
            AudioEdits: fine for placing a clip, not fine for a master. */
        double sampleRate = 0.0;

        int blockSize = 512;

        /** Renders only this track, for a stem; -1 renders the whole mix.

            The track still decides for itself whether it sounds — mute, solo
            and gain all apply exactly as they do live, because this only
            changes *which* tracks are asked to render, not what they do when
            asked. */
        int soloTrack = -1;

        /** Whether the master bus runs: the master filter, delay, reverb, EQ,
            the mastering rack and the master gain.

            False for stems. Running the mastering rack's limiter on each stem
            separately would limit the material once per stem and leave the set
            summing to something quite unlike the mix — stems are pre-master by
            definition, and the master file is where that processing belongs. */
        bool applyMasterBus = true;

        /** Called periodically from whichever thread is rendering, with
            progress in 0..1. Return false to cancel.

            Cancelling still restores the engine - the device callback comes
            back, the transport is put back, and everything is re-prepared for
            the device's rate - and renderOffline then returns an **empty**
            buffer. Empty rather than partial on purpose: a caller that wrote
            whatever it got back would otherwise produce a truncated file and
            report success. */
        std::function<bool (double)> onProgress;
    };

    juce::AudioBuffer<float> renderOffline(const OfflineRenderOptions& options);

    juce::String loadedClipName() const             { return loadedClipName_; }
    double       loadedClipSeconds() const noexcept { return loadedClipSeconds_; }

    // ---- lock-free UI readouts ----
    bool    isPlaying() const noexcept       { return transport_.playingForUI(); }
    int64_t playheadSamples() const noexcept { return transport_.playheadForUI(); }
    double  sampleRate() const noexcept      { return sampleRate_.load(std::memory_order_relaxed); }
    float   masterPeak(int channel) const noexcept { return master_.peak(channel); }
    /** Gain reduction the mastering rack's limiter is applying, in dB. */
    float   masteringReductionDb() const noexcept { return mastering_.currentReductionDb(); }

    /** The whole mastering rack in one call — it's a single settings struct
        on the document, so pushing it field-by-field would just be more ways
        to forget one. Message thread; every setter underneath is an atomic. */
    void setMastering(const model::MasteringSettings& s)
    {
        mastering_.setEnabled(s.enabled);
        mastering_.setLowShelf(s.lowShelfHz, s.lowShelfDb);
        mastering_.setPeak(s.peakHz, s.peakDb, s.peakQ);
        mastering_.setHighShelf(s.highShelfHz, s.highShelfDb);
        mastering_.setExciter(s.exciterAmount, s.exciterCrossoverHz);
        mastering_.setWidth(s.width);
        mastering_.setReverb(s.reverbAmount, s.reverbRoomSize);
        mastering_.setMaximizer(s.maximizerInputDb, s.maximizerCeilingDb, s.maximizerReleaseMs);
        mastering_.setOutputGainDb(s.outputGainDb);
    }
    float   trackPeak(int index, int channel) const noexcept
    {
        return (index >= 0 && index < kMaxTracks) ? tracks_[(size_t) index].peak(channel) : 0.0f;
    }

    // ---- juce::AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    /** Dry input straight to the output, ramped. Called from the device
        callback after processBlock — see the comment there. */
    void mixInputMonitoring(juce::AudioBuffer<float>& output,
                            const float* const* inputChannelData,
                            int numInputChannels, int numSamples) noexcept;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // ---- juce::MidiInputCallback ----
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

private:
    void drainCommandQueue() noexcept;

    /** One block of the mixer: tracks, send bus, file player, master chain,
        transport advance. Shared verbatim by the device callback and
        renderOffline() so an export cannot drift from what's heard — see
        renderOffline's comment for why that sharing is the design and not a
        convenience.

        @p midi is the live input for this block (empty when rendering
        offline). The metronome is deliberately *not* here: it sits outside
        the master bus so it stays off the meter and out of exports. */
    /** @p soloTrack renders only that track (-1 = all), and @p applyMasterBus
        runs the master chain. Both defaulted, so the device callback's call is
        unchanged — this exists for offline stem rendering. */
    void processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi,
                      const ProcessContext& context,
                      int soloTrack = -1, bool applyMasterBus = true) noexcept;

    /** Prepares every node for @p sampleRate / @p blockSize. Called on device
        start, and again either side of an offline render — re-preparing
        resets every delay line and reverb tail, so a render starts from
        silence rather than inheriting whatever was ringing when it began,
        and playback afterwards doesn't inherit the render's tails either. */
    void prepareAll(double sampleRate, int blockSize);

    /** Decodes @p file fully into RAM. Returns nullptr if it can't be read. Message thread. */
    std::unique_ptr<ClipData> decodeAudioFile(const juce::File& file);
    /** As above, but cached by absolute path — repeated calls (even from
        different tracks) reuse the same decoded ClipData instead of
        re-reading the file. Message thread only; the cache is never touched
        from the audio thread. */
    std::shared_ptr<ClipData> decodeOrGetCached(const juce::File& file);

    /** The decoded audio of @p file, time-stretched by @p stretchFactor (see
        AudioClipSpec). Returns the unstretched cache entry when the factor is
        1. Message thread — this runs a phase vocoder, which is emphatically
        not something to do in a callback. */
    std::shared_ptr<ClipData> warpedOrGetCached(const juce::File& file, double stretchFactor);

    juce::AudioDeviceManager          deviceManager_;
    juce::AudioFormatManager          formatManager_;
    juce::MidiMessageCollector        midiCollector_;
    juce::MidiKeyboardState           keyboardState_;
    juce::MidiBuffer                  incomingMidi_;
    rt::SpscRingBuffer<EngineCommand> commandQueue_ { 1024 };

    std::array<InstrumentTrack, kMaxTracks> tracks_;
    std::atomic<int>                        armedTrack_ { 0 };

    AudioFilePlayerNode filePlayer_;
    FilterEffect        masterFilter_;
    DelayEffect         masterDelay_;
    ReverbEffect        masterReverb_;
    EqEffect            masterEq_;
    MasteringProcessor  mastering_;
    MasterBusNode       master_;
    Transport           transport_;

    // Send bus: accumulated from every track's pre-fader send, passed through
    // one always-fully-wet effect (sendBusEffectType_: 0 = reverb, 1 =
    // delay), and mixed back into the main output before the master effects
    // chain. Both effect instances stay prepared/configured regardless of
    // which is selected, so switching types takes effect immediately.
    juce::AudioBuffer<float> sendBus_;
    ReverbEffect             sendBusReverb_;
    DelayEffect              sendBusDelay_;
    std::atomic<bool>        sendBusEnabled_    { false };
    std::atomic<int>         sendBusEffectType_ { 0 };
    std::atomic<float>       sendReturnGain_    { 0.0f };

    std::atomic<double> sampleRate_ { 0.0 };

    // The tempo map, handed over whole rather than a field at a time. Sized
    // small: a map is submitted when a project loads or a change is edited,
    // never per block.
    using TempoChangeList = std::vector<TempoChange>;
    rt::SpscRingBuffer<TempoChangeList*> tempoInbox_   { 8 };
    rt::SpscRingBuffer<TempoChangeList*> tempoReclaim_ { 16 };

    juce::String  inputOpenError_;

    // MIDI input identifiers this engine has registered a callback for.
    // Tracked rather than re-derived from juce::MidiInput::getAvailableDevices()
    // because registering the same device twice would deliver every message
    // twice, and a device that has been unplugged is no longer in that list at
    // all — so it could never be unregistered. Message thread only.
    juce::StringArray registeredMidiInputs_;

    AudioRecorder recorder_;
    MidiRecorder  midiRecorder_;

    // Scratch for translating a block's juce::MidiBuffer into the PODs
    // MidiRecorder takes. Sized once, on the message thread, so the audio
    // thread never grows it — and capped, so a stuck controller spraying
    // events can't make a block's translation unbounded.
    std::vector<RecordedMidiEvent> midiCaptureScratch_;

    // Drains the recorder's FIFO to disk. Started once and left running: it
    // idles when nothing is recording, and starting a thread at the moment the
    // user hits record is exactly when not to be doing it.
    juce::TimeSliceThread recordWriterThread_ { "LooperRecordWriter" };

    std::atomic<bool>  inputMonitoring_  { false };
    std::atomic<float> inputMonitorGain_ { 1.0f };
    float              monitorGainRamp_  = 0.0f; // audio thread only; see the callback
    Metronome     metronome_;
    int           countInBars_ = 0; // message thread only; read when arming
    std::atomic<double> launchQuantumBeats_ { 4.0 }; // one bar of 4/4

    /** Most note events one block will hand to the MIDI recorder. Well past
        anything a human can play in a buffer; a stuck controller past it is
        counted as dropped like any other overflow rather than allowed to grow
        the scratch buffer on the audio thread. */
    static constexpr std::size_t kMaxCapturedEventsPerBlock = 256;

    /** The count-in for a take about to be armed, in samples, from the tempo
        in force where it will start. Message thread; shared by both
        recorders so a MIDI take and an audio take count in identically. */
    int64_t countInLeadInSamples() const;

    /** Translates a block's note messages into PODs and offers them to the
        MIDI recorder. Audio thread; allocation-free. */
    void captureMidi(const juce::MidiBuffer& midi, const ProcessContext& context) noexcept;

    /** Rebuilds and submits a track's chain from chainStructure_. Message
        thread. Also called when the device (re)starts, since a chain must be
        prepared for the sample rate it will actually run at. */
    void rebuildTrackEffectChain(int index);

    // Message-thread view of each track's chain: the structure it was built
    // from, and a pointer to the chain last submitted. The pointer is how
    // parameter setters reach live nodes — safe because the newest chain is
    // never the one being reclaimed.
    std::array<std::vector<EffectSlotSpec>, kMaxTracks> chainStructure_;
    PluginHost                                         pluginHost_;
    std::array<EffectChain*, kMaxTracks>                submittedChain_ {};
    int                                                 currentBlockSize_ = 512;

    juce::String loadedClipName_;
    double       loadedClipSeconds_ = 0.0;

    // Decoded-audio cache, keyed by absolute path (message thread only) — see
    // decodeOrGetCached.
    /** Per-track output bus index, or -1 for the master. Message thread
        writes, audio thread reads. */
    std::array<std::atomic<int>, kMaxTracks> outputBus_;

    /** Per-track sidechain source index, or -1. Message thread writes, audio
        thread reads — hence atomic, like every other per-track control. */
    std::array<std::atomic<int>, kMaxTracks> sidechainSource_;

    /** This block's rendered audio per track, filled in as each renders.
        Audio thread only, and rebuilt every block — a track that produced
        nothing stays null. */
    std::array<const juce::AudioBuffer<float>*, kMaxTracks> trackOutputs_ {};

    /** Handed to a compressor whose source track produced no audio this block.
        Silence is the correct detector reading there — a muted kick should
        stop ducking the bass, not leave it ducking to a stale signal or fall
        back to compressing itself. Sized in prepare, never on the audio
        thread. */
    juce::AudioBuffer<float> silentDetector_;

    std::map<juce::String, std::shared_ptr<ClipData>> audioDecodeCache_;

    /** Warped renderings, at most one per file — keyed by path, holding the
        factor it was rendered at.

        One per path rather than one per (path, factor) on purpose: keying by
        both would grow without bound as someone drags the tempo around, and
        every superseded entry is a whole decoded file's worth of RAM held for
        a tempo nobody is at any more. Changing the tempo re-renders; sitting
        at one tempo costs a single rendering, which is the case that matters.
        Message thread only. */
    struct WarpedClip
    {
        double                    stretchFactor = 1.0;
        std::shared_ptr<ClipData> data;
    };
    std::map<juce::String, WarpedClip> audioWarpCache_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace looper::engine
