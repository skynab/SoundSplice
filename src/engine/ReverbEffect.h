#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Reverb.h"

namespace looper::engine
{
/**
    Stereo master reverb wrapping the JUCE-free Reverb core. Parameters are
    atomics set from the message thread and read on the audio thread; mix maps to
    wet = mix, dry = 1 - mix. Passes audio through untouched when disabled.
*/
class ReverbEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/) { reverb_.prepare(sampleRate); }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setRoomSize(float v)      { roomSize_.store(v, std::memory_order_relaxed); }
    void setDamping(float v)       { damping_.store(v, std::memory_order_relaxed); }
    void setMix(float v)           { mix_.store(v, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float mix = mix_.load(std::memory_order_relaxed);
        reverb_.setRoomSize(roomSize_.load(std::memory_order_relaxed));
        reverb_.setDamping(damping_.load(std::memory_order_relaxed));
        reverb_.setWidth(1.0f);
        reverb_.setWet(mix);
        reverb_.setDry(1.0f - mix);

        const int numSamples = buffer.getNumSamples();

        if (buffer.getNumChannels() >= 2)
        {
            float* left  = buffer.getWritePointer(0);
            float* right = buffer.getWritePointer(1);
            for (int i = 0; i < numSamples; ++i)
                reverb_.processStereo(left[i], right[i]);
        }
        else if (buffer.getNumChannels() == 1)
        {
            float* mono = buffer.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
            {
                float l = mono[i], r = mono[i];
                reverb_.processStereo(l, r);
                mono[i] = 0.5f * (l + r);
            }
        }
    }

private:
    Reverb             reverb_;
    std::atomic<bool>  enabled_  { false };
    std::atomic<float> roomSize_ { 0.5f };
    std::atomic<float> damping_  { 0.5f };
    std::atomic<float> mix_      { 0.3f };
};

} // namespace looper::engine
