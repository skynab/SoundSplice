#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace soundsplice::engine
{
/**
    TPDF dither, for reducing a float mix to a fixed word length.

    Quantising without it does not merely add noise - it adds *distortion*.
    The rounding error of an undithered quantiser is correlated with the
    signal, so quiet material gains harmonics that were never played, and
    anything below one LSB disappears in a way that depends on its own shape.
    That is why fades and reverb tails are where truncation is heard first:
    they are exactly the parts that spend their time near the bottom of the
    word.

    Adding a small amount of noise *before* rounding decorrelates the error
    from the signal. What is left is a steady, signal-independent hiss about
    5dB louder than truncation's, and in exchange the material underneath the
    LSB survives - a tone quieter than one quantisation step is still there
    afterwards, carried in the average. That trade is why every mastering
    chain ends with it.

    **Triangular** PDF specifically, made by summing two independent uniform
    values. A single uniform (RPDF) leaves the *variance* of the error
    modulating with the signal - audible as noise that breathes with the
    music - and triangular is the cheapest distribution that removes that too.
    Peak amplitude is one LSB either side.

    JUCE-free so that what it does is a headless, measurable claim rather than
    something you have to listen for; and deterministic from a seed, so two
    exports of the same mix produce the same file.
*/
class TpdfDither
{
public:
    /** @p bitsPerSample is the depth being written - 16 or 24. The noise is
        scaled to that word's LSB, so the same object is correct for either. */
    explicit TpdfDither(int bitsPerSample, uint32_t seed = 0x9E3779B9u) noexcept
    {
        setBitsPerSample(bitsPerSample);
        reset(seed);
    }

    void setBitsPerSample(int bitsPerSample) noexcept
    {
        // A sample is written as a signed integer of this width, so full scale
        // is 2^(bits-1) steps and one LSB is its reciprocal.
        const int bits = std::clamp(bitsPerSample, 2, 32);
        lsb_ = 1.0f / (float) ((int64_t) 1 << (bits - 1));
    }

    /** One LSB at the current depth - the peak amplitude of the noise added. */
    float lsb() const noexcept { return lsb_; }

    void reset(uint32_t seed = 0x9E3779B9u) noexcept
    {
        // Never zero: xorshift is stuck at zero forever.
        state_ = seed != 0 ? seed : 0x9E3779B9u;
    }

    /**
        @p input plus one TPDF-distributed LSB of noise.

        Does not round: the writer quantises, and doing it here as well would
        quantise twice. This only adds the noise that makes the writer's own
        rounding well behaved.
    */
    float processSample(float input) noexcept
    {
        // Two independent uniforms in [0,1), summed as a difference so the
        // result is triangular on (-1,1) and has zero mean - a non-zero mean
        // would be a DC offset added to the whole export.
        const float a = nextUniform();
        const float b = nextUniform();
        return input + (a - b) * lsb_;
    }

private:
    /** xorshift32: no allocation, no locks, and the same sequence on every
        platform - which is what makes an export reproducible. Its statistical
        quality is far beyond what dither needs. */
    float nextUniform() noexcept
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;

        // Top 24 bits, so the value is exactly representable as a float.
        return (float) (state_ >> 8) * (1.0f / 16777216.0f);
    }

    float    lsb_   = 1.0f / 32768.0f;
    uint32_t state_ = 0x9E3779B9u;
};

/**
    Noise-shaped dither: TPDF dither whose requantisation error is fed back
    through a filter, so the noise moves out of the 1-5 kHz region the ear is
    most sensitive to and up towards Nyquist, where it's far less audible.
    Worth most for a 16-bit master; more total noise, heard as less.

    Unlike TpdfDither this quantises itself - the feedback needs the error,
    and the error needs the rounding - so what it returns is already on the
    word's grid, and the writer's own conversion leaves it exactly as it is.

    At 44.1 and 48 kHz the filter is Wannamaker's nine-tap F-weighted
    ("Psychoacoustically optimal noise shaping", JAES 1992), designed against
    the ear's threshold curve. Above that the curve's shape is mostly beyond
    hearing anyway, and a plain second-order high-pass shape is used.

    One per channel: the feedback is the channel's own history.
*/
class NoiseShapedDither
{
public:
    NoiseShapedDither(int bitsPerSample, double sampleRate, uint32_t seed = 0x9E3779B9u) noexcept
        : tpdf_(bitsPerSample, seed)
    {
        static constexpr double kFWeighted[] { 2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847 };
        static constexpr double kSecondOrder[] { 2.0, -1.0 };
        if (sampleRate < 50000.0)
            coefficients_.assign(std::begin(kFWeighted), std::end(kFWeighted));
        else
            coefficients_.assign(std::begin(kSecondOrder), std::end(kSecondOrder));
        errors_.assign(coefficients_.size(), 0.0);
    }

    float lsb() const noexcept { return tpdf_.lsb(); }

    /** @p input, shaped, dithered and rounded to the word's grid. */
    float processSample(float input) noexcept
    {
        const double lsb = tpdf_.lsb();

        double feedback = 0.0;
        for (size_t k = 0; k < coefficients_.size(); ++k)
            feedback += coefficients_[k] * errors_[k];
        const double shaped = (double) input - feedback;

        const double dithered = (double) tpdf_.processSample((float) shaped);
        const double top      = 1.0 - lsb;
        const double out      = std::clamp(std::round(dithered / lsb) * lsb, -1.0, top);

        // Held to a few LSBs: when the output clips, the error is the clip,
        // not noise, and feeding that back would ring the filter.
        const double error = std::clamp(out - shaped, -4.0 * lsb, 4.0 * lsb);
        for (size_t k = errors_.size() - 1; k > 0; --k)
            errors_[k] = errors_[k - 1];
        errors_[0] = error;
        return (float) out;
    }

private:
    TpdfDither          tpdf_;
    std::vector<double> coefficients_;
    std::vector<double> errors_;
};

} // namespace soundsplice::engine
