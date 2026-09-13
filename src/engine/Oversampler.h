#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace looper::engine
{
/**
    4x oversampling around a nonlinearity.

    A waveshaper generates harmonics without limit. Any of them above Nyquist
    fold back down as inharmonic aliases, and unlike the harmonics themselves
    they are not related to the note being played, so they read as a metallic
    grit that gets worse the harder the pedal is driven. Running the shaper at
    four times the rate moves the fold-back point four times higher, so the
    harmonics that would have aliased land in a band that is filtered away on
    the way back down instead.

    `Waveshaper`'s ADAA already buys a measured 4.5-7dB of this
    (see WaveshaperTests), which is enough for a single moderate stage. It is
    not enough for the high-gain presets, which cascade a boost into an amp
    with no filtering between them: the first stage's near-Nyquist output is
    fed straight back into a second nonlinearity and folded again.

    ADAA and oversampling are complementary rather than alternatives - ADAA
    suppresses the aliasing at the source, oversampling moves what remains out
    of the audible band - so this wraps the shaper rather than replacing it.

    **Structure.** A 96-tap Blackman-windowed sinc, used polyphase going up
    and direct going down. 96 taps puts the transition band between roughly
    18.5kHz and 29kHz at a 48kHz base rate: the whole audible band passes, and
    the stopband is about 74dB down. JUCE-free, so the aliasing reduction is a
    headless measurement rather than a claim.
*/
class Oversampler4x
{
public:
    static constexpr int kFactor    = 4;
    static constexpr int kNumTaps   = 96;
    static constexpr int kPhaseTaps = kNumTaps / kFactor;

    void reset() noexcept
    {
        upHistory_.fill(0.0f);
        downHistory_.fill(0.0f);
        upIndex_   = 0;
        downIndex_ = 0;
    }

    /**
        Runs @p shape at four times the sample rate and returns one output
        sample.

        Takes the nonlinearity as a callable rather than owning one, so the
        caller keeps whatever state it has (a `Waveshaper`'s ADAA history, for
        instance) and this stays a pure rate converter. The callable is
        invoked exactly `kFactor` times per call, in order.
    */
    template <typename Shape>
    float process(float input, Shape&& shape) noexcept
    {
        // Up: one input sample becomes four, by evaluating the four polyphase
        // subfilters against the same history. Equivalent to inserting three
        // zeros and filtering, without doing the multiplications by zero.
        upHistory_[(size_t) upIndex_] = input;

        for (int phase = 0; phase < kFactor; ++phase)
        {
            float acc = 0.0f;
            int   pos = upIndex_;

            for (int tap = 0; tap < kPhaseTaps; ++tap)
            {
                acc += coefficients()[(size_t) (tap * kFactor + phase)] * upHistory_[(size_t) pos];
                pos = (pos == 0) ? kPhaseTaps - 1 : pos - 1;
            }

            // Zero-stuffing divides the signal's energy by the factor, so the
            // interpolation filter has to put it back.
            oversampled_[(size_t) phase] = acc * (float) kFactor;
        }

        upIndex_ = (upIndex_ + 1) % kPhaseTaps;

        for (int phase = 0; phase < kFactor; ++phase)
            oversampled_[(size_t) phase] = shape(oversampled_[(size_t) phase]);

        // Down: filter, then keep one sample in four. Direct form, since the
        // filter only has to be evaluated on the sample actually kept - the
        // three that get thrown away cost nothing.
        for (int phase = 0; phase < kFactor; ++phase)
        {
            downHistory_[(size_t) downIndex_] = oversampled_[(size_t) phase];
            downIndex_ = (downIndex_ + 1) % kNumTaps;
        }

        float acc = 0.0f;
        int   pos = (downIndex_ == 0) ? kNumTaps - 1 : downIndex_ - 1;

        for (int tap = 0; tap < kNumTaps; ++tap)
        {
            acc += coefficients()[(size_t) tap] * downHistory_[(size_t) pos];
            pos = (pos == 0) ? kNumTaps - 1 : pos - 1;
        }

        return acc;
    }

private:
    /** The shared prototype lowpass: cutoff at a quarter of the oversampled
        Nyquist, which is the base rate's Nyquist. Built once on first use -
        it depends on nothing but the tap count, so every instance and every
        sample rate uses the same table. */
    static const std::array<float, kNumTaps>& coefficients()
    {
        static const std::array<float, kNumTaps> taps = []
        {
            constexpr double pi = 3.14159265358979323846;
            std::array<float, kNumTaps> h {};

            // Normalised cutoff in cycles/sample at the oversampled rate.
            // 1/(2*kFactor) is exactly the base rate's Nyquist.
            const double cutoff = 0.5 / (double) kFactor;
            const double centre = 0.5 * (double) (kNumTaps - 1);

            double sum = 0.0;
            for (int n = 0; n < kNumTaps; ++n)
            {
                const double t    = (double) n - centre;
                const double sinc = (std::abs(t) < 1.0e-9)
                                        ? 2.0 * cutoff
                                        : std::sin(2.0 * pi * cutoff * t) / (pi * t);

                // Blackman: -74dB stopband, which is well under the noise
                // floor of anything this feeds.
                const double ratio  = (double) n / (double) (kNumTaps - 1);
                const double window = 0.42 - 0.5 * std::cos(2.0 * pi * ratio)
                                           + 0.08 * std::cos(4.0 * pi * ratio);

                const double value = sinc * window;
                h[(size_t) n] = (float) value;
                sum += value;
            }

            // Normalised to unity DC gain, so wrapping a nonlinearity in this
            // does not change the level of what comes out of it.
            for (auto& tap : h)
                tap = (float) ((double) tap / sum);

            return h;
        }();

        return taps;
    }

    std::array<float, kPhaseTaps> upHistory_ {};
    std::array<float, kNumTaps>   downHistory_ {};
    std::array<float, kFactor>    oversampled_ {};

    int upIndex_   = 0;
    int downIndex_ = 0;
};

} // namespace looper::engine
