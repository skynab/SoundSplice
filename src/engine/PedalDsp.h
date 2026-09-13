#pragma once

#include <algorithm>
#include <cmath>

#include "engine/DelayLine.h"
#include "engine/SequencerMath.h"
#include "engine/ShelfPeakFilter.h"
#include "engine/StateVariableFilter.h"

namespace looper::engine
{
/**
    A compressor pedal.

    Squash pedals are on more guitar boards than anything except tuners: they
    even out picking, add sustain to a clean part, and are what makes a funk
    or country line sit still. They also matter *before* a drive — a
    compressor into an overdrive is a different, more even distortion than an
    overdrive alone, which is exactly the kind of thing the chain's ordering
    exists to let you try.

    Feed-forward, and the smoothing is applied to the **gain reduction**
    rather than to the level detector. Smoothing the detector makes attack and
    release interact with signal level, so a stated 10ms attack isn't 10ms for
    a quiet note; smoothing the reduction makes them mean what they say, which
    is the only way the numbers on the control are worth showing.

    JUCE-free so the timing claims are measurable headlessly: an attack time
    that is quietly wrong is inaudible one note at a time and wrong on every
    note at once.
*/
class Compressor
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        updateCoefficients();
        reset();
    }

    void reset() noexcept { reductionDb_ = 0.0f; }

    void setThresholdDb(float db) noexcept { thresholdDb_ = db; }
    void setRatio(float ratio) noexcept    { ratio_ = std::max(1.0f, ratio); }

    void setAttackMs(float ms) noexcept
    {
        attackMs_ = std::max(0.1f, ms);
        updateCoefficients();
    }

    void setReleaseMs(float ms) noexcept
    {
        releaseMs_ = std::max(1.0f, ms);
        updateCoefficients();
    }

    /** The linear gain to apply to this sample, advancing the internal state.
        Returned rather than applied so a stereo pair can share one detector —
        compressing channels independently makes a hard-panned note pull the
        image across, which is not what a pedal does. */
    float gainFor(float detectorInput) noexcept
    {
        const float level   = std::abs(detectorInput);
        const float levelDb = 20.0f * std::log10(std::max(level, 1.0e-9f));
        const float overDb  = levelDb - thresholdDb_;

        // Above the threshold, keep 1/ratio of the excess: at 4:1, 12dB over
        // becomes 3dB over, so 9dB is given back.
        const float targetReductionDb = overDb > 0.0f ? -overDb * (1.0f - 1.0f / ratio_) : 0.0f;

        // More reduction is the attack direction; letting go is release.
        const float coeff = targetReductionDb < reductionDb_ ? attackCoeff_ : releaseCoeff_;
        reductionDb_ += coeff * (targetReductionDb - reductionDb_);

        return std::pow(10.0f, reductionDb_ / 20.0f);
    }

    /** How much gain reduction is currently applied, in dB (negative). Used by
        the tests, and by any meter that wants to show it. */
    float currentReductionDb() const noexcept { return reductionDb_; }

private:
    void updateCoefficients() noexcept
    {
        attackCoeff_  = timeToCoeff(attackMs_);
        releaseCoeff_ = timeToCoeff(releaseMs_);
    }

    /** One-pole coefficient reaching ~63% of a step in the stated time, which
        is the convention the numbers on a pedal refer to. */
    float timeToCoeff(float ms) const noexcept
    {
        const double samples = std::max(1.0, (double) ms * 0.001 * sampleRate_);
        return (float) (1.0 - std::exp(-1.0 / samples));
    }

    double sampleRate_   = 48000.0;
    float  thresholdDb_  = -18.0f;
    float  ratio_        = 4.0f;
    float  attackMs_     = 10.0f;
    float  releaseMs_    = 120.0f;
    float  attackCoeff_  = 0.01f;
    float  releaseCoeff_ = 0.001f;
    float  reductionDb_  = 0.0f;
};

/**
    A noise gate pedal.

    Compressor's mirror image: it attenuates *below* a threshold instead of
    above one, down to a floor (rangeDb) rather than by a ratio, because a
    gate isn't evening anything out — closed means "silent" (or as close to
    it as rangeDb allows), not "quieter." What it exists for is what
    high-gain distortion does to a guitar's noise floor: the hiss and hum a
    clean signal barely has gets amplified right along with the notes, and a
    gate is what keeps that hiss from filling every rest.

    A hold time is the one thing this needs that Compressor doesn't: without
    it, a decaying note whose level wanders back and forth across the
    threshold makes the gate chatter open and closed instead of closing once,
    cleanly, when the note is actually done. Held open through the hold
    window even after the level dips back under threshold, then released.

    Same feed-forward, smooth-the-output-not-the-detector shape as
    Compressor, and the same reason: a stated attack/release time should mean
    what it says regardless of signal level. JUCE-free for the same
    headless-measurability reason.
*/
class Gate
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        updateCoefficients();
        reset();
    }

    void reset() noexcept
    {
        gainDb_      = -rangeDb_;
        holdCounter_ = 0;
    }

    void setThresholdDb(float db) noexcept { thresholdDb_ = db; }
    void setRangeDb(float db) noexcept     { rangeDb_ = std::max(0.0f, db); }

    void setAttackMs(float ms) noexcept
    {
        attackMs_ = std::max(0.1f, ms);
        updateCoefficients();
    }

    void setHoldMs(float ms) noexcept
    {
        holdMs_ = std::max(0.0f, ms);
        updateCoefficients();
    }

    void setReleaseMs(float ms) noexcept
    {
        releaseMs_ = std::max(1.0f, ms);
        updateCoefficients();
    }

    /** The linear gain to apply to this sample, advancing the internal
        state. Returned rather than applied, same reason as Compressor's
        gainFor: a stereo pair shares one detector so a hard-panned note
        doesn't pull the image across as it opens/closes. */
    float gainFor(float detectorInput) noexcept
    {
        const float level   = std::abs(detectorInput);
        const float levelDb = 20.0f * std::log10(std::max(level, 1.0e-9f));
        const bool  open    = levelDb > thresholdDb_;

        if (open)
            holdCounter_ = holdSamples_;
        else if (holdCounter_ > 0)
            --holdCounter_;

        const float targetGainDb = (open || holdCounter_ > 0) ? 0.0f : -rangeDb_;

        // Opening is the attack direction; closing is release - the mirror
        // of Compressor's "more reduction is attack" comparison.
        const float coeff = targetGainDb > gainDb_ ? attackCoeff_ : releaseCoeff_;
        gainDb_ += coeff * (targetGainDb - gainDb_);

        return std::pow(10.0f, gainDb_ / 20.0f);
    }

    /** How far below unity the gate currently sits, in dB (negative, 0 when
        fully open). Same purpose as Compressor::currentReductionDb: tests,
        and any meter that wants to show it. */
    float currentGainDb() const noexcept { return gainDb_; }

private:
    void updateCoefficients() noexcept
    {
        attackCoeff_  = timeToCoeff(attackMs_);
        releaseCoeff_ = timeToCoeff(releaseMs_);
        holdSamples_  = (int) std::lround((double) holdMs_ * 0.001 * sampleRate_);
    }

    /** Identical to Compressor::timeToCoeff - one-pole coefficient reaching
        ~63% of a step in the stated time. */
    float timeToCoeff(float ms) const noexcept
    {
        const double samples = std::max(1.0, (double) ms * 0.001 * sampleRate_);
        return (float) (1.0 - std::exp(-1.0 / samples));
    }

    double sampleRate_    = 48000.0;
    float  thresholdDb_   = -40.0f;
    float  rangeDb_       = 60.0f;
    float  attackMs_      = 2.0f;
    float  holdMs_        = 20.0f;
    float  releaseMs_     = 150.0f;
    float  attackCoeff_   = 0.5f;
    float  releaseCoeff_  = 0.01f;
    int    holdSamples_   = 960;
    int    holdCounter_   = 0;
    float  gainDb_        = -60.0f;
};

/**
    Three bands of EQ in series: low shelf, a sweepable mid bell, high shelf.

    One channel's worth. JUCE-free like the rest of this header, so what the
    bands actually do to a signal is a headless measurement rather than a
    claim - which matters here because "the mid is sweepable" and "the shelves
    stay out of each other's way" are exactly the sort of thing that is easy
    to believe and easy to get wrong.

    Coefficients are recomputed only when a value changes: an RBJ biquad's
    update runs a cos, a sin and a sqrt, and repeating that per block for
    knobs that are usually still is real work for nothing.
*/
class ThreeBandEq
{
public:
    struct Settings
    {
        float lowShelfHz = 100.0f, lowShelfDb = 0.0f;
        float midHz = 800.0f, midDb = 0.0f, midQ = 1.0f;
        float highShelfHz = 4000.0f, highShelfDb = 0.0f;

        bool operator==(const Settings&) const = default;
    };

    void prepare(double sampleRate)
    {
        low_.prepare(sampleRate);
        low_.setShape(ShelfPeakFilter::Shape::LowShelf);
        mid_.prepare(sampleRate);
        mid_.setShape(ShelfPeakFilter::Shape::Peaking);
        high_.prepare(sampleRate);
        high_.setShape(ShelfPeakFilter::Shape::HighShelf);

        applied_ = Settings {};
        applied_.lowShelfHz = -1.0f; // nothing legal matches, so the first set applies
        setSettings(Settings {});
    }

    void reset() noexcept
    {
        low_.reset();
        mid_.reset();
        high_.reset();
    }

    void setSettings(const Settings& wanted)
    {
        Settings clamped;
        clamped.lowShelfHz  = std::clamp(wanted.lowShelfHz, 20.0f, 1000.0f);
        clamped.lowShelfDb  = std::clamp(wanted.lowShelfDb, -24.0f, 24.0f);
        clamped.midHz       = std::clamp(wanted.midHz, 100.0f, 8000.0f);
        clamped.midDb       = std::clamp(wanted.midDb, -24.0f, 24.0f);
        clamped.midQ        = std::clamp(wanted.midQ, 0.2f, 8.0f);
        clamped.highShelfHz = std::clamp(wanted.highShelfHz, 1000.0f, 16000.0f);
        clamped.highShelfDb = std::clamp(wanted.highShelfDb, -24.0f, 24.0f);

        if (clamped == applied_)
            return;

        low_.setFrequency(clamped.lowShelfHz);
        low_.setGainDb(clamped.lowShelfDb);
        mid_.setFrequency(clamped.midHz);
        mid_.setGainDb(clamped.midDb);
        mid_.setQ(clamped.midQ);
        high_.setFrequency(clamped.highShelfHz);
        high_.setGainDb(clamped.highShelfDb);

        applied_ = clamped;
    }

    float processSample(float x) noexcept
    {
        return high_.processSample(mid_.processSample(low_.processSample(x)));
    }

    /** The three bands' summed response at @p hz, in dB, read from the live
        coefficients - so a UI curve cannot disagree with what is heard. */
    float magnitudeDbAt(float hz) const
    {
        return low_.magnitudeDbAt(hz) + mid_.magnitudeDbAt(hz) + high_.magnitudeDbAt(hz);
    }

private:
    ShelfPeakFilter low_, mid_, high_;
    Settings        applied_;
};

/**
    A tremolo pedal: amplitude modulation, the oldest effect on this list.

    Depth is expressed as how far the *quiet* part drops rather than as a
    peak-to-peak swing, so depth 1 means "silent at the bottom" and depth 0
    means the pedal is doing nothing. That's the only reading under which
    turning the knob down leaves the signal untouched, which is what a player
    expects a depth control to do.

    The phase is kept in a double and wrapped rather than accumulated
    unbounded: at 48kHz an unwrapped float phase loses its resolution within
    minutes, and the audible result is a tremolo that gradually stops being
    periodic.
*/
class Tremolo
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    void reset() noexcept { phase_ = 0.0; }

    void setRateHz(float hz) noexcept  { rateHz_ = std::clamp(hz, 0.05f, 20.0f); }
    void setDepth(float depth) noexcept { depth_ = std::clamp(depth, 0.0f, 1.0f); }

    /** The gain for this sample, and the phase advances. Like the compressor,
        the gain is returned rather than applied so both channels move
        together — a tremolo that drifted between channels would turn into an
        auto-panner. */
    float nextGain() noexcept
    {
        // Starts at full and dips: a tremolo that began silent would swallow
        // the front of any note played on the beat.
        const float lfo  = 0.5f * (1.0f + (float) std::cos(kTwoPi * phase_));
        const float gain = 1.0f - depth_ * (1.0f - lfo);

        phase_ += (double) rateHz_ / sampleRate_;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        return gain;
    }

private:
    static constexpr double kTwoPi = 6.283185307179586;

    double sampleRate_ = 48000.0;
    double phase_      = 0.0;
    float  rateHz_     = 5.0f;
    float  depth_      = 0.5f;
};

/**
    A chorus pedal: the signal mixed with slightly delayed, slowly detuned
    copies of itself.

    The detuning is the effect. A fixed short delay mixed with the dry signal
    is a comb filter — a static tone colour, not a chorus. Sweeping that delay
    makes each copy drift sharp and flat around the original, and it's the
    beating between them that reads as several instruments rather than one.
    Depth at zero is therefore a deliberate state, not a broken one: it leaves
    the comb without the movement.

    Two voices, their LFOs half a cycle apart, so one copy drifts sharp while
    the other drifts flat. In phase they would move together and sound like a
    single detuned copy.

    The delay is read at a fractional position. Reading whole samples only
    would quantise the sweep and click on every step — see
    DelayLine::processSampleFractional, which exists for this.

    JUCE-free so the claims about it can be measured headlessly: that the
    modulation actually modulates, and that a swept delay stays smooth.
*/
class Chorus
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // Sized for the longest delay the controls can ask for, once, here —
        // the audio thread never resizes it.
        const int maxSamples = (int) std::ceil((kMaxDelayMs + kMaxDepthMs) * 0.001 * sampleRate_) + 4;
        line_.prepare(maxSamples);
        reset();
    }

    void reset() noexcept
    {
        line_.reset();
        phase_ = 0.0;
    }

    void setRateHz(float hz) noexcept  { rateHz_ = std::clamp(hz, 0.05f, 8.0f); }
    void setDepth(float depth) noexcept { depth_ = std::clamp(depth, 0.0f, 1.0f); }
    void setMix(float mix) noexcept     { mix_ = std::clamp(mix, 0.0f, 1.0f); }

    float processSample(float input) noexcept
    {
        const double centreSamples = kCentreDelayMs * 0.001 * sampleRate_;
        const double swingSamples  = (double) depth_ * kMaxDepthMs * 0.001 * sampleRate_;

        // Half a cycle apart: one copy drifts sharp as the other drifts flat.
        const double lfoA = std::sin(kTwoPi * phase_);
        const double lfoB = std::sin(kTwoPi * (phase_ + 0.5));

        // One line, read twice. Two lines would hold the same samples twice
        // over for no benefit — the taps differ in where they read, not in
        // what was written.
        const float wetA = line_.processSampleFractional(input, centreSamples + swingSamples * lfoA, 0.0f);
        const float wetB = line_.readFractional(centreSamples + swingSamples * lfoB);

        phase_ += (double) rateHz_ / sampleRate_;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        const float wet = 0.5f * (wetA + wetB);
        return input * (1.0f - mix_) + wet * mix_;
    }

private:
    static constexpr double kTwoPi         = 6.283185307179586;
    static constexpr double kCentreDelayMs = 14.0; // short enough to fuse, long enough to beat
    static constexpr double kMaxDelayMs    = 14.0;
    static constexpr double kMaxDepthMs    = 8.0;

    DelayLine line_;
    double    sampleRate_ = 48000.0;
    double    phase_      = 0.0;
    float     rateHz_     = 0.6f;
    float     depth_      = 0.5f;
    float     mix_        = 0.5f;
};

/**
    A wobble filter: a resonant low-pass whose cutoff is swept by an LFO
    locked to the song's tempo, in beats rather than Hz.

    The tempo lock is the point. Dubstep's wobble is dialled in as a note
    division — a sixteenth, an eighth — because it has to land exactly on
    the bar; a free-running Hz rate would drift out of the groove the moment
    the song's tempo changed, which is a different and much less useful
    effect. bpm is taken per sample rather than cached at prepare() so a
    tempo change mid-block is heard immediately rather than one buffer late,
    matching how the rest of the engine treats tempo as something that can
    move under playback.

    Built on StateVariableFilter rather than a filter of its own: everything
    about *being* a resonant low-pass already lives there, and the only new
    behaviour here is how its cutoff argument changes over time.

    The sweep opens the filter rather than closing it — depth multiplies how
    far the cutoff rises above the base frequency, so depth 0 leaves the base
    tone playing rather than muting it, the same "depth 0 does nothing"
    contract Tremolo and Chorus already keep.

    JUCE-free so the tempo-sync claim is measurable headlessly: that the
    wobble period actually scales with bpm is exactly the kind of thing that
    is easy to wire wrong and go unnoticed one throb at a time.
*/
class Wobble
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        filter_.prepare(sampleRate_);
        reset();
    }

    void reset() noexcept
    {
        phase_ = 0.0;
        filter_.reset();
    }

    /** How many beats one full sweep takes: 0.25/0.5/1.0/2.0 for a sixteenth,
        an eighth, a quarter, a half note — the values a wobble is actually
        dialled in as. Floored well above zero so a mis-set rate slows the
        sweep to a crawl rather than dividing by it. */
    void setRateInBeats(float beats) noexcept { rateBeats_ = std::max(0.03125f, beats); }
    void setDepth(float depth) noexcept       { depth_ = std::clamp(depth, 0.0f, 1.0f); }
    void setBaseCutoffHz(float hz) noexcept   { baseCutoffHz_ = std::clamp(hz, 40.0f, 4000.0f); }
    void setResonance(float q) noexcept       { filter_.setResonance(std::max(0.1f, q)); }
    void setMix(float mix) noexcept           { mix_ = std::clamp(mix, 0.0f, 1.0f); }

    /** Advances the LFO by one sample at @p bpm and returns the filtered,
        mixed result. */
    float processSample(float input, double bpm) noexcept
    {
        // Starts at the base cutoff and sweeps upward, so a wobble beginning
        // on a downbeat opens rather than snapping shut on the first sample.
        const float lfo         = 0.5f * (1.0f - std::cos(kTwoPi * (float) phase_));
        const float octaveSweep = depth_ * kMaxOctaves * lfo;
        cutoffHz_ = baseCutoffHz_ * std::pow(2.0f, octaveSweep);
        filter_.setCutoff(cutoffHz_);

        const float hz = (float) hzForBeatDivision(bpm, (double) rateBeats_);
        if (hz > 0.0f)
        {
            phase_ += (double) hz / sampleRate_;
            if (phase_ >= 1.0)
                phase_ -= 1.0;
        }

        const float wet = filter_.processSample(input);
        return input * (1.0f - mix_) + wet * mix_;
    }

    /** The filter's cutoff as of the last processSample() call, in Hz. Used
        by the tests to measure the sweep directly rather than infer it from
        the audio it produces, and by any meter that wants to show it. */
    float currentCutoffHz() const noexcept { return cutoffHz_; }

private:
    static constexpr float kTwoPi      = 6.283185307179586f;
    static constexpr float kMaxOctaves = 3.0f; // full depth reaches 8x the base cutoff

    StateVariableFilter filter_;
    double sampleRate_   = 48000.0;
    double phase_        = 0.0;
    float  rateBeats_    = 0.25f; // a sixteenth note, a common wobble rate
    float  depth_        = 0.7f;
    float  baseCutoffHz_ = 200.0f;
    float  mix_          = 1.0f;
    float  cutoffHz_     = 200.0f;
};

} // namespace looper::engine
