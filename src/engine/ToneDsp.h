#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "engine/DelayLine.h"
#include "engine/ShelfPeakFilter.h"

namespace soundsplice::engine
{
/**
    The DSP of the Phaser, Flanger, Bass and Treble, and Stereo Tools effects,
    one channel (or one stereo frame) at a time. JUCE-free, so each is tested
    headless; engine/ToneEffects.h wraps them as chain effects.
*/

/**
    A phaser: a chain of first-order allpass filters whose corner an LFO
    sweeps, mixed back with the dry signal. Each pair of stages makes one notch
    where the chain's phase shift cancels the dry path; sweeping moves the
    notches. Feedback sharpens them.
*/
class Phaser
{
public:
    static constexpr int kMaxStages = 12;

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    void reset() noexcept
    {
        state_.fill(0.0);
        lastOut_ = 0.0;
    }

    /** Where in its cycle the LFO starts, 0..1: a stereo pair runs a quarter apart. */
    void setPhase(double phase) noexcept { phase_ = phase - std::floor(phase); }

    void setRateHz(float hz) noexcept      { rateHz_ = std::clamp(hz, 0.01f, 20.0f); }
    void setDepth(float depth) noexcept    { depth_ = std::clamp(depth, 0.0f, 1.0f); }
    void setFeedback(float f) noexcept     { feedback_ = std::clamp(f, -0.95f, 0.95f); }
    void setStages(int stages) noexcept    { stages_ = std::clamp(stages - stages % 2, 2, kMaxStages); }
    void setMix(float mix) noexcept        { mix_ = std::clamp(mix, 0.0f, 1.0f); }

    float processSample(float input) noexcept
    {
        // The sweep runs over octaves, as hearing does: from kLowestHz up by
        // as many as the depth allows.
        const double lfo    = 0.5 - 0.5 * std::cos(kTwoPi * phase_);
        const double hz     = std::min(kLowestHz * std::pow(2.0, lfo * depth_ * kOctaves), 0.45 * sampleRate_);
        const double t      = std::tan(kPi * hz / sampleRate_);
        const double coeff  = (t - 1.0) / (t + 1.0);

        double x = (double) input + feedback_ * lastOut_;
        for (int stage = 0; stage < stages_; ++stage)
        {
            // y = a*x + x[-1] - a*y[-1], in transposed form: one state per stage.
            auto&        z = state_[(size_t) stage];
            const double y = coeff * x + z;
            z              = x - coeff * y;
            x              = y;
        }
        lastOut_ = std::abs(x) < 1.0e-20 ? 0.0 : x;

        phase_ += (double) rateHz_ / sampleRate_;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        return (float) ((double) input * (1.0 - mix_) + x * mix_);
    }

private:
    static constexpr double kPi       = 3.14159265358979323846;
    static constexpr double kTwoPi    = 2.0 * kPi;
    static constexpr double kLowestHz = 150.0;
    static constexpr double kOctaves  = 6.0;

    std::array<double, kMaxStages> state_ {};
    double sampleRate_ = 48000.0;
    double phase_      = 0.0;
    double lastOut_    = 0.0;
    float  rateHz_     = 0.5f;
    float  depth_      = 0.7f;
    float  feedback_   = 0.5f;
    int    stages_     = 6;
    float  mix_        = 0.5f;
};

/**
    A flanger: the signal mixed with a copy delayed by a millisecond or so, the
    delay swept by an LFO, so the comb of notches that makes moves up and down
    together. Feedback, either sign, makes the comb resonant.
*/
class Flanger
{
public:
    static constexpr double kMaxDelayMs = 10.0;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        line_.prepare((int) std::ceil(kMaxDelayMs * 0.001 * sampleRate_) + 4);
        reset();
    }

    void reset() noexcept { line_.reset(); }

    void setPhase(double phase) noexcept { phase_ = phase - std::floor(phase); }

    void setRateHz(float hz) noexcept     { rateHz_ = std::clamp(hz, 0.01f, 10.0f); }
    void setDepth(float depth) noexcept   { depth_ = std::clamp(depth, 0.0f, 1.0f); }
    void setDelayMs(float ms) noexcept    { delayMs_ = std::clamp(ms, 0.1f, 5.0f); }
    void setFeedback(float f) noexcept    { feedback_ = std::clamp(f, -0.95f, 0.95f); }
    void setMix(float mix) noexcept       { mix_ = std::clamp(mix, 0.0f, 1.0f); }

    float processSample(float input) noexcept
    {
        const double lfo     = 0.5 - 0.5 * std::cos(kTwoPi * phase_);
        const double delayMs = (double) delayMs_ + lfo * depth_ * kSweepMs;
        const float  wet     = line_.processSampleFractional(input, delayMs * 0.001 * sampleRate_, feedback_);

        phase_ += (double) rateHz_ / sampleRate_;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        return input * (1.0f - mix_) + wet * mix_;
    }

private:
    static constexpr double kTwoPi   = 6.283185307179586;
    static constexpr double kSweepMs = 4.5; // with the longest base delay, inside kMaxDelayMs

    DelayLine line_;
    double    sampleRate_ = 48000.0;
    double    phase_      = 0.0;
    float     rateHz_     = 0.25f;
    float     depth_      = 0.7f;
    float     delayMs_    = 1.0f;
    float     feedback_   = 0.5f;
    float     mix_        = 0.5f;
};

/** Bass and treble, as Audacity's: a low shelf at 100 Hz, a high shelf at
    8 kHz, and an output volume. */
class BassTreble
{
public:
    static constexpr float kBassHz   = 100.0f;
    static constexpr float kTrebleHz = 8000.0f;

    void prepare(double sampleRate)
    {
        bass_.prepare(sampleRate);
        bass_.setShape(ShelfPeakFilter::Shape::LowShelf);
        bass_.setFrequency(kBassHz);
        bass_.setQ(0.707f);
        treble_.prepare(sampleRate);
        treble_.setShape(ShelfPeakFilter::Shape::HighShelf);
        treble_.setFrequency(kTrebleHz);
        treble_.setQ(0.707f);
        setBassDb(bassDb_);
        setTrebleDb(trebleDb_);
    }

    void setBassDb(float db)
    {
        if (db != bassDb_ || ! bassSet_)
            bass_.setGainDb(bassDb_ = db);
        bassSet_ = true;
    }

    void setTrebleDb(float db)
    {
        if (db != trebleDb_ || ! trebleSet_)
            treble_.setGainDb(trebleDb_ = db);
        trebleSet_ = true;
    }

    void setVolumeDb(float db) noexcept { volume_ = std::pow(10.0f, db / 20.0f); }

    float processSample(float input) noexcept
    {
        return treble_.processSample(bass_.processSample(input)) * volume_;
    }

    /** The steady-state response at @p hz, in dB, volume included. */
    float magnitudeDbAt(float hz) const
    {
        return bass_.magnitudeDbAt(hz) + treble_.magnitudeDbAt(hz) + 20.0f * std::log10(volume_);
    }

private:
    ShelfPeakFilter bass_, treble_;
    float           bassDb_ = 0.0f, trebleDb_ = 0.0f;
    bool            bassSet_ = false, trebleSet_ = false;
    float           volume_ = 1.0f;
};

/**
    A channel mixer for a stereo pair: swap the sides, fold to mono, set the
    width in mid/side (0 is mono, 1 unchanged, 2 twice as wide), and balance
    one side down against the other. Unlike the mastering widener it adds no
    mid compensation: this is a plain tool, and at width 1, balance 0 with
    nothing switched on it leaves every sample exactly as it was.
*/
struct StereoTool
{
    float width   = 1.0f;  // 0..2
    float balance = 0.0f;  // -1 (left only) .. +1 (right only)
    bool  mono    = false;
    bool  swap    = false;

    void processFrame(float& left, float& right) const noexcept
    {
        if (swap)
            std::swap(left, right);

        if (mono || width != 1.0f)
        {
            const float mid  = 0.5f * (left + right);
            const float side = mono ? 0.0f : 0.5f * (left - right) * std::clamp(width, 0.0f, 2.0f);
            left             = mid + side;
            right            = mid - side;
        }

        const float b = std::clamp(balance, -1.0f, 1.0f);
        if (b > 0.0f)
            left *= 1.0f - b;
        else if (b < 0.0f)
            right *= 1.0f + b;
    }
};

} // namespace soundsplice::engine
