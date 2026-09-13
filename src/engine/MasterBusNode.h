#pragma once

#include <atomic>
#include <cmath>

#include "engine/Node.h"

namespace looper::engine
{
/**
    The master bus: applies a smoothed output gain and measures per-channel peak
    for the meters. Peaks are published into atomics that the UI reads on a timer
    (continuous level readout → atomics, not the command FIFO).
*/
class MasterBusNode final : public Node
{
public:
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        gain_.reset(sampleRate, 0.02);
    }

    void setGainDb(float db) noexcept { gainDb_ = db; }

    /** Read by the UI thread. */
    float peak(int channel) const noexcept
    {
        return (channel >= 0 && channel < 2)
            ? channelPeak_[channel].load(std::memory_order_relaxed)
            : 0.0f;
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/, const ProcessContext& /*context*/) override
    {
        gain_.setTargetValue(juce::Decibels::decibelsToGain(gainDb_));

        const int numSamples  = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gain_.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer(ch)[i] *= g;
        }

        for (int ch = 0; ch < numChannels && ch < 2; ++ch)
        {
            float peak = 0.0f;
            const float* data = buffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                peak = std::max(peak, std::abs(data[i]));

            channelPeak_[ch].store(peak, std::memory_order_relaxed);
        }
    }

private:
    float gainDb_ = 0.0f;
    juce::LinearSmoothedValue<float> gain_ { 1.0f };
    std::atomic<float> channelPeak_[2] {};
};

} // namespace looper::engine
