#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ProcessContext.h"

namespace looper::engine
{
/**
    A processing-graph node. Sources add their output into the buffer; the master
    bus processes in place. All three methods that touch audio run on the audio
    thread and must obey the real-time rules (no locks, no allocation, no I/O).

    prepare()/release() run on the message thread while the device is stopped.
*/
class Node
{
public:
    virtual ~Node() = default;

    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const ProcessContext& context) = 0;
    virtual void release() {}
};

} // namespace looper::engine
