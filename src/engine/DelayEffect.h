#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DelayLine.h"

namespace looper::engine
{
/**
    A stereo feedback delay built on two DelayLines. Parameters are atomics set
    from the message thread and read on the audio thread. Applied on the master
    bus; when disabled it passes audio through untouched.
*/
class DelayEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/)
    {
        sampleRate_ = sampleRate;
        const int maxDelay = (int) (sampleRate * 2.0); // up to 2 s
        left_.prepare(maxDelay);
        right_.prepare(maxDelay);
    }

    void setEnabled(bool enabled)   { enabled_.store(enabled, std::memory_order_relaxed); }
    void setTimeMs(float ms)        { timeMs_.store(ms, std::memory_order_relaxed); }
    void setFeedback(float amount)  { feedback_.store(juce::jlimit(0.0f, 0.95f, amount), std::memory_order_relaxed); }
    void setMix(float amount)       { mix_.store(juce::jlimit(0.0f, 1.0f, amount), std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed) || sampleRate_ <= 0.0)
            return;

        const int   delaySamples = (int) (timeMs_.load(std::memory_order_relaxed) * 0.001 * sampleRate_);
        const float feedback     = feedback_.load(std::memory_order_relaxed);
        const float mix          = mix_.load(std::memory_order_relaxed);
        const int   numSamples   = buffer.getNumSamples();
        const int   numChannels  = buffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            if (numChannels > 0)
            {
                const float dry = buffer.getSample(0, i);
                const float wet = left_.processSample(dry, delaySamples, feedback);
                buffer.setSample(0, i, dry * (1.0f - mix) + wet * mix);
            }
            if (numChannels > 1)
            {
                const float dry = buffer.getSample(1, i);
                const float wet = right_.processSample(dry, delaySamples, feedback);
                buffer.setSample(1, i, dry * (1.0f - mix) + wet * mix);
            }
        }
    }

private:
    DelayLine          left_, right_;
    double             sampleRate_ = 0.0;
    std::atomic<bool>  enabled_  { false };
    std::atomic<float> timeMs_   { 300.0f };
    std::atomic<float> feedback_ { 0.35f };
    std::atomic<float> mix_      { 0.30f };
};

} // namespace looper::engine
