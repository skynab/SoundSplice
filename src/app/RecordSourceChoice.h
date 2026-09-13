#pragma once

namespace looper::app
{
/**
    What pressing Record captures, decided from the armed track's type and
    what is actually connected.

    JUCE-free and pulled out of MainComponent for the usual reason (see
    TrackSelection, ClipLengthRepair, DragCommit) — but with a sharper one
    here: this decision shipped wrong. Routing on the armed track's type
    *alone* meant that with the default Instrument track selected, Record chose
    MIDI and captured nothing at all on a machine that had only a microphone.
    The button appeared to do nothing.

    The lesson that shape encodes: "what can this track hold?" and "what is
    there to record from?" are two independent questions, and answering only
    the first is what made a dead Record button. Both are inputs here, the
    whole table is enumerated, and every row has a test.
*/
enum class RecordSource
{
    None, // nothing connected to record from
    Audio,
    Midi
};

/** Why the choice came out the way it did — enough for the caller to say
    something useful, without this module holding user-facing strings. */
enum class RecordSourceReason
{
    /** The armed track gets what it can hold, and it is available. */
    Ok,

    /** A MIDI-capable track with no controller attached: recording MIDI could
        only produce an empty take, so this falls back to an audio take on a
        new track — which is what the app did before MIDI recording existed.
        Worth telling the user, since the take lands somewhere other than the
        track they armed. */
    FallbackToAudioNoMidi,

    /** An audio take is the only option and there is no input device. */
    NoAudioInput,

    /** As above, but the device manager reported a failure opening the input
        — on macOS, overwhelmingly a denied microphone permission, which is
        worth naming rather than making the user guess. */
    NoAudioInputPermission,

    /** A MIDI-capable track, no controller, and no audio input either. */
    NothingConnected
};

struct RecordSourceDecision
{
    RecordSource       source = RecordSource::None;
    RecordSourceReason reason = RecordSourceReason::Ok;

    bool operator==(const RecordSourceDecision&) const = default;
};

/**
    @param trackHoldsMidi  the armed track is anything but an Audio track —
                           Instrument, Drum and Guitar are all driven by MIDI
                           clips, so all three can hold a recorded pattern.
    @param haveMidi        at least one MIDI input device is connected.
    @param haveAudio       the open audio device actually has input channels.
    @param audioOpenFailed the device manager reported an error opening the
                           input; only used to sharpen the message when there
                           is no audio input.
*/
inline RecordSourceDecision chooseRecordSource(bool trackHoldsMidi, bool haveMidi,
                                               bool haveAudio, bool audioOpenFailed)
{
    if (! trackHoldsMidi)
    {
        // An Audio track can only hold audio: not a choice, just possible or
        // not. Notably a controller being connected is irrelevant here — the
        // armed track has nowhere to put a pattern.
        if (haveAudio)
            return { RecordSource::Audio, RecordSourceReason::Ok };

        return { RecordSource::None, audioOpenFailed ? RecordSourceReason::NoAudioInputPermission
                                                     : RecordSourceReason::NoAudioInput };
    }

    // A MIDI-capable track with something to play it from. This wins over
    // audio whenever a controller is present: it is the case the feature
    // exists for, and recording audio onto a synth track is impossible anyway.
    if (haveMidi)
        return { RecordSource::Midi, RecordSourceReason::Ok };

    if (haveAudio)
        return { RecordSource::Audio, RecordSourceReason::FallbackToAudioNoMidi };

    return { RecordSource::None, RecordSourceReason::NothingConnected };
}

} // namespace looper::app
