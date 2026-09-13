#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>

namespace soundsplice::engine
{
class ClipStream;

/**
    An audio clip ready to play: decoded whole into RAM, or for a long clip, a
    stream that plays it from disk (engine/ClipStream.h), in which case audio is
    empty. Allocated and freed on the message thread; ownership is handed to the
    audio thread through a lock-free queue and handed back the same way for
    deletion — so the audio thread never allocates.
*/
struct ClipData
{
    juce::AudioBuffer<float> audio;
    double sourceSampleRate = 0.0;
    int    numChannels      = 0;
    int    lengthSamples    = 0;

    std::shared_ptr<ClipStream> stream;
};

} // namespace soundsplice::engine
