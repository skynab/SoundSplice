#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace soundsplice
{
/**
    A min/max peak summary of decoded audio, for drawing waveforms.

    Replaces juce::AudioThumbnail in the audio editor for three things a
    thumbnail can't do:

      - **Show the gain that's actually applied.** A thumbnail draws the file
        on disk. The clip's gainDb is applied at playback, so turning it down
        or pressing Normalize left the waveform pixel-identical — the edit was
        real and completely invisible, which is the problem this exists to
        fix. Peaks are stored un-gained and scaled when drawn, so one cache
        serves any gain.
      - **Show what would clip.** Knowing a peak's actual value lets the view
        mark where `sample * gain` passes full scale, which is the single most
        useful thing to see while setting a level.
      - **Show the original underneath.** Drawing the same peaks at gain 1
        behind the gained ones is what makes an edit legible as a change
        rather than just a different-looking waveform.

    JUCE-free so the bucketing and range maths are unit-tested headlessly —
    off-by-ones here are the difference between a waveform that lines up with
    the selection and one that doesn't, which is very hard to judge by eye.
*/
struct PeakBin
{
    float minimum = 0.0f;
    float maximum = 0.0f;

    /** The larger absolute excursion — what "how loud is it here" means when
        a single number is wanted. */
    float magnitude() const { return std::max(std::abs(minimum), std::abs(maximum)); }

    bool isEmpty() const { return minimum == 0.0f && maximum == 0.0f; }
};

class WaveformPeaks
{
public:
    /** Samples summarised into each bin. 64 keeps a five-minute stereo file
        under about 8MB while still giving one bin per pixel at roughly a
        second across a 1000px view — fine enough that transients stay
        visible at any zoom anyone edits at. */
    static constexpr int kDefaultSamplesPerBin = 64;

    WaveformPeaks() = default;

    /** Builds from per-channel sample vectors. */
    void build(const std::vector<std::vector<float>>& channels,
               int samplesPerBin = kDefaultSamplesPerBin)
    {
        clear();
        samplesPerBin_ = std::max(1, samplesPerBin);

        bins_.resize(channels.size());
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            const auto& samples = channels[ch];
            totalSamples_ = std::max(totalSamples_, (int) samples.size());

            const int binCount = ((int) samples.size() + samplesPerBin_ - 1) / samplesPerBin_;
            bins_[ch].resize((size_t) std::max(0, binCount));

            for (int b = 0; b < binCount; ++b)
            {
                const int from = b * samplesPerBin_;
                const int to   = std::min((int) samples.size(), from + samplesPerBin_);

                float lowest  = samples[(size_t) from];
                float highest = samples[(size_t) from];
                for (int i = from + 1; i < to; ++i)
                {
                    lowest  = std::min(lowest, samples[(size_t) i]);
                    highest = std::max(highest, samples[(size_t) i]);
                }
                bins_[ch][(size_t) b] = { lowest, highest };
            }
        }
    }

    /** Adds @p chunk (per channel, the samples that follow what's already
        summarised) to the end, exactly as if it had been part of one build.
        A long clip is summarised a chunk at a time this way, so drawing it
        never needs the whole recording in memory. A chunk needn't end on a
        bin boundary: the next one fills out the last bin first. */
    void append(const std::vector<std::vector<float>>& chunk)
    {
        if (chunk.empty() || chunk[0].empty())
            return;

        if (bins_.empty())
            bins_.resize(chunk.size());

        const int filled      = totalSamples_ % samplesPerBin_; // already in the last, partial bin
        const int chunkLength = (int) chunk[0].size();

        for (size_t ch = 0; ch < bins_.size(); ++ch)
        {
            const auto& samples = chunk[std::min(ch, chunk.size() - 1)];
            const int   length  = std::min(chunkLength, (int) samples.size());
            auto&       bins    = bins_[ch];

            int i = 0;
            if (filled > 0 && ! bins.empty())
            {
                auto& last = bins.back();
                for (; i < std::min(length, samplesPerBin_ - filled); ++i)
                {
                    last.minimum = std::min(last.minimum, samples[(size_t) i]);
                    last.maximum = std::max(last.maximum, samples[(size_t) i]);
                }
            }

            while (i < length)
            {
                const int to = std::min(length, i + samplesPerBin_);

                PeakBin bin { samples[(size_t) i], samples[(size_t) i] };
                for (int j = i + 1; j < to; ++j)
                {
                    bin.minimum = std::min(bin.minimum, samples[(size_t) j]);
                    bin.maximum = std::max(bin.maximum, samples[(size_t) j]);
                }
                bins.push_back(bin);
                i = to;
            }
        }

        totalSamples_ += chunkLength;
    }

    void clear()
    {
        bins_.clear();
        totalSamples_  = 0;
        samplesPerBin_ = kDefaultSamplesPerBin;
    }

    bool isEmpty() const { return bins_.empty() || totalSamples_ <= 0; }
    int  numChannels() const { return (int) bins_.size(); }
    int  totalSamples() const { return totalSamples_; }
    int  samplesPerBin() const { return samplesPerBin_; }

    /**
        The combined min/max over [fromSample, toSample) of @p channel.

        Reduced from the bins that overlap the range rather than from the
        samples, which is the whole point of the cache: a view 1000px wide
        over a five-minute file asks this a thousand times per repaint, and
        rescanning millions of samples per frame would make scrolling
        unusable.

        A range shorter than one bin still returns that bin — better to
        over-report slightly than to draw nothing where there is audio, since
        a gap in a waveform reads as silence.
    */
    PeakBin range(int channel, int fromSample, int toSample) const
    {
        if (channel < 0 || channel >= (int) bins_.size() || bins_[(size_t) channel].empty())
            return {};

        const auto& bins     = bins_[(size_t) channel];
        const int   binCount = (int) bins.size();

        const int first = std::clamp(fromSample / samplesPerBin_, 0, binCount - 1);
        const int last  = std::clamp((toSample - 1) / samplesPerBin_, first, binCount - 1);

        PeakBin result = bins[(size_t) first];
        for (int b = first + 1; b <= last; ++b)
        {
            result.minimum = std::min(result.minimum, bins[(size_t) b].minimum);
            result.maximum = std::max(result.maximum, bins[(size_t) b].maximum);
        }
        return result;
    }

    /** The loudest excursion anywhere in the file — what a "this will clip at
        the current gain" warning is judged against. */
    float overallMagnitude() const
    {
        float peak = 0.0f;
        for (const auto& channel : bins_)
            for (const auto& bin : channel)
                peak = std::max(peak, bin.magnitude());
        return peak;
    }

private:
    std::vector<std::vector<PeakBin>> bins_;
    int                               totalSamples_  = 0;
    int                               samplesPerBin_ = kDefaultSamplesPerBin;
};

} // namespace soundsplice
