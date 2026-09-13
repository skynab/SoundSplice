#pragma once

#include <memory>
#include <vector>

#include "engine/Node.h"

namespace looper::engine
{
/**
    A minimal processing graph: a set of source nodes summed into a master node.

    Phase 1 keeps the topology fixed — nodes are added on the message thread
    before the device starts and are not changed while audio is running, so
    process() does no allocation and needs no locking. Real-time-safe structural
    edits (swapping graph state under the audio thread) arrive in a later phase;
    this class is the seam where that will live.
*/
class AudioGraph
{
public:
    void addSource(std::unique_ptr<Node> node)  { sources_.push_back(std::move(node)); }
    void setMaster(std::unique_ptr<Node> node)  { master_ = std::move(node); }

    void prepare(double sampleRate, int maxBlockSize)
    {
        for (auto& s : sources_)
            s->prepare(sampleRate, maxBlockSize);
        if (master_)
            master_->prepare(sampleRate, maxBlockSize);
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const ProcessContext& context)
    {
        buffer.clear();

        for (auto& s : sources_)
            s->process(buffer, midi, context);

        if (master_)
            master_->process(buffer, midi, context);
    }

    void release()
    {
        for (auto& s : sources_)
            s->release();
        if (master_)
            master_->release();
    }

private:
    std::vector<std::unique_ptr<Node>> sources_;
    std::unique_ptr<Node>              master_;
};

} // namespace looper::engine
