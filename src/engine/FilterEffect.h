#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/StateVariableFilter.h"

namespace looper::engine
{
/**
    A stereo master filter built on two StateVariableFilters. Parameters are
    atomics set from the message thread and read on the audio thread; coefficients
    are refreshed once per block. Passes audio through untouched when disabled.
    Mode: 0 = low-pass, 1 = high-pass, 2 = band-pass.
*/
class FilterEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/)
    {
        left_.prepare(sampleRate);
        right_.prepare(sampleRate);
    }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setCutoff(float hz)       { cutoff_.store(hz, std::memory_order_relaxed); }
    void setResonance(float q)     { resonance_.store(q, std::memory_order_relaxed); }
    void setMode(int mode)         { mode_.store(mode, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const auto  mode      = (StateVariableFilter::Mode) mode_.load(std::memory_order_relaxed);
        const float cutoff    = cutoff_.load(std::memory_order_relaxed);
        const float resonance = resonance_.load(std::memory_order_relaxed);

        for (auto* svf : { &left_, &right_ })
        {
            svf->setMode(mode);
            svf->setCutoff(cutoff);
            svf->setResonance(resonance);
        }

        const int numSamples  = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        for (int i = 0; i < numSamples; ++i)
        {
            if (numChannels > 0) buffer.setSample(0, i, left_.processSample(buffer.getSample(0, i)));
            if (numChannels > 1) buffer.setSample(1, i, right_.processSample(buffer.getSample(1, i)));
        }
    }

private:
    StateVariableFilter left_, right_;
    std::atomic<bool>   enabled_   { false };
    std::atomic<float>  cutoff_    { 1000.0f };
    std::atomic<float>  resonance_ { 0.707f };
    std::atomic<int>    mode_      { 0 };
};

} // namespace looper::engine
