#pragma once

#include <atomic>
#include <cmath>

#include "engine/Loudness.h"
#include "engine/Node.h"

namespace soundsplice::engine
{
/**
    The master bus: applies a smoothed output gain and measures per-channel peak
    for the meters, and loudness (engine/Loudness.h) for the loudness meter.
    Readings are published into atomics that the UI reads on a timer
    (continuous level readout → atomics, not the command FIFO).

    Loudness is measured only on what reaches the device: an offline render
    runs through here too, and an export shouldn't count as listening.
*/
class MasterBusNode final : public Node
{
public:
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        gain_.reset(sampleRate, 0.02);

        // Every export re-prepares the engine, twice; only a new rate should
        // throw away the integrated loudness measured so far.
        if (! loudness_.isPrepared() || loudness_.sampleRate() != sampleRate)
        {
            loudness_.prepare(sampleRate, 2);
            publishLoudness(true);
        }
    }

    /** Asks the audio thread to start the loudness measurement afresh. */
    void resetLoudness() noexcept { loudnessResetRequested_.store(true, std::memory_order_relaxed); }

    /** Read by the UI thread. */
    LiveLoudness loudness() const noexcept
    {
        LiveLoudness out;
        out.momentaryLufs   = momentary_.load(std::memory_order_relaxed);
        out.shortTermLufs   = shortTerm_.load(std::memory_order_relaxed);
        out.integratedLufs  = integrated_.load(std::memory_order_relaxed);
        out.loudnessRangeLu = range_.load(std::memory_order_relaxed);
        out.truePeakDb      = truePeak_.load(std::memory_order_relaxed);
        return out;
    }

    void setGainDb(float db) noexcept { gainDb_ = db; }

    /** Read by the UI thread. */
    float peak(int channel) const noexcept
    {
        return (channel >= 0 && channel < 2)
            ? channelPeak_[channel].load(std::memory_order_relaxed)
            : 0.0f;
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/, const ProcessContext& context) override
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

        if (! context.offline && numChannels > 0 && loudness_.isPrepared())
        {
            const bool reset = loudnessResetRequested_.exchange(false, std::memory_order_relaxed);
            if (reset)
                loudness_.reset();

            const float* channels[2] { buffer.getReadPointer(0), buffer.getReadPointer(juce::jmin(1, numChannels - 1)) };
            const auto   hops = loudness_.hopCount();
            loudness_.process(channels, 2, numSamples);
            publishLoudness(reset || loudness_.hopCount() != hops);
        }
    }

private:
    /** The integrated loudness and range take a pass over their histograms,
        so they're only worked out again when another 100 ms has gone by. */
    void publishLoudness(bool includingIntegrated) noexcept
    {
        momentary_.store(loudness_.momentaryLufs(), std::memory_order_relaxed);
        shortTerm_.store(loudness_.shortTermLufs(), std::memory_order_relaxed);
        truePeak_.store(loudness_.truePeakDb(), std::memory_order_relaxed);
        if (includingIntegrated)
        {
            integrated_.store(loudness_.integratedLufs(), std::memory_order_relaxed);
            range_.store(loudness_.loudnessRangeLu(), std::memory_order_relaxed);
        }
    }

    LoudnessMeter       loudness_;
    std::atomic<bool>   loudnessResetRequested_ { false };
    std::atomic<double> momentary_ { LoudnessMeter::kSilence };
    std::atomic<double> shortTerm_ { LoudnessMeter::kSilence };
    std::atomic<double> integrated_ { LoudnessMeter::kSilence };
    std::atomic<double> range_ { 0.0 };
    std::atomic<double> truePeak_ { LoudnessMeter::kSilence };

    float gainDb_ = 0.0f;
    juce::LinearSmoothedValue<float> gain_ { 1.0f };
    std::atomic<float> channelPeak_[2] {};
};

} // namespace soundsplice::engine
