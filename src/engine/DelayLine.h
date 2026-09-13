#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace looper::engine
{
/**
    A single-channel feedback delay line. JUCE-free, so the delay/feedback maths
    are unit-tested headlessly. One sample in, the delayed sample out; the written
    sample is input + feedback * delayed.
*/
class DelayLine
{
public:
    void prepare(int maxDelaySamples)
    {
        const int size = std::max(2, maxDelaySamples + 1);
        buffer_.assign((size_t) size, 0.0f);
        writeIndex_ = 0;
    }

    void reset()
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_ = 0;
    }

    float processSample(float input, int delaySamples, float feedback) noexcept
    {
        if (buffer_.empty())
            return input;

        const int size = (int) buffer_.size();
        const int d    = std::clamp(delaySamples, 0, size - 1);

        int readIndex = writeIndex_ - d;
        if (readIndex < 0)
            readIndex += size;

        const float delayed = buffer_[(size_t) readIndex];
        buffer_[(size_t) writeIndex_] = input + feedback * delayed;
        writeIndex_ = (writeIndex_ + 1) % size;
        return delayed;
    }

    /**
        As processSample, but reading at a fractional delay.

        A chorus sweeps its delay continuously. Reading only whole samples
        quantises that sweep, and every step from one sample to the next is a
        discontinuity — a click, on a control whose whole purpose is to be
        smooth. Linear interpolation between the two neighbouring samples is
        enough for a modulation this slow, and is what stops the sweep from
        sounding like a stepped switch.

        The integer path above is left exactly as it was: the existing delay
        effect uses it and its output is a fixed reference.
    */
    float processSampleFractional(float input, double delaySamples, float feedback) noexcept
    {
        if (buffer_.empty())
            return input;

        const int    size    = (int) buffer_.size();
        const double clamped = std::clamp(delaySamples, 0.0, (double) (size - 2));

        const int    whole    = (int) clamped;
        const double fraction = clamped - (double) whole;

        int first = writeIndex_ - whole;
        if (first < 0)
            first += size;

        int second = first - 1;
        if (second < 0)
            second += size;

        const float a = buffer_[(size_t) first];
        const float b = buffer_[(size_t) second];
        const float delayed = a + (float) fraction * (b - a);

        buffer_[(size_t) writeIndex_] = input + feedback * delayed;
        writeIndex_ = (writeIndex_ + 1) % size;
        return delayed;
    }

    /** Reads at a fractional delay without writing or advancing.

        A chorus takes several taps from the same history; giving each its own
        line would store the same samples several times over, since the taps
        differ in where they read and not in what was written. */
    float readFractional(double delaySamples) const noexcept
    {
        if (buffer_.empty())
            return 0.0f;

        const int    size    = (int) buffer_.size();
        const double clamped = std::clamp(delaySamples, 0.0, (double) (size - 2));

        const int    whole    = (int) clamped;
        const double fraction = clamped - (double) whole;

        // One further back than processSampleFractional, because the write
        // index has already advanced past the sample just written.
        int first = writeIndex_ - 1 - whole;
        while (first < 0)
            first += size;

        int second = first - 1;
        if (second < 0)
            second += size;

        const float a = buffer_[(size_t) first];
        const float b = buffer_[(size_t) second];
        return a + (float) fraction * (b - a);
    }

private:
    std::vector<float> buffer_;
    int                writeIndex_ = 0;
};

} // namespace looper::engine
