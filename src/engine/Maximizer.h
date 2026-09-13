#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace looper::engine
{
/**
    A lookahead brickwall limiter — the "loudness maximizer" of a mastering
    chain.

    Two things separate this from the Compressor in PedalDsp.h:

    **Lookahead.** The signal is delayed, while the level detector reads it
    *undelayed*. By the time a peak reaches the output, the gain has already
    been pulled down for it. Without that, any attack time longer than zero
    lets the leading edge of a transient straight through — which on a
    limiter isn't "a bit soft", it's an overshoot past the ceiling, i.e. the
    one thing a brickwall exists to prevent. The cost is latency equal to the
    lookahead, which is why it's short and fixed rather than a control.

    **A ceiling, not a ratio.** Gain reduction is computed so the output lands
    exactly at the ceiling, so nothing above it survives regardless of how far
    in it went.

    Loudness comes from the input gain: driving the signal in and limiting the
    result raises average level while the peak stays pinned. That's the same
    trade every maximizer makes, and it's why `inputGainDb` and `ceilingDb`
    are separate controls rather than one "amount".

    The gain envelope itself is the same one-pole-smoothed-dB shape
    Compressor and Gate use, for consistency and because a stated release time
    should mean the same thing across this codebase. Release only: the attack
    is the lookahead.

    JUCE-free, so the ceiling guarantee is a headless test rather than a
    comment.
*/
class Maximizer
{
public:
    void prepare(double sampleRate, int numChannels = 2)
    {
        sampleRate_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        numChannels_ = std::clamp(numChannels, 1, kMaxChannels);

        lookaheadSamples_ = std::max(1, (int) std::lround(kLookaheadMs * 0.001 * sampleRate_));

        for (auto& channel : delays_)
            channel.assign((size_t) lookaheadSamples_ + 1, 0.0f);

        updateCoefficients();
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : delays_)
            std::fill(channel.begin(), channel.end(), 0.0f);

        writeIndex_       = 0;
        gainDb_           = 0.0f;
        currentReduction_ = 0.0f;
    }

    void setInputGainDb(float db) noexcept { inputGainDb_ = std::clamp(db, 0.0f, 24.0f); }
    void setCeilingDb(float db) noexcept   { ceilingDb_ = std::clamp(db, -24.0f, 0.0f); }

    void setReleaseMs(float ms) noexcept
    {
        releaseMs_ = std::max(1.0f, ms);
        updateCoefficients();
    }

    /** How far the limiter is pulling down right now, in dB (negative). For a
        gain-reduction meter, and for tests. */
    float currentReductionDb() const noexcept { return currentReduction_; }

    /** The latency this introduces, which a caller compensating for it needs
        to know. */
    int latencySamples() const noexcept { return lookaheadSamples_; }

    /** Processes one frame in place. @p samples must have numChannels
        entries. */
    void processFrame(float* samples, int numChannels) noexcept
    {
        const int channels = std::min(numChannels, numChannels_);
        if (channels <= 0 || delays_[0].empty())
            return;

        const float inputGain = std::pow(10.0f, inputGainDb_ / 20.0f);
        const float ceiling   = std::pow(10.0f, ceilingDb_ / 20.0f);

        // One detector fed the loudest channel, so a hard-panned peak doesn't
        // pull the stereo image across as it limits — the same rule
        // CompressorEffect and GateEffect follow.
        float peak = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            peak = std::max(peak, std::abs(samples[ch] * inputGain));

        // The gain that would put this sample exactly on the ceiling. Above
        // the ceiling this is negative; at or below it, zero.
        const float peakDb   = 20.0f * std::log10(std::max(peak, 1.0e-9f));
        const float targetDb = std::min(0.0f, ceilingDb_ - peakDb);

        // Downwards instantly, upwards smoothed. Instant attack is safe
        // *because* of the lookahead: the reduction is applied to samples
        // that haven't been output yet, so there's no click, and the peak it
        // was computed for arrives already handled.
        if (targetDb < gainDb_)
            gainDb_ = targetDb;
        else
            gainDb_ += releaseCoeff_ * (targetDb - gainDb_);

        currentReduction_ = gainDb_;
        const float gain  = std::pow(10.0f, gainDb_ / 20.0f);

        const int size      = (int) delays_[0].size();
        int       readIndex = writeIndex_ - lookaheadSamples_;
        if (readIndex < 0)
            readIndex += size;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto&       line    = delays_[(size_t) ch];
            const float delayed = line[(size_t) readIndex];
            line[(size_t) writeIndex_] = samples[ch] * inputGain;

            // Clamped as well as scaled. The envelope is exact for the peak
            // it was computed from, but a *louder* sample can arrive within
            // the lookahead window while the gain is still releasing from an
            // earlier, quieter one. That's rare and small, and clamping it is
            // what makes "brickwall" true for every input rather than almost
            // every input.
            samples[ch] = std::clamp(delayed * gain, -ceiling, ceiling);
        }

        writeIndex_ = (writeIndex_ + 1) % size;
    }

private:
    static constexpr int   kMaxChannels = 2;
    /** Short enough that the latency is inaudible in practice, long enough to
        catch the leading edge of a real transient. */
    static constexpr float kLookaheadMs = 1.5f;

    void updateCoefficients() noexcept
    {
        const double samples = std::max(1.0, (double) releaseMs_ * 0.001 * sampleRate_);
        releaseCoeff_ = (float) (1.0 - std::exp(-1.0 / samples));
    }

    double sampleRate_       = 48000.0;
    int    numChannels_      = 2;
    int    lookaheadSamples_ = 72;
    int    writeIndex_       = 0;

    float inputGainDb_      = 0.0f;
    float ceilingDb_        = -0.3f;
    float releaseMs_        = 100.0f;
    float releaseCoeff_     = 0.001f;
    float gainDb_           = 0.0f;
    float currentReduction_ = 0.0f;

    std::array<std::vector<float>, kMaxChannels> delays_;
};

} // namespace looper::engine
