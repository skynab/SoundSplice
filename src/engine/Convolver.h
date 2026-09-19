#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine
{
/**
    Convolution with an impulse response, for the convolution reverb: uniform
    partitioned overlap-save. The response is cut into blocks of kBlock
    samples, each held as its spectrum; every kBlock samples of input are
    transformed once and multiplied against every block's spectrum, delayed
    by that block's place, and summed. So a response seconds long costs a
    multiply-add per partition per bin, rather than per sample of response.

    Output lags input by kBlock samples (about 5 ms at 48 kHz): a block has
    to be complete before its transform. For a reverb that's a sliver of
    pre-delay, and the dry signal isn't delayed at all.

    Everything is allocated by setImpulse; processSample allocates nothing,
    so a Convolver made on the message thread can be handed to the audio
    thread whole. JUCE-free, so it's tested headless against direct
    convolution.
*/
class Convolver
{
public:
    static constexpr int kBlock = 256;
    static constexpr int kSize  = kBlock * 2;
    static constexpr int kBins  = kBlock + 1;

    /** @p impulse, which may be empty (silence out). */
    void setImpulse(const std::vector<float>& impulse)
    {
        partitions_ = std::max<int>(1, (int) ((impulse.size() + kBlock - 1) / kBlock));
        filterRe_.assign((size_t) partitions_ * kBins, 0.0f);
        filterIm_.assign((size_t) partitions_ * kBins, 0.0f);
        historyRe_.assign((size_t) partitions_ * kBins, 0.0f);
        historyIm_.assign((size_t) partitions_ * kBins, 0.0f);
        re_.assign((size_t) kSize, 0.0f);
        im_.assign((size_t) kSize, 0.0f);
        accRe_.assign((size_t) kBins, 0.0f);
        accIm_.assign((size_t) kBins, 0.0f);
        window_.assign((size_t) kSize, 0.0f); // the last two blocks of input
        output_.assign((size_t) kBlock, 0.0f);
        position_ = 0;
        newest_   = 0;

        for (int p = 0; p < partitions_; ++p)
        {
            std::fill(re_.begin(), re_.end(), 0.0f);
            std::fill(im_.begin(), im_.end(), 0.0f);
            for (int i = 0; i < kBlock; ++i)
            {
                const size_t at = (size_t) p * kBlock + (size_t) i;
                re_[(size_t) i] = at < impulse.size() ? impulse[at] : 0.0f;
            }
            fft::transform(re_, im_, false);
            std::copy(re_.begin(), re_.begin() + kBins, filterRe_.begin() + (long) p * kBins);
            std::copy(im_.begin(), im_.begin() + kBins, filterIm_.begin() + (long) p * kBins);
        }
    }

    bool isReady() const noexcept { return ! filterRe_.empty(); }

    /** One sample in, the output kBlock samples behind it out. */
    float processSample(float x) noexcept
    {
        if (filterRe_.empty())
            return 0.0f;

        const float out = output_[(size_t) position_];
        window_[(size_t) (kBlock + position_)] = x;
        if (++position_ == kBlock)
        {
            position_ = 0;
            processBlock();
        }
        return out;
    }

    void reset() noexcept
    {
        std::fill(historyRe_.begin(), historyRe_.end(), 0.0f);
        std::fill(historyIm_.begin(), historyIm_.end(), 0.0f);
        std::fill(window_.begin(), window_.end(), 0.0f);
        std::fill(output_.begin(), output_.end(), 0.0f);
        position_ = 0;
    }

private:
    void processBlock() noexcept
    {
        // The newest input block, with the one before it: overlap-save.
        std::copy(window_.begin(), window_.end(), re_.begin());
        std::fill(im_.begin(), im_.end(), 0.0f);
        fft::transform(re_, im_, false);

        newest_ = (newest_ + partitions_ - 1) % partitions_;
        std::copy(re_.begin(), re_.begin() + kBins, historyRe_.begin() + (long) newest_ * kBins);
        std::copy(im_.begin(), im_.begin() + kBins, historyIm_.begin() + (long) newest_ * kBins);

        // Partition p meets the input from p blocks ago.
        std::fill(accRe_.begin(), accRe_.end(), 0.0f);
        std::fill(accIm_.begin(), accIm_.end(), 0.0f);
        for (int p = 0; p < partitions_; ++p)
        {
            const size_t h = (size_t) ((newest_ + p) % partitions_) * kBins;
            const size_t f = (size_t) p * kBins;
            for (int k = 0; k < kBins; ++k)
            {
                const float xr = historyRe_[h + (size_t) k], xi = historyIm_[h + (size_t) k];
                const float fr = filterRe_[f + (size_t) k], fi = filterIm_[f + (size_t) k];
                accRe_[(size_t) k] += xr * fr - xi * fi;
                accIm_[(size_t) k] += xr * fi + xi * fr;
            }
        }

        // Back to time, the spectrum of a real signal: the upper half mirrors.
        for (int k = 0; k < kBins; ++k)
        {
            re_[(size_t) k] = accRe_[(size_t) k];
            im_[(size_t) k] = accIm_[(size_t) k];
        }
        for (int k = 1; k < kBlock; ++k)
        {
            re_[(size_t) (kSize - k)] = accRe_[(size_t) k];
            im_[(size_t) (kSize - k)] = -accIm_[(size_t) k];
        }
        fft::transform(re_, im_, true);

        // The second half is the valid part.
        std::copy(re_.begin() + kBlock, re_.end(), output_.begin());
        std::copy(window_.begin() + kBlock, window_.end(), window_.begin());
    }

    int                partitions_ = 0;
    std::vector<float> filterRe_, filterIm_;   // each partition's spectrum
    std::vector<float> historyRe_, historyIm_; // the input's last `partitions_` spectra, a ring
    std::vector<float> re_, im_, accRe_, accIm_;
    std::vector<float> window_, output_;
    int                position_ = 0;
    int                newest_   = 0;
};

namespace convolution
{
    /** Scales @p channels of an impulse response so a steady broadband
        sound comes out as loud as it went in: the reverb's level is then set
        by its mix, not by how the response happened to be recorded. */
    inline void normalise(std::vector<std::vector<float>>& channels)
    {
        double energy = 0.0;
        size_t count  = 0;
        for (const auto& channel : channels)
        {
            for (float s : channel)
                energy += (double) s * s;
            ++count;
        }
        if (energy <= 1.0e-20 || count == 0)
            return;
        const float scale = (float) (1.0 / std::sqrt(energy / (double) count));
        for (auto& channel : channels)
            for (auto& s : channel)
                s *= scale;
    }

    /** A plain hall to use until a response is loaded: noise dying away by
        60 dB over @p seconds, a different noise in each channel so it's wide,
        with a few milliseconds' fade in. Normalised. */
    inline std::vector<std::vector<float>> syntheticHall(double sampleRate, double seconds = 1.8)
    {
        const int                       length = std::max(1, (int) (sampleRate * seconds));
        std::vector<std::vector<float>> channels(2, std::vector<float>((size_t) length));
        for (int ch = 0; ch < 2; ++ch)
        {
            std::mt19937                          random(1234u + (unsigned) ch);
            std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
            float                                 smoothed = 0.0f;
            for (int i = 0; i < length; ++i)
            {
                const double t     = (double) i / sampleRate;
                const double decay = std::exp(-6.908 * t / seconds); // -60 dB at the end
                const double fade  = std::min(1.0, t / 0.004);
                // A gentle low-pass that darkens as it decays, as air does.
                const float  pole  = (float) std::min(0.9, 0.2 + 0.6 * t / seconds);
                smoothed += (1.0f - pole) * (uniform(random) - smoothed);
                channels[(size_t) ch][(size_t) i] = (float) (smoothed * decay * fade);
            }
        }
        normalise(channels);
        return channels;
    }
}

} // namespace soundsplice::engine
