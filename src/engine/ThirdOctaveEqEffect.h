#pragma once

#include <array>
#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ThirdOctaveEq.h"

namespace soundsplice::engine
{
/** The 31-band graphic EQ as a chain node. */
class ThirdOctaveEqEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (auto& channel : channels_)
            channel.prepare(sampleRate);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void setBandDb(int band, float db)
    {
        if (band >= 0 && band < ThirdOctaveEq::kBands)
            gains_[(size_t) band].store(db, std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& eq = channels_[(size_t) ch];
            for (int b = 0; b < ThirdOctaveEq::kBands; ++b)
                eq.setGainDb(b, gains_[(size_t) b].load(std::memory_order_relaxed));

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = eq.processSample(samples[n]);
        }
    }

private:
    std::array<ThirdOctaveEq, 2>                                channels_;
    std::array<std::atomic<float>, ThirdOctaveEq::kBands>       gains_ {};
    std::atomic<bool>                                           enabled_ { false };
};

} // namespace soundsplice::engine
