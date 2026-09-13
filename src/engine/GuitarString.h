#pragma once

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

namespace looper::engine
{
/**
    One plucked string, as a digital waveguide (extended Karplus-Strong).

    A delay line whose length sets the pitch, fed back through a damping filter,
    excited by a shaped noise burst. Cheap — a handful of multiplies per sample —
    and it models the things that actually make a string sound like a string:
    the pitch-dependent decay, the brightness that fades as the note rings, and
    the comb colouring of where along the string it was plucked.

    JUCE-free so the tuning and decay can be measured headlessly, which is the
    whole point of doing this layer first: a string that is a few cents sharp
    up the neck is the kind of wrong that is silent in code review and obvious
    the moment anyone plays it.

    **Tuning is why this doesn't reuse DelayLine.** That one takes an integer
    delay, and rounding the loop length quantises pitch badly as notes rise —
    at 48kHz the 24th fret of the high E lands about 19 cents sharp. The loop
    here is fractional, read through an allpass interpolator, which delays
    without adding damping of its own (linear interpolation would lowpass the
    loop and muddle decay with tuning).

    Not thread-safe and not meant to be: one string belongs to one voice on the
    audio thread. Everything is pre-allocated in prepare().
*/
class GuitarString
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // Sized for the lowest note anything is likely to ask for, so
        // setFrequency never has to allocate. 25Hz rather than 30: a piano's
        // bottom A is 27.5Hz, and a string that cannot reach its own lowest
        // note is not a limit anyone would think to look for.
        buffer_.assign((size_t) std::ceil(sampleRate_ / 25.0) + 4, 0.0f);

        // Sized here, once, for the longest loop this string could ever hold.
        // pluck() runs on the audio thread and must not allocate, so it only
        // ever *indexes* into this — never resizes it.
        excitation_.assign(buffer_.size(), 0.0f);
        reset();
        setFrequency(frequency_);
    }

    void reset() noexcept
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_     = 0;
        lastFilterIn_    = 0.0f;
        allpassLastIn_   = 0.0f;
        allpassLastOut_  = 0.0f;
        energy_          = 0.0f;
        pendingCoupling_ = 0.0f;
        dispersionX_.fill(0.0f);
        dispersionY_.fill(0.0f);
    }

    /** Sets the pitch. Changing this *without* plucking is exactly a hammer-on
        or pull-off: the string keeps whatever energy it has and simply rings
        at the new length, which is what makes those articulations quieter and
        smoother than a struck note. See GuitarNode::pluckNote. */
    void setFrequency(double hz) noexcept
    {
        frequency_ = std::clamp(hz, 20.0, sampleRate_ * 0.25);
        updateDamping(); // the cap below depends on the pitch
    }

    /** How long the note takes to fall 60dB, in seconds, measured at the
        fundamental. Specified in *time* rather than as a filter coefficient so
        it holds across the range: a fixed coefficient makes high notes die far
        too fast, because they go round the loop more often per second. */
    void setDecaySeconds(double seconds) noexcept
    {
        decaySeconds_ = std::clamp(seconds, 0.05, 30.0);
        updateLoopGain();
    }

    /** 0 = dull, 1 = bright. Sets how much the loop filter rolls off each pass,
        which is what makes the tail darken as it decays. */
    void setBrightness(float brightness) noexcept
    {
        // Mapped away from both extremes: at 0.5 the filter is a two-point
        // average (maximum damping of the top), at 0 it's a pure delay and the
        // string never darkens at all.
        requestedDamping_ = 0.5f - 0.45f * std::clamp(brightness, 0.0f, 1.0f);
        updateDamping();
    }

    /** Where along the string it's plucked: 0 = at the bridge (thin, nasal),
        0.5 = the middle (round and full). A comb notch, which is most of the
        difference between a bridge pickup and a soundhole. */
    void setPickPosition(float position) noexcept
    {
        pickPosition_ = std::clamp(position, 0.02f, 0.5f);
    }

    /** 0 = soft/fingertip, 1 = hard/plectrum. Shapes the excitation burst. */
    void setPickHardness(float hardness) noexcept
    {
        pickHardness_ = std::clamp(hardness, 0.0f, 1.0f);
    }

    /**
        How much a note's velocity brightens it, on top of its loudness.

        Until this existed, velocity scaled amplitude and nothing else: every
        note had the same spectrum, which is most of why a programmed part
        sounds machine-gunned. On a real instrument picking harder excites more
        partials — it is a different attack, not a louder one.

        Applied as a *deviation from kReferenceVelocity*, so a note at that
        velocity excites exactly as it did before this parameter existed. That
        keeps this an addition rather than a retune of every part already
        written; 0 restores the old behaviour completely.
    */
    void setVelocitySensitivity(float amount) noexcept
    {
        velocitySensitivity_ = std::clamp(amount, 0.0f, 1.0f);
    }

    /** The velocity at which velocity-to-timbre does nothing — engine::Note's
        default, and what every generated pattern uses. */
    static constexpr float kReferenceVelocity = 0.8f;

    /**
        String stiffness, 0..1 — how far the partials stretch sharp.

        An ideal string is perfectly harmonic: partial *n* sits at exactly *n*
        times the fundamental, which is what a plain waveguide produces and
        part of why one sounds synthetic. A real string resists bending, and
        that stiffness makes it *dispersive* — high frequencies travel along it
        faster than low ones, so the upper partials arrive early and end up
        progressively sharp. It is most audible on thick, low strings, and it
        is a real component of what a drop-tuned guitar's "growl" is: the
        partials of a stiff low string beat against each other instead of
        locking into a clean harmonic stack.

        Modelled the standard way, as a cascade of first-order allpasses in the
        loop: an allpass passes every frequency at full level but delays them
        by different amounts, which is exactly what dispersion is. The
        coefficient is negative so the delay *falls* with frequency — a
        positive one would flatten the partials instead, which is the easiest
        sign error to make here and sounds like a detuned string rather than a
        stiff one.

        0 is the ideal string, i.e. the behaviour that predates this.
    */
    void setStiffness(float stiffness) noexcept
    {
        stiffness_       = std::clamp(stiffness, 0.0f, 1.0f);
        dispersionCoeff_ = -kMaxDispersion * stiffness_;
        updateLoopLength(); // the allpasses' delay is part of the loop
    }

    /** Excites the string. Replaces whatever was ringing, which is what a
        second pluck on the same string does in life. */
    void pluck(float velocity) noexcept
    {
        const int size = (int) buffer_.size();
        if (size < 4)
            return;

        // The excitation has to land where the loop will actually read it. The
        // buffer is sized for the lowest note this could ever play, so it's far
        // longer than the current loop — filling from index 0 would put the
        // burst outside the span the read pointer visits and the string would
        // sound silence for its first pass.
        const int length = std::clamp(integerDelay_, 2, size - 2);

        const int combOffset = std::max(1, (int) std::lround(pickPosition_ * (float) length));

        // Harder picking is brighter as well as louder — the deviation is from
        // kReferenceVelocity, so a note at that velocity is excited exactly as
        // it was before velocity affected timbre at all.
        //
        // The small random term stops every pluck being identical, which is
        // audible as a mechanical sameness even when nothing else repeats.
        //
        // It varies the *pick's hardness*, not its position, and that choice
        // was forced by measurement. Jittering the position moves the comb,
        // and the comb's notches fall on real harmonics: it is what nulls the
        // even ones when you pluck at the midpoint, and what gives a
        // palm-muted note its darkness. Two existing checks — the
        // pick-position test and the bounce tool's palm-mute brightness —
        // both moved when the position was jittered, because a shifted comb
        // is a tonal change rather than a variation. Hardness only shapes how
        // bright the burst is, which is exactly the "no two plucks alike"
        // quality wanted, and leaves every comb property intact.
        //
        // Drawn from its own generator, not nextNoise(): taking one sample
        // from that stream would shift every sample of the burst that follows,
        // changing the entire realisation of the note rather than nudging it.
        const float dynamicHardness = std::clamp(
            pickHardness_
                + velocitySensitivity_ * (velocity - kReferenceVelocity)
                + 0.03f * nextJitter(),
            0.0f, 1.0f);

        // A soft pluck excites fewer partials: lowpass the noise more.
        const float smoothing = 0.85f - 0.75f * dynamicHardness;

        // Built into scratch first so the comb can read earlier samples of the
        // *excitation*, not of whatever the loop happened to contain. The
        // scratch is pre-sized in prepare(); resizing it here would allocate
        // on the audio thread.
        float smoothed = 0.0f;
        for (int i = 0; i < length; ++i)
        {
            const float white = nextNoise();
            smoothed = smoothing * smoothed + (1.0f - smoothing) * white;
            excitation_[(size_t) i] = smoothed;
        }

        commitExcitation(length, combOffset, velocity);
    }

    /**
        Strikes the string with a hammer — how a piano note starts.

        A pluck sets the string's *displacement* and lets go; a hammer is a
        felt mass in **contact** with it for a brief moment, and that
        difference is most of why the two instruments do not sound alike.

        The expressive part is that the contact time is not fixed: a harder
        blow compresses the felt more, the hammer leaves sooner, and the string
        is left with far more high-frequency energy. So a loud piano note is
        not a quiet one turned up — it is a *brighter* note, and steeply so.
        That is the instrument's entire dynamic range, and it is intrinsic
        here rather than an optional mapping the way velocity-to-timbre is for
        a pluck (see setVelocitySensitivity): a hammer that ignored velocity
        would not be a hammer.

        The strike position combs the excitation exactly as a pluck position
        does, and for the same reason — the wave leaves in both directions and
        the near reflection returns inverted. That comb is also why pianos are
        struck between a seventh and a ninth of the way along: it puts a notch
        on the seventh partial, which is the one that would clash. See
        setPickPosition, which is that same geometry.
    */
    void strike(float velocity) noexcept
    {
        const int size = (int) buffer_.size();
        if (size < 4)
            return;

        const int length     = std::clamp(integerDelay_, 2, size - 2);
        const int combOffset = std::max(1, (int) std::lround(pickPosition_ * (float) length));

        const float blow = std::clamp(velocity, 0.0f, 1.0f);

        // Contact time, in samples. Harder felt and harder playing both
        // shorten it; the range is the few milliseconds a real hammer spends
        // on the string, longest for a soft blow on soft felt.
        const float contactMs = kMaxContactMs
                              - (kMaxContactMs - kMinContactMs)
                                * std::clamp(0.5f * blow + 0.5f * hammerHardness_, 0.0f, 1.0f);

        const int pulse = std::clamp((int) std::lround((double) contactMs * 0.001 * sampleRate_),
                                     2, length);

        // A raised cosine: the force rises and falls smoothly, because felt
        // compresses rather than striking like a hammer on an anvil. A square
        // pulse would put a step in the string and sound like a click.
        for (int i = 0; i < length; ++i)
        {
            excitation_[(size_t) i] = i < pulse
                ? (float) (0.5 * (1.0 - std::cos(2.0 * M_PI * (double) i / (double) pulse)))
                : 0.0f;
        }

        // The comb below differences the excitation with a delayed copy of
        // itself, which is what removes the pulse's DC. That matters more here
        // than for a pluck: the loop filter has unity gain at DC by design, so
        // an offset would sit in the string and decay only as slowly as the
        // note itself — heard as a thump under every key.
        commitExcitation(length, combOffset, blow);
    }

    /** How hard the hammer's felt is: 0 is a soft, worn hammer, 1 a bright,
        freshly voiced one. Shortens contact time the same way a harder blow
        does, which is why a hard hammer sounds bright even played gently. */
    void setHammerHardness(float hardness) noexcept
    {
        hammerHardness_ = std::clamp(hardness, 0.0f, 1.0f);
    }

    /**
        Writes a built excitation into the loop and restarts the string.

        Shared by pluck() and strike() rather than restated: the comb, the
        placement relative to the write pointer, and the state reset are
        identical for both, and the only thing that differs is the shape of
        the burst that gets written.
    */
    void commitExcitation(int length, int combOffset, float velocity) noexcept
    {
        const int size = (int) buffer_.size();

        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_ = 0;

        // Excitation-position comb: the string cannot move at the point it is
        // held or struck, so the excitation cancels with a copy of itself
        // delayed by how far along that point is. Written backwards from the
        // write pointer, so the first sample read is the start of the burst.
        for (int i = 0; i < length; ++i)
        {
            const int   earlier = i - combOffset;
            const float delayed = earlier >= 0 ? excitation_[(size_t) earlier] : 0.0f;

            int index = writeIndex_ - length + i;
            while (index < 0)
                index += size;
            buffer_[(size_t) index] = (excitation_[(size_t) i] - delayed) * velocity;
        }

        lastFilterIn_   = 0.0f;
        allpassLastIn_  = 0.0f;
        allpassLastOut_ = 0.0f;
        energy_         = velocity;
    }

    /**
        Injects energy arriving through the bridge from the other strings.

        A guitar's bridge is not perfectly rigid: a struck string moves it, and
        that motion drives every other string attached to it. That is what
        makes an open string ring sympathetically, and a large part of why a
        real chord blooms while six independent waveguides just stack up.

        Added at the write index, i.e. into the loop's input for this sample,
        so it enters the string the same way its own feedback does. Kept small
        by the caller: the string-to-string-and-back path is a feedback loop,
        and its gain has to stay well under one (see GuitarNode::setCoupling).
    */
    void couple(float bridgeSignal) noexcept
    {
        const int size = (int) buffer_.size();
        if (size < 4 || bridgeSignal == 0.0f)
            return;

        // Accumulated rather than written into the buffer directly. The slot
        // at writeIndex_ is the one process() is about to *assign*, so writing
        // there is silently discarded — which is exactly what happened when
        // this was first written, and what the coupling test caught: the
        // neighbouring string received precisely zero energy.
        pendingCoupling_ += bridgeSignal;

        // Energy is what isRinging() reports, and a string only sounding
        // because of coupling still has to be processed next block or it will
        // be skipped and never ring at all.
        const float magnitude = std::abs(bridgeSignal);
        if (magnitude > energy_)
            energy_ = magnitude;
    }

    /** Damps the string — palm muting, or a hand laid across it. 0 = open,
        1 = fully stopped. */
    void mute(float amount) noexcept
    {
        muteFactor_ = 1.0f - std::clamp(amount, 0.0f, 1.0f);
        updateLoopGain();
    }

    /** True while the string is still audibly moving. Lets a caller skip
        silent strings rather than running six loops for one note. */
    bool isRinging() const noexcept { return energy_ > 1.0e-5f; }

    /** One sample. */
    float process() noexcept
    {
        const int size = (int) buffer_.size();
        if (size < 4)
            return 0.0f;

        int readIndex = writeIndex_ - integerDelay_;
        while (readIndex < 0)
            readIndex += size;

        const float delayed = buffer_[(size_t) readIndex];

        // Allpass interpolation for the fractional part of the loop: it delays
        // without attenuating, so tuning and damping stay independent.
        const float interpolated = allpassCoeff_ * (delayed - allpassLastOut_) + allpassLastIn_;
        allpassLastIn_  = delayed;
        allpassLastOut_ = interpolated;

        // Stiffness: an allpass cascade, so each frequency is delayed by a
        // different amount and the partials stretch. Same first-order form as
        // the interpolator above, run several times over.
        float dispersed = interpolated;
        if (dispersionCoeff_ != 0.0f)
        {
            for (int i = 0; i < activeDispersionSections_; ++i)
            {
                const float input  = dispersed;
                const float output = dispersionCoeff_ * (input - dispersionY_[(size_t) i])
                                   + dispersionX_[(size_t) i];

                dispersionX_[(size_t) i] = input;
                dispersionY_[(size_t) i] = output;
                dispersed = output;
            }
        }

        // One-zero lowpass, unity gain at DC, so the decay rate is set by
        // loopGain_ alone and the filter only shapes the tail's brightness.
        const float filtered = loopGain_ * ((1.0f - damping_) * dispersed + damping_ * lastFilterIn_);
        lastFilterIn_ = dispersed;

        // Whatever arrived through the bridge since the last sample joins the
        // loop's input here, the same place the string's own feedback enters.
        buffer_[(size_t) writeIndex_] = filtered + pendingCoupling_;
        pendingCoupling_ = 0.0f;
        writeIndex_ = (writeIndex_ + 1) % size;

        // Cheap envelope follower, only so isRinging() can retire the voice.
        energy_ += 0.001f * (std::abs(filtered) - energy_);

        // The dispersed signal is the string's actual motion at the pickup —
        // returning the pre-dispersion value would leave the stiffness audible
        // only through the feedback path and not in the note itself.
        return dispersed;
    }

private:
    int loopLengthSamples() const noexcept
    {
        return (int) std::lround(sampleRate_ / frequency_);
    }

    /** Splits the required loop period across the integer delay, the allpass
        fraction and the loop filter's own phase delay — all three add up, so
        the filter's contribution has to come out of the delay line or the
        string plays sharp. */
    void updateLoopLength() noexcept
    {
        if (buffer_.size() < 4)
            return;

        const double totalPeriod = sampleRate_ / frequency_;

        // Phase delay of the one-zero loop filter at the fundamental, derived
        // rather than approximated as `damping_`: the approximation is fine low
        // down and drifts sharp as the pitch rises, which is exactly where
        // tuning errors are most audible.
        const double omega = 2.0 * M_PI * frequency_ / sampleRate_;
        const double b     = damping_;
        const double real  = (1.0 - b) + b * std::cos(omega);
        const double imag  = -b * std::sin(omega);
        const double filterDelay = omega > 1.0e-9 ? -std::atan2(imag, real) / omega : b;

        // The dispersion allpasses delay the fundamental too, and unless that
        // is taken out of the line the whole string plays flat — by a lot, at
        // eight sections. Derived at the fundamental for the same reason the
        // loop filter's is, rather than approximated by the coefficient.
        //
        // How many of them actually run is decided here, against the note's
        // own period. A section's delay is a fixed number of *samples*, while
        // a high note's whole period is only a hundred or so — so a full
        // cascade can easily ask for more delay than the string has, which
        // leaves no delay line at all and the note plays at whatever pitch the
        // clamp allows. Budgeting a quarter of the period keeps the string in
        // tune everywhere and simply gives high notes less stiffness, which is
        // the graceful failure: they have fewer audible partials to stretch in
        // the first place.
        double dispersionDelay = 0.0;
        activeDispersionSections_ = 0;

        if (dispersionCoeff_ != 0.0f)
        {
            const double perSection = allpassPhaseDelay((double) dispersionCoeff_, omega);

            if (perSection > 1.0e-9)
            {
                const double budget = 0.25 * totalPeriod;
                activeDispersionSections_ = std::clamp((int) std::floor(budget / perSection),
                                                       0, kDispersionSections);
                dispersionDelay = activeDispersionSections_ * perSection;
            }
        }

        double lineDelay = totalPeriod - filterDelay - dispersionDelay;

        // The allpass is well behaved for fractions around 0.5 and misbehaves
        // near zero, so borrow a whole sample from the integer part.
        int    integerPart = (int) std::floor(lineDelay);
        double fraction    = lineDelay - integerPart;
        if (fraction < 0.1)
        {
            integerPart -= 1;
            fraction    += 1.0;
        }

        integerDelay_  = std::clamp(integerPart, 1, (int) buffer_.size() - 2);
        allpassCoeff_  = (float) ((1.0 - fraction) / (1.0 + fraction));
    }

    /** Loop gain for the requested T60. A string goes round its loop f0 times a
        second, so the per-pass gain that reaches -60dB in t seconds depends on
        pitch — this is the pitch compensation the plan calls for. Always < 1,
        so the loop cannot self-oscillate. */
    /**
        Applies the requested damping, capped so the loop filter cannot take
        meaningful energy out of the *fundamental*.

        The filter is there to darken the tail — to roll the harmonics off as
        the note rings. Low down that is exactly what it does, because the
        fundamental sits far below the filter's reach. High up it does not: at
        2.6kHz the fundamental is already where the filter cuts, and since a
        note that pitch goes round the loop thousands of times a second, a loss
        of a fraction of a percent per pass compounds into a note that is gone
        in a tenth of a second no matter what decay time was asked for.

        That is what made a piano's top octave silent, and it was invisible on
        a guitar whose highest note is half that pitch. The cap solves for the
        largest damping whose gain at the fundamental stays above kMinFundamentalGain,
        and leaves anything lower untouched — so nothing in the guitar's range
        changes, and the top of a piano keeps the brightness a short string
        actually has.
    */
    void updateDamping() noexcept
    {
        damping_ = requestedDamping_;

        const double omega = 2.0 * M_PI * frequency_ / sampleRate_;
        const double u     = 1.0 - std::cos(omega);

        if (u > 1.0e-12)
        {
            // |H|^2 = 1 - 2bu + 2ub^2, solved for |H| = kMinFundamentalGain.
            constexpr double g = kMinFundamentalGain;
            const double discriminant = 1.0 - 2.0 * (1.0 - g * g) / u;

            // Negative means no damping value in range loses that much here —
            // the whole guitar range — so the request stands as asked.
            if (discriminant >= 0.0)
            {
                const auto cap = (float) (0.5 * (1.0 - std::sqrt(discriminant)));
                damping_ = std::min(damping_, cap);
            }
        }

        updateLoopLength(); // the filter's own delay is part of the loop
        updateLoopGain();
    }

    void updateLoopGain() noexcept
    {
        const double passes = std::max(1.0, decaySeconds_ * frequency_);
        double perPass = std::exp(std::log(0.001) / passes);

        // Divided by what the damping filter itself takes out at the
        // fundamental, because that is part of the loop too and it is *not*
        // small up high.
        //
        // Without this, setDecaySeconds does not mean what it says at the top
        // of the range — the documented contract is a time that "holds across
        // the range", and it did not. The filter's per-pass loss is tiny (a
        // fraction of a percent) but a high note goes round the loop thousands
        // of times a second, so it compounds into everything: a 2.6kHz note
        // asked for a three-second decay and got about a tenth of one. The
        // guitar never showed it because its top note is half that pitch;
        // building a piano, whose top C is 4186Hz, is what exposed it.
        const double omega = 2.0 * M_PI * frequency_ / sampleRate_;
        const double b     = damping_;
        const double real  = (1.0 - b) + b * std::cos(omega);
        const double imag  = -b * std::sin(omega);
        const double filterGain = std::hypot(real, imag);

        if (filterGain > 1.0e-6)
            perPass /= filterGain;

        // Still clamped below unity: a very high note with very dark damping
        // can ask for more compensation than a stable loop allows, and there
        // the note simply decays faster than requested. That is a graceful
        // limit rather than a silent one — the alternative is a loop that
        // grows.
        loopGain_ = std::min((float) perPass * muteFactor_, 0.99999f);
    }

    /** Deterministic noise: a plucked string wants a burst, and a fixed
        sequence makes the tests repeatable. */
    /** Pick-position variation, on its own stream — see pluck(). */
    /** Phase delay, in samples, of one first-order allpass
        (a + z^-1)/(1 + a z^-1) at @p omega radians/sample. */
    static double allpassPhaseDelay(double a, double omega) noexcept
    {
        if (omega < 1.0e-9)
            return (1.0 - a) / (1.0 + a); // the DC limit, where the ratio below is 0/0

        const double sine   = std::sin(omega);
        const double cosine = std::cos(omega);

        const double numerator   = std::atan2(-sine, a + cosine);
        const double denominator = std::atan2(-a * sine, 1.0 + a * cosine);

        return -(numerator - denominator) / omega;
    }

    float nextJitter() noexcept
    {
        jitterState_ = jitterState_ * 1664525u + 1013904223u;
        return (float) ((int32_t) jitterState_) * (1.0f / 2147483648.0f);
    }

    float nextNoise() noexcept
    {
        noiseState_ = noiseState_ * 1664525u + 1013904223u;
        return (float) ((int32_t) noiseState_) * (1.0f / 2147483648.0f);
    }

    double sampleRate_    = 48000.0;
    double frequency_     = 110.0;
    double decaySeconds_  = 2.0;

    std::vector<float> buffer_;
    std::vector<float> excitation_;
    int                writeIndex_   = 0;
    int                integerDelay_ = 100;

    float allpassCoeff_   = 0.0f;
    float allpassLastIn_  = 0.0f;
    float allpassLastOut_ = 0.0f;
    float lastFilterIn_   = 0.0f;

    float damping_      = 0.15f;
    float loopGain_     = 0.999f;
    float muteFactor_   = 1.0f;
    float pickPosition_ = 0.25f;
    /** The least the loop filter may pass at the fundamental. Chosen so the
        filter's contribution to the decay stays negligible next to the decay
        actually requested. */
    static constexpr double kMinFundamentalGain = 0.9995;

    float requestedDamping_ = 0.5f - 0.45f * 0.7f; // matches the default brightness
    float pickHardness_ = 0.6f;
    float velocitySensitivity_ = 0.0f; // 0 = the behaviour that predates this
    uint32_t jitterState_ = 0x9e3779b9u; // seeded away from the noise stream
    float    pendingCoupling_ = 0.0f;    // bridge energy awaiting the next sample

    /** How many allpass sections the stiffness cascade uses. More sections
        spread the partials further for the same coefficient; four is enough
        for a guitar, where the stretch is subtle compared to a piano's. */
    static constexpr int   kDispersionSections = 8;
    static constexpr float kMaxDispersion      = 0.88f;

    /** The contact time a hammer spends on the string, in milliseconds:
        roughly the range a real one covers between a gentle blow on soft felt
        and a hard blow on bright felt. */
    static constexpr float kMaxContactMs = 4.0f;
    static constexpr float kMinContactMs = 0.6f;

    float hammerHardness_  = 0.5f;
    float stiffness_       = 0.0f;
    int   activeDispersionSections_ = 0; // how many fit in this note's period
    float dispersionCoeff_ = 0.0f; // negative: see setStiffness
    std::array<float, kDispersionSections> dispersionX_ {};
    std::array<float, kDispersionSections> dispersionY_ {};
    float energy_       = 0.0f;

    uint32_t noiseState_ = 22222u;
};

} // namespace looper::engine
