#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace soundsplice::engine
{
/**
    Sample-rate conversion for Resample Track: band-limited interpolation with
    a Kaiser-windowed sinc, as Audacity's and REAPER's offline resamplers do.
    Playback's linear interpolation (Interpolation.h) is cheap enough for the
    audio thread but aliases; this is for writing a file once.

    Stateless over the output: output frame k is built from the input around
    k * ratio, so a file can be converted a chunk at a time (inputRangeFor
    says which input frames a chunk reads) and gives exactly what converting
    it whole would. Input outside the file counts as silence.

    Output frame 0 lines up with input frame 0, so every point in the audio
    keeps its time in seconds: a clip's offset, fades and volume curve still
    land where they did. JUCE-free, so the response is tested headless.
*/
class Resampler
{
public:
    /** Zero crossings of the sinc either side of the centre, at the cutoff. */
    static constexpr int kZeroCrossings = 32;

    Resampler(double fromRate, double toRate)
        : ratio_(fromRate > 0.0 && toRate > 0.0 ? fromRate / toRate : 1.0)
    {
        // A little below the lower of the two Nyquists, so the transition band
        // the window leaves falls short of it rather than folding back over it.
        cutoff_    = kPassband * std::min(1.0, 1.0 / ratio_);
        halfWidth_ = (double) kZeroCrossings / cutoff_;
        buildTable();
    }

    /** Input frames per output frame. */
    double ratio() const noexcept { return ratio_; }

    /** How long @p inputFrames at the input rate are at the output rate. */
    std::int64_t outputLength(std::int64_t inputFrames) const noexcept
    {
        return inputFrames <= 0 ? 0 : std::max<std::int64_t>(1, std::llround((double) inputFrames / ratio_));
    }

    /** The input frames [first, end) that output frames [@p outFirst,
        @p outFirst + @p outCount) read. Either end may lie outside the file. */
    void inputRangeFor(std::int64_t outFirst, int outCount, std::int64_t& first, std::int64_t& end) const noexcept
    {
        const double from = (double) outFirst * ratio_;
        const double to   = (double) (outFirst + std::max(outCount, 1) - 1) * ratio_;
        first = (std::int64_t) std::floor(from - halfWidth_);
        end   = (std::int64_t) std::floor(to + halfWidth_) + 2;
    }

    /** Writes output frames [@p outFirst, @p outFirst + @p outCount) of one
        channel to @p out. @p input holds input frames [@p inputFirst,
        @p inputFirst + @p inputCount) of a file @p inputLength frames long;
        frames it doesn't hold, and frames outside the file, are silence. */
    void process(const float* input, std::int64_t inputFirst, int inputCount, std::int64_t inputLength,
                 std::int64_t outFirst, int outCount, float* out) const noexcept
    {
        const std::int64_t heldFirst = std::max<std::int64_t>(inputFirst, 0);
        const std::int64_t heldEnd   = std::min<std::int64_t>(inputFirst + std::max(inputCount, 0), inputLength);
        const double       scale     = (double) kTableSteps * cutoff_;

        for (int k = 0; k < outCount; ++k)
        {
            const double       centre = (double) (outFirst + k) * ratio_;
            const std::int64_t first  = std::max<std::int64_t>((std::int64_t) std::ceil(centre - halfWidth_), heldFirst);
            const std::int64_t end    = std::min<std::int64_t>((std::int64_t) std::floor(centre + halfWidth_) + 1, heldEnd);

            double sum = 0.0;
            for (std::int64_t i = first; i < end; ++i)
            {
                const double position = std::abs(centre - (double) i) * scale;
                const auto   index    = (std::size_t) position;
                if (index + 1 >= table_.size())
                    continue;
                const double fraction = position - (double) index;
                const double weight   = table_[index] + (table_[index + 1] - table_[index]) * fraction;
                sum += weight * input[i - inputFirst];
            }
            out[k] = (float) (sum * cutoff_);
        }
    }

    /** A whole channel converted: for tests and short buffers. */
    std::vector<float> processAll(const std::vector<float>& input) const
    {
        const auto         length = (std::int64_t) input.size();
        std::vector<float> out((std::size_t) outputLength(length));
        process(input.data(), 0, (int) length, length, 0, (int) out.size(), out.data());
        return out;
    }

private:
    static constexpr int    kTableSteps = 512; // table entries per zero crossing
    static constexpr double kPassband   = 0.97;
    static constexpr double kBeta       = 9.0; // Kaiser window: about 90 dB stopband

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        const double half = x * 0.5;
        for (int k = 1; k < 64; ++k)
        {
            term *= (half / k) * (half / k);
            sum += term;
            if (term < sum * 1.0e-12)
                break;
        }
        return sum;
    }

    /** sinc times the window, sampled every 1/kTableSteps of a zero crossing
        from the centre out to the last one, plus one entry to interpolate to. */
    void buildTable()
    {
        const int  size = kZeroCrossings * kTableSteps + 2;
        const auto pi   = 3.14159265358979323846;
        const auto norm = besselI0(kBeta);

        table_.resize((std::size_t) size);
        for (int n = 0; n < size; ++n)
        {
            const double u    = (double) n / kTableSteps;
            const double x    = u / kZeroCrossings;
            const double sinc = n == 0 ? 1.0 : std::sin(pi * u) / (pi * u);
            const double win  = x >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - x * x)) / norm;
            table_[(std::size_t) n] = sinc * win;
        }
    }

    double              ratio_     = 1.0;
    double              cutoff_    = 1.0;
    double              halfWidth_ = 0.0;
    std::vector<double> table_;
};

} // namespace soundsplice::engine
