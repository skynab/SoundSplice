#pragma once

#include <algorithm>
#include <cmath>

#include "engine/Waveshaper.h"

namespace looper::engine
{
/**
    A harmonic exciter: saturate the top end only, and mix it back under the
    original.

    The point isn't distortion, it's *apparent* brightness. Turning a treble
    shelf up amplifies whatever high frequencies are already there — on a dull
    source there may be nothing to amplify but hiss. Generating harmonics from
    the upper-mid content instead adds high-frequency energy that is
    musically related to what's already playing, so the result reads as
    "clearer" rather than "louder and hissier".

    Signal path per channel:

        input -> one-pole split at crossover -> high band -> shaper/drive -> * amount
              -> summed back onto the *whole* input

    The dry path is the whole signal, not the low band: this is a parallel
    effect, so at amount 0 the output must be bit-identical to the input. A
    crossover-and-recombine design can't promise that — two one-poles summed
    back together don't reconstruct the original exactly — and "turning it
    down doesn't quite leave the signal alone" is precisely the bug that makes
    an effect untrustworthy on a master bus.

    Uses the ADAA Waveshaper rather than a plain tanh for a reason that
    matters more here than anywhere else in this codebase: the shaper is being
    fed the *highest* band of the signal, so its harmonics start close to
    Nyquist and fold immediately. Naive saturation of a 6kHz component at
    48kHz puts its third harmonic at 18kHz and its fifth at 30kHz, which
    aliases back to 18kHz — inharmonic, and exactly the "cheap" sound a bad
    exciter has.

    JUCE-free so the claims above are headless tests rather than comments.
*/
class Exciter
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        updateCoefficient();
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : channels_)
        {
            channel.lowState = 0.0f;
            channel.shaper.reset();
        }
    }

    /** 0 = off (a bit-identical no-op), 1 = maximum added harmonics. */
    void setAmount(float amount) noexcept { amount_ = std::clamp(amount, 0.0f, 1.0f); }

    /** Everything above this is what gets saturated. */
    void setCrossoverHz(float hz) noexcept
    {
        crossoverHz_ = std::clamp(hz, 200.0f, 12000.0f);
        updateCoefficient();
    }

    float processSample(int channelIndex, float input) noexcept
    {
        if (amount_ <= 0.0f || channelIndex < 0 || channelIndex >= kMaxChannels)
            return input;

        auto& channel = channels_[(size_t) channelIndex];

        // A single pole, deliberately. This is a *subtractive* split — the
        // high band is what the low-pass didn't take — so extra poles don't
        // make it steeper, they add phase lag, and subtracting a
        // further-lagged copy leaves a *larger* residual. Measured: cascading
        // a second pole doubled the low-frequency leakage (0.058 -> 0.114 on
        // a 100Hz tone) rather than reducing it.
        channel.lowState += coefficient_ * (input - channel.lowState);
        const float high = input - channel.lowState;

        // Drive rises with amount so the control does something across its
        // whole range rather than only near the top.
        const float drive = 1.0f + amount_ * 6.0f;
        channel.shaper.setDrive(drive);

        // Divided by the drive again on the way out. Waveshaper multiplies by
        // drive before shaping, so without this the stage has `drive` times
        // linear gain on the whole high band — which is how the low-frequency
        // residue of the split (small, but real) got amplified 7x and became
        // audible distortion on the bass. Compensating leaves small signals
        // at unity and lets only large ones actually saturate, which is what
        // "excite" is supposed to mean.
        const float excited = channel.shaper.processSample(high) / drive;

        return input + excited * amount_ * 0.5f;
    }

private:
    static constexpr int kMaxChannels = 2;

    struct ChannelState
    {
        Waveshaper shaper;
        float      lowState = 0.0f;
    };

    void updateCoefficient() noexcept
    {
        const float x = (float) (6.2831853 * (double) crossoverHz_ / sampleRate_);
        coefficient_  = std::clamp(x / (1.0f + x), 0.0f, 1.0f);

        for (auto& channel : channels_)
            channel.shaper.setKind(Waveshaper::Kind::Soft);
    }

    double sampleRate_  = 48000.0;
    float  crossoverHz_ = 3000.0f;
    float  coefficient_ = 0.3f;
    float  amount_      = 0.0f;

    std::array<ChannelState, kMaxChannels> channels_;
};

} // namespace looper::engine
