#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace looper::engine
{
/**
    A whole audio clip decoded into RAM. Allocated and freed on the message
    thread; ownership is handed to the audio thread through a lock-free queue and
    handed back the same way for deletion — so the audio thread never allocates.
*/
struct ClipData
{
    juce::AudioBuffer<float> audio;
    double sourceSampleRate = 0.0;
    int    numChannels      = 0;
    int    lengthSamples    = 0;
};

} // namespace looper::engine
