#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Node.h"
#include "engine/PianoNote.h"

namespace looper::engine
{
/**
    A piano: a pool of struck, damped, coupled unisons (see PianoNote).

    Built like GuitarNode — settings arrive as atomics and are read once per
    block, MIDI is rendered in spans so a note lands on the sample it was
    scheduled for — with three differences that come straight from the
    instrument.

    **Note-off damps.** A guitar string rings until it is replucked; a piano
    note stops when the key comes up, and that is the *normal* end of it.
    GuitarNode ignores note-offs on purpose; this one must not.

    **A voice is a key, not a string.** Each holds up to three strings of its
    own, so the pool is sized in notes and the real string count is up to three
    times larger. That is the cost model that matters here.

    **The pool is small, and stealing is expected.** With the sustain pedal
    down (step 4) a passage can leave every voice ringing, so running out is a
    normal condition rather than an error, and what gets taken has to be chosen
    rather than left to whichever slot happens to be first.
*/
class PianoNode final : public Node
{
public:
    /**
        How many keys can sound at once.

        Sized against measurement rather than taste — see the bounce tool's
        piano polyphony figure. Each voice is up to three waveguides, each
        running an allpass cascade for stiffness, so 48 keys is up to 144
        strings — a different order of cost from six guitar strings.

        The number came from the bounce tool's measurement rather than from
        taste: 24 voices of bass, where the strings are longest and the
        stiffness cascade deepest, rendered at 32x realtime, so the budget was
        never the constraint the plan feared it might be. It is set here at
        48 because that is what the sustain pedal needs — with the dampers up,
        a pedalled passage leaves everything ringing — and the measurement says
        it is affordable. A passage that still exceeds it steals, which is the
        same trade a real piano's dampers make with felt.
    */
    static constexpr int kMaxVoices = 48;

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        for (auto& voice : voices_)
        {
            voice.note.prepare(sampleRate);
            voice.midiNote = -1;
            voice.struckAt = 0;
        }

        applySettings();
    }

    // ---- message thread (atomics, read once per block) ----
    void setDecaySeconds(float seconds) { decaySeconds_.store(seconds, std::memory_order_relaxed); }
    void setBrightness(float value)     { brightness_.store(value, std::memory_order_relaxed); }
    void setHammerHardness(float value) { hammerHardness_.store(value, std::memory_order_relaxed); }
    void setStrikePosition(float value) { strikePosition_.store(value, std::memory_order_relaxed); }
    void setDetuneCents(float value)    { detuneCents_.store(value, std::memory_order_relaxed); }
    void setCoupling(float value)       { coupling_.store(value, std::memory_order_relaxed); }

    /** Scales the whole keyboard's inharmonicity. The *shape* across the range
        is pianoStiffness's and is not a preference; this is how much of it. */
    void setStiffness(float value)      { stiffness_.store(value, std::memory_order_relaxed); }

    /** How far the keyboard is spread across the stereo field, 0..1 — low
        notes left, high notes right, as a piano sounds from the player's seat
        and as most recordings of one are mic'd. Mono-compatible for the same
        reason the guitar's width is: these are distinct notes, not delayed
        copies of one signal. */
    void setWidth(float value)          { width_.store(value, std::memory_order_relaxed); }

    /** How long a damper takes to stop a string, in effect. 1 is a hard stop;
        real felt takes a moment, and a hard stop clicks. */
    void setDamperStrength(float value) { damperStrength_.store(value, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                 const ProcessContext& context) override
    {
        applySettings();

        // Stopping the transport damps everything, for the same reason it does
        // on a guitar: letting a note ring past its note-off is a musical
        // choice within playback, but ringing on after the user pressed stop
        // is not.
        if (wasPlaying_ && ! context.transport.playing)
            for (auto& voice : voices_)
            {
                voice.note.damp(1.0f);
                voice.midiNote  = -1;
                voice.heldByKey = false;
            }

        wasPlaying_ = context.transport.playing;

        const int numSamples = buffer.getNumSamples();
        int       position   = 0;

        for (const auto metadata : midi)
        {
            const int eventTime = juce::jlimit(0, numSamples, metadata.samplePosition);
            renderSpan(buffer, position, eventTime - position);
            position = eventTime;

            const auto message = metadata.getMessage();

            if (message.isNoteOn())
                strikeNote(message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isNoteOff())
                releaseNote(message.getNoteNumber());
            else if (message.isSustainPedalOn())
                setSustain(true);
            else if (message.isSustainPedalOff())
                setSustain(false);
        }

        renderSpan(buffer, position, numSamples - position);
    }

    /** How many keys are sounding. For meters and for the polyphony
        measurement; not used by the audio path. */
    int soundingVoices() const noexcept
    {
        int count = 0;
        for (const auto& voice : voices_)
            if (voice.note.isRinging())
                ++count;
        return count;
    }

private:
    struct Voice
    {
        PianoNote note;
        int       midiNote  = -1; // -1 when released; the note still rings on
        bool      heldByKey = false;
        uint64_t  struckAt  = 0;  // for stealing the oldest
        float     gainLeft  = 1.0f;
        float     gainRight = 1.0f;
    };

    void strikeNote(int midiNote, float velocity)
    {
        auto& voice = voiceFor(midiNote);

        voice.midiNote  = midiNote;
        voice.heldByKey = true;
        voice.struckAt  = ++strikeCounter_;

        // Everything that depends on which key this is, set at strike time
        // rather than per block: a voice's pitch, string count and stiffness
        // only change when it is reassigned to a different note.
        voice.note.setStringCount(pianoStringCount(midiNote));
        voice.note.setFrequency(440.0 * std::pow(2.0, (midiNote - 69) / 12.0));
        voice.note.setStiffness(pianoStiffness(midiNote) * stiffnessScale_);
        voice.note.setDetuneCents(detuneCentsValue_);
        voice.note.setCoupling(couplingValue_);
        voice.note.setDecaySeconds(decayForNote(midiNote));
        voice.note.setBrightness(brightnessValue_);
        voice.note.setHammerHardness(hammerHardnessValue_);
        voice.note.setStrikePosition(strikePositionValue_);

        // Low notes left, high notes right.
        const float position = std::clamp((float) (midiNote - 60) / 40.0f, -1.0f, 1.0f) * widthValue_;
        voice.gainLeft  = position <= 0.0f ? 1.0f : 1.0f - position;
        voice.gainRight = position >= 0.0f ? 1.0f : 1.0f + position;

        voice.note.strike(velocity);
    }

    /**
        Raises or lowers every damper at once.

        Down, the dampers come off: notes already released keep ringing, and
        everything struck afterwards rings until the pedal comes up. Up, every
        key that is no longer held is damped — including the ones released
        minutes ago, which is exactly what a pianist hears when they lift.
    */
    void setSustain(bool down)
    {
        sustain_ = down;

        if (down)
            return;

        for (auto& voice : voices_)
            if (! voice.heldByKey)
            {
                voice.note.damp(damperStrengthValue_);
                voice.midiNote = -1;
            }
    }

    void releaseNote(int midiNote)
    {
        // The top of a real piano has no dampers at all — the strings are so
        // short and quiet that felt would do nothing useful — so those notes
        // ring on past the key release. It is a small thing and it is audible:
        // the top octave of a piano does not stop when you let go.
        if (midiNote >= kLowestUndampedNote)
            return;

        for (auto& voice : voices_)
            if (voice.midiNote == midiNote)
            {
                voice.heldByKey = false;

                // With the pedal down the damper never reaches the string, so
                // the note rings on and the voice stays claimed until the
                // pedal lifts. That is the whole behaviour of a sustain pedal
                // and the reason it cannot be modelled as a longer decay.
                if (sustain_)
                    continue;

                voice.note.damp(damperStrengthValue_);
                voice.midiNote = -1; // released; it may still be decaying
            }
    }

    /** The voice this note should use: the one already playing it if there is
        one, then any silent one, and only then the oldest still ringing. */
    Voice& voiceFor(int midiNote)
    {
        for (auto& voice : voices_)
            if (voice.midiNote == midiNote)
                return voice; // a repeated key takes its own string back

        for (auto& voice : voices_)
            if (voice.midiNote < 0 && ! voice.note.isRinging())
                return voice;

        // Everything is busy. Take the one struck longest ago: it is the most
        // decayed and the least likely to be missed, and stealing the *newest*
        // would cut off the note someone just played.
        Voice* oldest = &voices_[0];
        for (auto& voice : voices_)
            if (voice.struckAt < oldest->struckAt)
                oldest = &voice;

        return *oldest;
    }

    /** How long a note rings before its damper falls. Bass strings carry far
        more energy and ring far longer than treble ones — a low A sustains for
        the best part of a minute, a top C for a second or two — and a single
        decay time across the keyboard is one of the more obvious ways a
        modelled piano gives itself away. */
    double decayForNote(int midiNote) const
    {
        const double normalised = std::clamp((midiNote - 21) / 87.0, 0.0, 1.0);
        return baseDecaySeconds_ * std::pow(0.12, normalised);
    }

    void renderSpan(juce::AudioBuffer<float>& buffer, int start, int count)
    {
        if (count <= 0)
            return;

        const int channels = buffer.getNumChannels();

        // Sample-at-a-time across all voices rather than voice-at-a-time,
        // because with the pedal down they are no longer independent: every
        // string feeds the soundboard and the soundboard drives every other
        // string, so they all have to advance together.
        for (int i = 0; i < count; ++i)
        {
            float left  = 0.0f;
            float right = 0.0f;
            float board = 0.0f;
            int   ringing = 0;

            for (auto& voice : voices_)
            {
                if (! voice.note.isRinging())
                    continue;

                const float value = voice.note.process() * kOutputScale;

                left  += value * voice.gainLeft;
                right += value * voice.gainRight;
                board += voice.note.bridgeMotion();
                ++ringing;
            }

            // The soundboard's *average* motion, not the sum.
            //
            // Summing was the first version and it ran away: this is positive
            // feedback from every string into every other one, so with ten
            // voices ringing the loop gain was ten times what a single note
            // implied, and a pedalled chord grew to a peak of 1327 before
            // collapsing. Averaging makes the loop gain independent of how
            // many keys are down, which is the only form of it that can be
            // reasoned about at all.
            if (ringing > 1)
                board /= (float) ringing;

            // Sympathetic resonance: with the dampers up, every string is free
            // to be driven by what the soundboard is doing, so a struck chord
            // makes the whole instrument answer. This is the single most
            // convincing thing a modelled piano can do, and it falls out of
            // the model rather than being faked with reverb — but only with
            // the pedal down, because a damped string cannot resonate.
            //
            // Fed from the *already-lowpassed* bridge motion of each note (see
            // PianoNote), for the same reason the unison's own coupling is:
            // broadband feedback around a lightly damped loop goes positive
            // wherever a delay lands half a period out, and this is a feedback
            // path across every voice at once.
            if (sustain_ && board != 0.0f)
            {
                const float drive = kSympatheticGain * board;
                for (auto& voice : voices_)
                    if (voice.note.isRinging())
                        voice.note.exciteSympathetically(drive);
            }

            if (channels >= 2)
            {
                buffer.addSample(0, start + i, left);
                buffer.addSample(1, start + i, right);
            }
            else if (channels == 1)
            {
                buffer.addSample(0, start + i, 0.5f * (left + right));
            }
        }
    }

    void applySettings()
    {
        baseDecaySeconds_     = (double) decaySeconds_.load(std::memory_order_relaxed);
        brightnessValue_      = brightness_.load(std::memory_order_relaxed);
        hammerHardnessValue_  = hammerHardness_.load(std::memory_order_relaxed);
        strikePositionValue_  = strikePosition_.load(std::memory_order_relaxed);
        detuneCentsValue_     = detuneCents_.load(std::memory_order_relaxed);
        couplingValue_        = coupling_.load(std::memory_order_relaxed);
        stiffnessScale_       = stiffness_.load(std::memory_order_relaxed);
        widthValue_           = std::clamp(width_.load(std::memory_order_relaxed), 0.0f, 1.0f);
        damperStrengthValue_  = damperStrength_.load(std::memory_order_relaxed);
    }

    /** Above this, a real piano has no dampers. */
    static constexpr int kLowestUndampedNote = 93; // ~A6

    /** Keeps a handful of keys inside range without a limiter downstream, the
        same job GuitarNode's 0.4 does for six strings. A five-note chord
        measured a peak of 2.9 before this existed — a piano is played in
        chords far more than a guitar is, so the headroom has to assume them. */
    static constexpr float kOutputScale = 0.3f;

    /** How hard the soundboard drives the undamped strings. Very small: this
        is positive feedback across up to 48 voices at once, and the point is a
        halo behind the note rather than a second instrument. */
    static constexpr float kSympatheticGain = 0.004f;

    std::array<Voice, kMaxVoices> voices_;
    uint64_t                      strikeCounter_ = 0;
    bool                          wasPlaying_    = false;
    bool                          sustain_       = false;

    std::atomic<float> decaySeconds_   { 20.0f }; // the bottom of the keyboard; scaled up the range
    std::atomic<float> brightness_     { 0.72f };
    std::atomic<float> hammerHardness_ { 0.45f };
    std::atomic<float> strikePosition_ { 0.125f }; // an eighth along — see PianoNote
    std::atomic<float> detuneCents_    { PianoNote::kDefaultDetuneCents };
    std::atomic<float> coupling_       { 0.7f };
    std::atomic<float> stiffness_      { 0.8f };
    std::atomic<float> width_          { 0.35f };
    std::atomic<float> damperStrength_ { 0.85f };

    double baseDecaySeconds_    = 20.0;
    float  brightnessValue_     = 0.72f;
    float  hammerHardnessValue_ = 0.45f;
    float  strikePositionValue_ = 0.125f;
    float  detuneCentsValue_    = PianoNote::kDefaultDetuneCents;
    float  couplingValue_       = 0.7f;
    float  stiffnessScale_      = 0.8f;
    float  widthValue_          = 0.35f;
    float  damperStrengthValue_ = 0.85f;
};

} // namespace looper::engine
