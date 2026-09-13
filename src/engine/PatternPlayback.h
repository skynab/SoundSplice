#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Pattern.h"
#include "engine/SequencerMath.h"

namespace looper::engine
{
/** The 128 note numbers a player currently has sounding, so they can be
    released when it stops or switches to different material. */
using ActiveNotes = std::array<bool, 128>;

/**
    Turning a looping Pattern into MIDI for one block.

    Shared by the arrangement's Sequencer and the session's SessionPlayer:
    both emit exactly the same notes from exactly the same Pattern, and only
    differ in *which* pattern is playing and *from when*. Keeping one copy
    means the next timing bug has one place to be fixed rather than two that
    quietly disagree.
*/
struct PatternPlayback
{
    /** Emits the note edges falling inside this block.

        @p localStartBeats is where the block begins relative to the pattern's
        own start — the sequencer measures it from a clip's position on the
        timeline, the session player from wherever the clip was launched. It's
        wrapped to the pattern length here, so either caller can hand over a
        raw distance.

        @p blockLengthBeats is how much musical time the block covers. In beats
        rather than samples because a beat's length is no longer a constant:
        the modulo that wraps a looping pattern has to happen in musical time,
        and the one conversion back to samples is edgeInBlockBeats. */
    static void emitBlock(juce::MidiBuffer& midi, const Pattern& pattern,
                          double localStartBeats, double blockLengthBeats,
                          int numSamples, ActiveNotes& activeNotes)
    {
        const double length = pattern.lengthBeats;
        if (length <= 0.0 || blockLengthBeats <= 0.0 || numSamples <= 0)
            return;

        const double blockStart = wrapPositive(localStartBeats, length);

        // The musical length of one sample in this block — what the old code's
        // "one sample" nudges below were, expressed in the units this now
        // works in.
        const double oneSample = blockLengthBeats / (double) numSamples;

        for (const auto& note : pattern.notes)
        {
            double onBeat  = note.startBeats;
            double offBeat = note.startBeats + note.lengthBeats;

            if (onBeat >= length)
                continue; // starts past the loop's end, so it never sounds
            if (offBeat >= length)
                offBeat = length - oneSample;
            if (offBeat <= onBeat)
                offBeat = onBeat + oneSample;

            const int noteNumber = juce::jlimit(0, 127, note.noteNumber);
            int offset = 0;

            if (edgeInBlockBeats(onBeat, blockStart, length, blockLengthBeats, numSamples, offset))
            {
                const auto velocity = (juce::uint8) juce::jlimit(1, 127, (int) (note.velocity * 127.0f));
                midi.addEvent(juce::MidiMessage::noteOn(channelFor(note.articulation), noteNumber, velocity),
                              offset);
                activeNotes[(size_t) noteNumber] = true;
            }

            if (edgeInBlockBeats(offBeat, blockStart, length, blockLengthBeats, numSamples, offset))
            {
                midi.addEvent(juce::MidiMessage::noteOff(1, noteNumber), offset);
                activeNotes[(size_t) noteNumber] = false;
            }
        }

        // Pedal movements, emitted as ordinary CC64 so anything that
        // understands MIDI understands them — including an external synth on a
        // hosted plugin, which would otherwise need a private channel.
        for (const auto& pedal : pattern.pedals)
        {
            if (pedal.beat < 0.0 || pedal.beat >= length)
                continue;

            int offset = 0;
            if (edgeInBlockBeats(pedal.beat, blockStart, length, blockLengthBeats, numSamples, offset))
                midi.addEvent(juce::MidiMessage::controllerEvent(1, kSustainPedalCc,
                                                                 pedal.down ? 127 : 0),
                              offset);
        }
    }

    /** The controller number a sustain pedal sends on: the MIDI standard's,
        so a real pedal plugged into the machine works without mapping. */
    static constexpr int kSustainPedalCc = 64;

    /** Which MIDI channel a note is sent on.

        The articulation rides the channel because nothing else uses it: every
        note-on here was hardcoded to channel 1, and no engine node reads
        getChannel() at all - so this carries the flag to GuitarNode with no
        side-channel and no change to how notes are scheduled. It also means an
        external sequencer or controller can drive the articulation.

        Safe for the other instruments because SynthVoice's and DrumKitNode's
        sounds both return true from appliesToChannel for every channel, so a
        note on 2 still sounds on a synth or drum track - it simply means
        nothing there. */
    static int channelFor(Articulation articulation) noexcept
    {
        return articulation == Articulation::PalmMute ? 2 : 1;
    }

    /** Releases everything currently sounding. Called when playback stops, or
        when a player switches material, so notes can't hang.

        Note-offs stay on channel 1 whatever the note-on used: GuitarNode
        matches them by note number and ignores the channel, and ActiveNotes is
        indexed by note number alone - so a second channel here would be state
        nothing reads. */
    static void flush(juce::MidiBuffer& midi, ActiveNotes& activeNotes)
    {
        for (int n = 0; n < 128; ++n)
        {
            if (activeNotes[(size_t) n])
            {
                midi.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                activeNotes[(size_t) n] = false;
            }
        }

        // And lift the pedal, unconditionally.
        //
        // Sent even when it was never pressed, because it costs one ignored
        // message and the alternative is tracking a second piece of state in
        // two callers purely to avoid it. A stuck pedal is a far worse failure
        // than a redundant message: every note played after it would ring
        // forever with no visible cause.
        midi.addEvent(juce::MidiMessage::controllerEvent(1, kSustainPedalCc, 0), 0);
    }
};

} // namespace looper::engine
