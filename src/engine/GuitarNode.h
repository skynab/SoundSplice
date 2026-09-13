#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "engine/GuitarString.h"
#include "engine/Pickup.h"
#include "engine/MidiNote.h"
#include "engine/Node.h"

namespace looper::engine
{
/** How many strings the instrument has. Six is the guitar; the code is written
    against this constant rather than a literal so a seven-string is a one-line
    change later. */
inline constexpr int kNumGuitarStrings = 6;

/** Which note-driven instrument a track's notes go to. Replaces the old
    isDrumTrack boolean, which stopped being able to express the choice as soon
    as there were three of them. */
enum class TrackInstrument
{
    Synth  = 0,
    Drum   = 1,
    Guitar = 2
};

/** Standard tuning, low to high (E2 A2 D3 G3 B3 E4), as MIDI note numbers.
    Stored as notes rather than frequencies so alternate tunings are expressed
    the way a player would say them ("drop D"). */
inline constexpr int kStandardTuning[kNumGuitarStrings] = { 40, 45, 50, 55, 59, 64 };

/** The highest fret any string can reach. Beyond this a note simply can't be
    played on that string, which is what stops the allocator putting a high
    lead line on the low E. */
inline constexpr int kMaxFret = 24;

/**
    Six strings played like a guitar rather than six voices played like a synth.

    The difference is entirely in the allocation: a guitar can sound at most one
    note per string, and a new note on a string *cuts the one already ringing
    there*. That single rule is the most audible thing separating this from a
    polyphonic synth with a plucked patch — more than any amount of DSP
    refinement — which is why it's the part the bounce checks pin down.

    Note-offs are deliberately ignored by default. A guitar string rings until
    it's replucked or damped; stopping every note when the MIDI says so would
    make held chords behave like an organ. `setMuteOnNoteOff` exists for
    callers that want the other behaviour, and is what a palm-mute articulation
    will drive later.
*/
class GuitarNode final : public Node
{
public:
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        for (auto& string : strings_)
            string.prepare(sampleRate);
        pickup_.prepare(sampleRate);
        pickupRight_.prepare(sampleRate);
        applySettings();
    }

    // ---- message thread (atomics, read once per block) ----
    void setDecaySeconds(float seconds) { decaySeconds_.store(seconds, std::memory_order_relaxed); }
    void setBrightness(float value)     { brightness_.store(value, std::memory_order_relaxed); }
    void setPickPosition(float value)   { pickPosition_.store(value, std::memory_order_relaxed); }
    void setPickHardness(float value)   { pickHardness_.store(value, std::memory_order_relaxed); }
    void setMuteOnNoteOff(float value)  { muteOnNoteOff_.store(value, std::memory_order_relaxed); }

    /** How much a note's velocity brightens it - see
        GuitarString::setVelocitySensitivity. */
    void setVelocitySensitivity(float value) { velocitySensitivity_.store(value, std::memory_order_relaxed); }

    /**
        How much energy crosses between strings at the bridge, 0..1.

        Scaled hard on the way to the strings: this is a feedback path, and the
        string-to-string-and-back gain goes as the square of the coefficient
        times the string count. kMaxCoupling keeps that a couple of orders of
        magnitude below unity even at 1.0, so the "no string may grow" property
        holds at every setting rather than up to some threshold nobody checks.
    */
    void setCoupling(float value) { couplingAmount_.store(value, std::memory_order_relaxed); }

    /** String stiffness — see GuitarString::setStiffness. Applied *scaled by
        string*: a wound low E is far stiffer than a plain high E, and giving
        all six the same value makes the top strings sound detuned rather than
        stiff. */
    void setStiffness(float value) { stiffness_.store(value, std::memory_order_relaxed); }

    /** How far the strings are spread across the stereo field, 0..1. Mono at
        0, and mono-compatible at any setting: the strings are distinct
        signals, not delayed copies, so folding down cannot comb-filter. */
    void setWidth(float value) { widthAmount_.store(value, std::memory_order_relaxed); }

    /** The pickup's resonant peak - see engine::Pickup. */
    void setPickupResonanceHz(float hz) { pickupResonanceHz_.store(hz, std::memory_order_relaxed); }
    void setPickupQ(float q)            { pickupQ_.store(q, std::memory_order_relaxed); }

    /** What a palm-muted note sounds like - see model::GuitarSettings. */
    void setPalmMuteDecaySeconds(float seconds) { palmMuteDecay_.store(seconds, std::memory_order_relaxed); }
    void setPalmMuteBrightness(float value)     { palmMuteBrightness_.store(value, std::memory_order_relaxed); }

    /** Open-string pitch of one string, as a MIDI note. Drop-D is
        setOpenNote(0, 38). */
    void setOpenNote(int stringIndex, int midiNote)
    {
        if (stringIndex >= 0 && stringIndex < kNumGuitarStrings)
            openNotes_[(size_t) stringIndex].store(midiNote, std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const ProcessContext& context) override
    {
        applySettings();

        // Stopping the transport damps every string.
        //
        // Note-offs cannot do this job: this node ignores them on purpose,
        // because a guitar string rings until it is replucked or damped, and
        // muteOnNoteOff is 0 in the tones where that matters most. But "let a
        // note ring past its note-off" is a musical choice *within* playback,
        // while "keep ringing after the user pressed stop" is not - so the two
        // are separate events and only one of them is an articulation.
        //
        // Damped rather than cut: mute() drops the loop gain so the strings
        // decay away over a few milliseconds, which is a hand landing on them.
        // Zeroing the buffers instead would put a click on the end of every
        // take.
        if (wasPlaying_ && ! context.transport.playing)
            for (int s = 0; s < kNumGuitarStrings; ++s)
            {
                strings_[(size_t) s].mute(1.0f);
                sounding_[(size_t) s].store(-1, std::memory_order_relaxed);
                palmMuted_[(size_t) s] = false;
            }

        wasPlaying_ = context.transport.playing;

        const int numSamples = buffer.getNumSamples();
        int       position   = 0;

        // Rendered in spans between MIDI events, so a pluck lands on the
        // sample it was scheduled for rather than at the top of the block.
        for (const auto metadata : midi)
        {
            const int eventTime = juce::jlimit(0, numSamples, metadata.samplePosition);
            renderSpan(buffer, position, eventTime - position);
            position = eventTime;

            const auto message = metadata.getMessage();
            if (message.isNoteOn())
                pluckNote(message.getNoteNumber(), message.getFloatVelocity(),
                          message.getChannel() == 2); // see PatternPlayback::channelFor
            else if (message.isNoteOff())
                releaseNote(message.getNoteNumber());
        }

        renderSpan(buffer, position, numSamples - position);
    }

    /** Which note each string is currently sounding, or -1. Atomic because the
        fretboard pane reads it from the UI thread every frame to show what's
        ringing — which is what makes the cut-on-retrigger behaviour visible
        rather than mysterious. */
    int noteOnString(int stringIndex) const
    {
        return (stringIndex >= 0 && stringIndex < kNumGuitarStrings)
                   ? sounding_[(size_t) stringIndex].load(std::memory_order_relaxed) : -1;
    }

private:
    void renderSpan(juce::AudioBuffer<float>& buffer, int start, int count)
    {
        if (count <= 0)
            return;

        const int   channels = buffer.getNumChannels();
        const float coupling = coupling_;
        const float width    = width_;

        for (int i = 0; i < count; ++i)
        {
            // Panned sums rather than one. Because a linear filter distributes
            // over a weighted sum, filtering these two with identical pickups
            // is *exactly* equivalent to filtering each string and then
            // panning - so width costs one extra filter instance and no
            // correctness.
            float left  = 0.0f;
            float right = 0.0f;
            float bridge = 0.0f;

            for (int s = 0; s < kNumGuitarStrings; ++s)
            {
                // With coupling on, a silent string still has to run: it can
                // only start ringing sympathetically if its loop is turning.
                // That is the entire effect, so the skip-silent-strings
                // optimisation is conditional rather than removed.
                if (coupling <= 0.0f && ! strings_[(size_t) s].isRinging())
                    continue;

                const float value = strings_[(size_t) s].process();

                bridge += value;
                left   += value * stringGainLeft_[(size_t) s];
                right  += value * stringGainRight_[(size_t) s];
            }

            // What the bridge passes back to every string. Applied after all
            // six have been read, so each sees the same bridge state for this
            // sample rather than a different one depending on its index.
            if (coupling > 0.0f)
            {
                const float injected = bridge * coupling;
                for (auto& string : strings_)
                    string.couple(injected);
            }

            // Six strings can sum well past unity, so scale to keep a full
            // strum inside range without needing a limiter downstream.
            left  *= 0.4f;
            right *= 0.4f;

            // One pickup senses the whole instrument, so this is on the sum
            // rather than per string - and it is the last thing in the
            // instrument, so whatever the track's chain does to the guitar it
            // is working on a signal that already has a pickup's shape.
            left  = pickup_.processSample(left);
            right = pickupRight_.processSample(right);

            if (channels <= 1 || width <= 0.0f)
            {
                // Mono output, or no width asked for: the two sums are equal
                // by construction at width 0, so either one is the answer.
                const float mono = width <= 0.0f ? left : 0.5f * (left + right);
                for (int ch = 0; ch < channels; ++ch)
                    buffer.addSample(ch, start + i, mono);
            }
            else
            {
                buffer.addSample(0, start + i, left);
                buffer.addSample(1, start + i, right);

                for (int ch = 2; ch < channels; ++ch)
                    buffer.addSample(ch, start + i, 0.5f * (left + right));
            }
        }
    }

    /**
        Chooses a string and sounds the note on it.

        Prefers a string that isn't already holding a note, and among those the
        one needing the lowest fret — roughly what a player reaching for the
        note would do.

        When every reachable string is *already held*, the note becomes a
        **hammer-on or pull-off**: the string is re-fretted without being struck
        again, so it keeps the energy it has and simply rings at the new pitch.
        That is what a guitarist does when their hand is already on the string,
        and it's why those notes are softer than picked ones — the softness
        falls out of the model rather than being simulated.

        Only when no held string can reach the note is one taken from the
        oldest. That isn't voice stealing to save CPU; it's the hand having to
        leave one note to play another.
    */
    void pluckNote(int midiNote, float velocity, bool palmMuted)
    {
        int best       = -1;
        int bestFret   = kMaxFret + 1;
        int hammerOn   = -1;
        int hammerMove = kMaxFret + 1;
        int oldest     = -1;
        int oldestAge  = -1;

        for (int s = 0; s < kNumGuitarStrings; ++s)
        {
            const int open = openNotes_[(size_t) s].load(std::memory_order_relaxed);
            const int fret = midiNote - open;
            if (fret < 0 || fret > kMaxFret)
                continue; // this string can't reach the note at all

            if (sounding_[(size_t) s].load(std::memory_order_relaxed) < 0)
            {
                if (fret < bestFret)
                {
                    bestFret = fret;
                    best     = s;
                }
            }
            else
            {
                // Held. The nearest hand movement wins the hammer-on: a player
                // re-frets whichever string is already closest to the new note.
                const int move = std::abs(midiNote - sounding_[(size_t) s].load(std::memory_order_relaxed));
                if (move < hammerMove)
                {
                    hammerMove = move;
                    hammerOn   = s;
                }

                const int age = pluckCounter_ - pluckedAt_[(size_t) s];
                if (age > oldestAge)
                {
                    oldestAge = age;
                    oldest    = s;
                }
            }
        }

        if (best >= 0)
        {
            palmMuted_[(size_t) best] = palmMuted;

            auto& string = strings_[(size_t) best];
            // Applied here, not only from applySettings at the top of the
            // block: the block's settings were pushed before this note-on was
            // read, so a note plucked mid-block would otherwise chug from the
            // *next* block - about 10ms late, on the one articulation whose
            // whole character is its attack.
            applyStringSettings(best);

            string.mute(0.0f); // a fresh pluck lifts any damping the last note left
            string.setFrequency(midiNoteToHertz(midiNote));
            string.pluck(velocity);

            sounding_[(size_t) best].store(midiNote, std::memory_order_relaxed);
            pluckedAt_[(size_t) best] = ++pluckCounter_;
            return;
        }

        if (hammerOn >= 0)
        {
            // Hand already on the string: re-fret without striking it again.
            // The picking hand decides palm muting, and it hasn't moved, so a
            // hammer-on inherits whatever the string was already doing.
            auto& string = strings_[(size_t) hammerOn];
            string.setFrequency(midiNoteToHertz(midiNote));

            sounding_[(size_t) hammerOn].store(midiNote, std::memory_order_relaxed);
            pluckedAt_[(size_t) hammerOn] = ++pluckCounter_;
            return;
        }

        if (oldest < 0)
            return; // no string can reach this note at all; better silent than wrong

        palmMuted_[(size_t) oldest] = palmMuted;

        auto& string = strings_[(size_t) oldest];
        applyStringSettings(oldest);
        string.mute(0.0f);
        string.setFrequency(midiNoteToHertz(midiNote));
        string.pluck(velocity);

        sounding_[(size_t) oldest].store(midiNote, std::memory_order_relaxed);
        pluckedAt_[(size_t) oldest] = ++pluckCounter_;
    }

    /** A note-off frees the string for reuse, and damps it only if the caller
        asked for that — see the class comment on why ringing on is the
        default. */
    void releaseNote(int midiNote)
    {
        const float damping = muteOnNoteOff_.load(std::memory_order_relaxed);

        for (int s = 0; s < kNumGuitarStrings; ++s)
        {
            if (sounding_[(size_t) s].load(std::memory_order_relaxed) != midiNote)
                continue;

            if (damping > 0.0f)
                strings_[(size_t) s].mute(damping);

            // Freed either way: the string is no longer holding that note, so
            // the allocator may reach for it before it has finished ringing.
            sounding_[(size_t) s].store(-1, std::memory_order_relaxed);
            return;
        }
    }

    void applySettings()
    {
        pickup_.setResonanceHz(pickupResonanceHz_.load(std::memory_order_relaxed));
        pickup_.setQ(pickupQ_.load(std::memory_order_relaxed));
        pickupRight_.setResonanceHz(pickupResonanceHz_.load(std::memory_order_relaxed));
        pickupRight_.setQ(pickupQ_.load(std::memory_order_relaxed));

        const float velocitySensitivity = velocitySensitivity_.load(std::memory_order_relaxed);
        const float stiffness           = stiffness_.load(std::memory_order_relaxed);

        for (int i = 0; i < kNumGuitarStrings; ++i)
        {
            strings_[(size_t) i].setVelocitySensitivity(velocitySensitivity);

            // Thickest string gets the full amount, thinnest a third of it.
            const float thickness = 1.0f - 0.67f * (float) i / (float) (kNumGuitarStrings - 1);
            strings_[(size_t) i].setStiffness(stiffness * thickness);
        }

        // Resolved once per block into plain audio-thread floats: the render
        // loop reads them per sample, and re-loading an atomic six times a
        // sample buys nothing when the value cannot change mid-block anyway.
        coupling_ = kMaxCoupling * std::clamp(couplingAmount_.load(std::memory_order_relaxed), 0.0f, 1.0f);
        width_    = std::clamp(widthAmount_.load(std::memory_order_relaxed), 0.0f, 1.0f);

        updateStringPans();

        // Per string rather than uniform, because a palm-muted string wants a
        // different decay and brightness from an open one and this is the
        // once-per-block place those atomics reach the strings.
        for (int s = 0; s < kNumGuitarStrings; ++s)
            applyStringSettings(s);
    }

    /** Pushes the current settings to one string, choosing the palm-muted
        values if that string is currently holding a muted note. */
    void applyStringSettings(int stringIndex)
    {
        if (stringIndex < 0 || stringIndex >= kNumGuitarStrings)
            return;

        const bool muted = palmMuted_[(size_t) stringIndex];

        const float palmBright = palmMuteBrightness_.load(std::memory_order_relaxed);

        const float decay  = muted ? palmMuteDecay_.load(std::memory_order_relaxed)
                                   : decaySeconds_.load(std::memory_order_relaxed);
        const float bright = muted ? palmBright
                                   : brightness_.load(std::memory_order_relaxed);

        // The excitation is damped too, not just the loop - and this is the
        // part that actually makes a chug dark.
        //
        // Measured: setting only the loop's brightness made a muted note come
        // out *brighter* than an open one. The loop filter darkens the tone a
        // little on each round trip, so a short decay means fewer trips and
        // less darkening; the note dies before it can get dark. That is a real
        // property of the string model, not a bug in it.
        //
        // A palm mute is dark because the hand is resting on the string when it
        // is struck, so the strike itself is muffled. pickHardness is exactly
        // that control ("a soft pluck excites fewer partials"), so scaling it
        // by the palm-mute brightness damps the burst the same way the hand
        // does.
        const float hardness = pickHardness_.load(std::memory_order_relaxed);

        auto& string = strings_[(size_t) stringIndex];
        string.setDecaySeconds(decay);
        string.setBrightness(bright);
        string.setPickPosition(pickPosition_.load(std::memory_order_relaxed));
        string.setPickHardness(muted ? hardness * palmBright : hardness);
    }

    std::array<GuitarString, kNumGuitarStrings> strings_;
    Pickup                                     pickup_;

    /** The right channel's pickup. Identical settings to pickup_ — see the
        render loop for why two instances are exactly equivalent to one. */
    Pickup                                     pickupRight_;

    /** The largest fraction of the bridge signal a single string ever receives.
        Deliberately small: six strings each feeding back through the bridge is
        a loop, and this is what keeps its gain far below one. */
    static constexpr float kMaxCoupling = 0.03f;

    std::array<float, kNumGuitarStrings> stringGainLeft_  { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, kNumGuitarStrings> stringGainRight_ { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    /** Spreads the strings across the field, low to high.

        A linear pan law with a unity centre, the same one tracks and drum pads
        use — so at width 0 both gains are exactly 1 and the two sums are
        bit-identical, which is what lets the render loop treat width 0 as
        plain mono rather than as a special case that merely sounds like it. */
    void updateStringPans() noexcept
    {
        for (int i = 0; i < kNumGuitarStrings; ++i)
        {
            // -1 for the lowest string, +1 for the highest.
            const float position = kNumGuitarStrings > 1
                ? (2.0f * (float) i / (float) (kNumGuitarStrings - 1) - 1.0f)
                : 0.0f;

            const float pan = position * width_;

            stringGainLeft_[(size_t) i]  = pan <= 0.0f ? 1.0f : 1.0f - pan;
            stringGainRight_[(size_t) i] = pan >= 0.0f ? 1.0f : 1.0f + pan;
        }
    }

    // Audio-thread state: which note each string holds, and when it was struck.
    std::array<std::atomic<int>, kNumGuitarStrings> sounding_ { -1, -1, -1, -1, -1, -1 };
    std::array<int, kNumGuitarStrings> pluckedAt_ {};

    // Whether each string is currently holding a palm-muted note. Audio thread
    // only: written when a note is plucked, read when settings are applied,
    // both of which happen inside process().
    std::array<bool, kNumGuitarStrings> palmMuted_ {};

    // Audio thread only: used to notice the transport stopping — see process().
    bool wasPlaying_ = false;
    int                                pluckCounter_ = 0;

    std::array<std::atomic<int>, kNumGuitarStrings> openNotes_ {
        kStandardTuning[0], kStandardTuning[1], kStandardTuning[2],
        kStandardTuning[3], kStandardTuning[4], kStandardTuning[5]
    };

    std::atomic<float> decaySeconds_  { 3.0f };
    std::atomic<float> brightness_    { 0.7f };
    std::atomic<float> pickPosition_  { 0.22f };
    std::atomic<float> pickHardness_  { 0.6f };
    std::atomic<float> muteOnNoteOff_ { 0.0f };

    std::atomic<float> pickupResonanceHz_ { 3000.0f };
    std::atomic<float> pickupQ_           { 1.4f };
    std::atomic<float> velocitySensitivity_ { 0.0f };
    std::atomic<float> couplingAmount_       { 0.0f };
    std::atomic<float> stiffness_            { 0.0f };
    std::atomic<float> widthAmount_          { 0.0f };

    // Resolved from the atomics above once per block; audio thread only.
    float coupling_ = 0.0f;
    float width_    = 0.0f;

    std::atomic<float> palmMuteDecay_      { 0.18f };
    std::atomic<float> palmMuteBrightness_ { 0.25f };
};

} // namespace looper::engine
