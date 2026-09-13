#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/GuitarString.h"

namespace looper::engine
{
/**
    One piano key: the two or three slightly detuned strings a hammer strikes
    together, joined at a shared bridge.

    This is the part that makes a piano sound like a piano rather than like a
    struck string, and the reason is the **double decay**. The strings of a
    unison start in phase, and in phase they push the bridge hard — which is a
    lossy termination, so that motion dies quickly. Their slight detuning then
    pulls them out of phase, and out of phase their forces cancel *at the
    bridge*: it barely moves, almost nothing is lost through it, and what is
    left rings on far longer. The result is a fast initial decay followed by a
    long, quiet aftersound, and it is audible on every piano note ever
    recorded. A single decaying exponential does not sound like a piano, and no
    amount of EQ makes it one.

    So the coupling here **loads** the strings rather than feeding them: each
    receives a fraction of the summed bridge motion with the sign reversed. In
    phase that is a strong damping term; out of phase the sum is near zero and
    there is nothing to damp. That is the whole mechanism, and it falls out of
    the same bridge coupling §33 built for the guitar — pointed at strings of
    the *same* nominal pitch instead of different ones.

    JUCE-free, like the string it is built from, so the decay envelope is a
    measured claim rather than something only checkable by ear.
*/

/** The lowest MIDI note that gets two strings, and the lowest that gets three.

    A real piano is single-strung through the low bass (thick wound wire, where
    one string already moves plenty of air), doubles through the upper bass,
    and is triple-strung from the tenor up. */
inline constexpr int kPianoTwoStringNote   = 32; // ~G#1
inline constexpr int kPianoThreeStringNote = 44; // ~G#2

/** How many strings @p midiNote is strung with, 1..3. */
inline int pianoStringCount(int midiNote)
{
    if (midiNote < kPianoTwoStringNote)
        return 1;
    if (midiNote < kPianoThreeStringNote)
        return 2;
    return 3;
}

/**
    How stiff @p midiNote's string is, 0..1, for GuitarString::setStiffness.

    Inharmonicity on a real piano is **U-shaped** across the keyboard, not
    constant and not simply rising: it is high in the bass (short, very thick
    wire, where the stiffness of the core dominates), falls to a minimum
    through the middle where the scaling is most ideal, and climbs steeply
    again in the top octaves where the strings are very short. Both ends of
    that curve are audible — the bass's stretched partials are a large part of
    a piano's weight, and the treble's are why the top of the instrument is
    tuned sharp to match.
*/
inline float pianoStiffness(int midiNote)
{
    // Where the scaling is most ideal, and inharmonicity is least: around
    // middle C, which is also where the ear is most sensitive to it.
    constexpr float kMinimumAt = 60.0f;

    const float distance = std::abs((float) midiNote - kMinimumAt) / 48.0f;

    // Squared, so the curve is flat through the middle and rises sharply at
    // the extremes rather than sloping evenly — which is what the measured
    // curve looks like.
    return std::clamp(0.12f + 0.88f * distance * distance, 0.0f, 1.0f);
}

class PianoNote
{
public:
    static constexpr int kMaxStrings = 3;

    /** How far the strings of a unison are detuned from each other, in cents.

        Real piano unisons are tuned to within a couple of cents and never
        exactly together — a perfectly tuned unison would have no beating and,
        far more importantly, no way to fall out of phase, which is what the
        long aftersound depends on. Tuners set this by ear and call it
        "stretch" or simply leave the unison "alive". */
    static constexpr float kDefaultDetuneCents = 1.2f;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        for (auto& string : strings_)
            string.prepare(sampleRate_);

        // ~320Hz. Low enough that the bridge is genuinely mass-controlled
        // above it, which is both the physics and what keeps a short, lightly
        // damped treble loop stable.
        const auto x = (float) (2.0 * M_PI * 320.0 / sampleRate_);
        bridgeCoeff_ = std::clamp(x / (1.0f + x), 0.0f, 1.0f);

        applyFrequencies();
    }

    void reset()
    {
        for (auto& string : strings_)
            string.reset();
        bridgeState_ = 0.0f;
    }

    // ---- setup ----
    void setStringCount(int count)
    {
        stringCount_ = std::clamp(count, 1, kMaxStrings);
        applyFrequencies();
    }

    int stringCount() const noexcept { return stringCount_; }

    void setFrequency(double hz)
    {
        frequency_ = hz > 0.0 ? hz : 440.0;
        applyFrequencies();
    }

    void setDetuneCents(float cents)
    {
        detuneCents_ = std::clamp(cents, 0.0f, 25.0f);
        applyFrequencies();
    }

    /**
        How heavily the bridge loads the strings, 0..1.

        Scaled hard on the way through: this is a feedback path around three
        strings, and it has to stay far enough below unity that the existing
        "no string may grow" guarantee holds at every setting rather than up to
        some threshold nobody checks.
    */
    void setCoupling(float amount)
    {
        coupling_ = kMaxCoupling * std::clamp(amount, 0.0f, 1.0f);
    }

    void setDecaySeconds(double seconds)
    {
        for (auto& string : strings_)
            string.setDecaySeconds(seconds);
    }

    void setBrightness(float brightness)
    {
        for (auto& string : strings_)
            string.setBrightness(brightness);
    }

    void setStiffness(float stiffness)
    {
        for (auto& string : strings_)
            string.setStiffness(stiffness);
    }

    void setHammerHardness(float hardness)
    {
        for (auto& string : strings_)
            string.setHammerHardness(hardness);
    }

    /** Where along the string the hammer lands — a seventh to a ninth on a
        real instrument, which puts the excitation comb's notch on the seventh
        partial. See GuitarString::strike. */
    void setStrikePosition(float position)
    {
        for (auto& string : strings_)
            string.setPickPosition(position);
    }

    // ---- playing ----
    /** Strikes every string of the unison together, as one hammer does. */
    void strike(float velocity)
    {
        for (int i = 0; i < stringCount_; ++i)
            strings_[(size_t) i].strike(velocity);

        damping_ = 0.0f;
    }

    /**
        Lowers the damper onto the strings — what a key release does.

        1 stops the note, 0 lets it ring. Unlike a guitar, this is the *normal*
        end of a piano note: the felt comes down and the string stops within a
        few tens of milliseconds.
    */
    void damp(float amount)
    {
        damping_ = std::clamp(amount, 0.0f, 1.0f);
        for (int i = 0; i < stringCount_; ++i)
            strings_[(size_t) i].mute(damping_);
    }

    /**
        Energy arriving from the *other keys*, through the soundboard.

        Distinct from the unison's own bridge coupling above, which loads these
        strings against each other. This is the path that makes an undamped
        piano resonate: strike a chord with the pedal down and the whole
        instrument answers, because every other string is free to move.
    */
    void exciteSympathetically(float signal) noexcept
    {
        if (signal == 0.0f)
            return;

        for (int i = 0; i < stringCount_; ++i)
            strings_[(size_t) i].couple(signal);
    }

    /** This block's bridge motion — what the soundboard is driven by. Valid
        after process(). */
    float bridgeMotion() const noexcept { return bridgeState_; }

    bool isRinging() const noexcept
    {
        for (int i = 0; i < stringCount_; ++i)
            if (strings_[(size_t) i].isRinging())
                return true;
        return false;
    }

    /** One sample: every string of the unison, summed, with the bridge load
        applied afterwards so each string sees the same bridge state. */
    float process() noexcept
    {
        float bridge = 0.0f;
        for (int i = 0; i < stringCount_; ++i)
            bridge += strings_[(size_t) i].process();

        if (coupling_ > 0.0f && stringCount_ > 1)
        {
            // The bridge is mass-controlled: it is heavy, it moves with the
            // low modes and barely responds to the high ones. Lowpassing what
            // it passes back is therefore the physical description, not a
            // safety measure — but it is also what makes this stable.
            //
            // Without it, the load is broadband feedback around the string's
            // own loop, and a delay turns negative feedback positive at every
            // frequency where it lands half a period out. Low down that never
            // mattered, because the loop's own damping swamped it. Once the
            // damping cap (see GuitarString::updateDamping) left the top
            // octave's loop nearly lossless, a treble note fed back louder
            // each pass and ran away to NaN within a second.
            bridgeState_ += bridgeCoeff_ * (bridge - bridgeState_);

            // Negative: the bridge is a *load*, not a source. Feeding the sum
            // back in phase would reinforce the very motion that is supposed
            // to be dying quickly, and the note would have no double decay at
            // all — it would have an anti-decay.
            const float load = -coupling_ * bridgeState_;
            for (int i = 0; i < stringCount_; ++i)
                strings_[(size_t) i].couple(load);
        }

        // Averaged rather than summed, so a triple-strung note is not three
        // times louder than a single-strung one for a reason that has nothing
        // to do with how the instrument is voiced.
        return bridge / (float) stringCount_;
    }

private:
    /** The most of the bridge sum any one string is loaded by. Small, because
        three strings feeding back through a shared bridge is a loop. */
    static constexpr float kMaxCoupling = 0.02f;

    void applyFrequencies()
    {
        for (int i = 0; i < stringCount_; ++i)
        {
            // Spread symmetrically about the nominal pitch: -1, 0, +1 for a
            // trichord, so the note itself stays in tune however many strings
            // it has and adding one does not shift the key sharp.
            const float offset = stringCount_ > 1
                ? ((float) i - 0.5f * (float) (stringCount_ - 1))
                : 0.0f;

            const double cents = (double) offset * (double) detuneCents_;
            strings_[(size_t) i].setFrequency(frequency_ * std::pow(2.0, cents / 1200.0));
        }
    }

    std::array<GuitarString, kMaxStrings> strings_;

    double sampleRate_   = 48000.0;
    float  bridgeCoeff_  = 0.04f;
    float  bridgeState_  = 0.0f;
    double frequency_    = 440.0;
    int    stringCount_  = kMaxStrings;
    float  detuneCents_  = kDefaultDetuneCents;
    float  coupling_     = kMaxCoupling * 0.6f;
    float  damping_      = 0.0f;
};

} // namespace looper::engine
