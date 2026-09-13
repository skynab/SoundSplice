#pragma once

#include <algorithm>
#include <cstdint>

namespace looper::engine
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

} // namespace looper::engine
