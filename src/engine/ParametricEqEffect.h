#pragma once

#include <array>
#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ParametricEq.h"

namespace soundsplice::engine
{
/** The parametric EQ as a chain node: the bands come in from the message
    thread through atomics and are applied at the start of each block. */
class ParametricEqEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (auto& channel : channels_)
            channel.prepare(sampleRate);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void setBand(int index, const ParametricBand& band)
    {
        if (index < 0 || index >= ParametricEq::kBands)
            return;
        auto& shared = shared_[(size_t) index];
        shared.type.store((int) band.type, std::memory_order_relaxed);
        shared.hz.store(band.hz, std::memory_order_relaxed);
        shared.gainDb.store(band.gainDb, std::memory_order_relaxed);
        shared.q.store(band.q, std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        ParametricEq::Bands bands;
        for (int b = 0; b < ParametricEq::kBands; ++b)
        {
            const auto& shared = shared_[(size_t) b];
            bands[(size_t) b].type   = (ParametricBand::Type) juce::jlimit(0, 6, shared.type.load(std::memory_order_relaxed));
            bands[(size_t) b].hz     = shared.hz.load(std::memory_order_relaxed);
            bands[(size_t) b].gainDb = shared.gainDb.load(std::memory_order_relaxed);
            bands[(size_t) b].q      = shared.q.load(std::memory_order_relaxed);
        }

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& eq = channels_[(size_t) ch];
            for (int b = 0; b < ParametricEq::kBands; ++b)
                eq.setBand(b, bands[(size_t) b]);

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = eq.processSample(samples[n]);
        }
    }

private:
    struct SharedBand
    {
        std::atomic<int>   type { 0 };
        std::atomic<float> hz { 1000.0f };
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> q { 1.0f };
    };

    std::array<ParametricEq, 2>                         channels_;
    std::array<SharedBand, ParametricEq::kBands>        shared_;
    std::atomic<bool>                                   enabled_ { false };
};

} // namespace soundsplice::engine
