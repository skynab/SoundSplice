#pragma once

#include <atomic>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Pattern.h"
#include "engine/PatternPlayback.h"
#include "engine/ProcessContext.h"
#include "engine/SessionMath.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/** One cell of a track's session column. An empty slot still occupies an
    index, because the index is the scene. */
struct SessionSlotData
{
    bool    hasClip = false;
    Pattern pattern;
};

/**
    A track's session column: at most one clip playing at a time, looping from
    wherever it was launched, switched at musical boundaries.

    The message thread only ever says *which* slot it wants; the audio thread
    decides *when* that happens (see SessionMath), because only it knows the
    sample-accurate playhead. Quantizing on the message thread would round
    launches to the 30Hz UI timer — the mistake the automation work just undid.

    A launch is a single atomic rather than a queue, deliberately: clicking a
    second clip before the boundary should *replace* the pending launch, not
    stack up behind it. An atomic gets that right for free; a queue would have
    to be taught it.

    While a slot is engaged the track ignores its arrangement clips entirely —
    see InstrumentTrack::render. Stopping hands the track back to the
    arrangement.
*/
class SessionPlayer
{
public:
    using SlotList = std::vector<SessionSlotData>;

    static constexpr int kNoRequest = -2; // nothing pending
    static constexpr int kStop      = -1; // stop whatever is playing

    ~SessionPlayer()
    {
        collectRetired();
        delete current_;

        SlotList* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    // ---- message thread ----
    /** Hands ownership of @p slots (this track's whole column) to the audio thread. */
    void submitSlots(SlotList* slots)
    {
        if (! inbox_.push(slots))
            delete slots;
    }

    void collectRetired()
    {
        SlotList* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    /** Asks for @p sceneIndex to start at the next boundary. Replaces any
        launch still waiting. */
    void requestLaunch(int sceneIndex) { pending_.store(sceneIndex, std::memory_order_relaxed); }

    /** Asks for whatever is playing to stop at the next boundary. */
    void requestStop() { pending_.store(kStop, std::memory_order_relaxed); }

    /** Which slot is sounding right now, or -1. Published by the audio thread
        for the UI to read: the grid can't work this out for itself, because a
        launch stays pending until the next boundary and it would otherwise
        light the wrong cell for up to a bar. */
    int playingSlotForUI() const noexcept { return activeSlotForUI_.load(std::memory_order_relaxed); }

    /** True if this track is currently under session control — playing a clip,
        or about to. Read from the audio thread to decide whether the
        arrangement sequencer gets a look in. */
    bool isEngaged() const noexcept
    {
        return activeSlot_ >= 0 || pending_.load(std::memory_order_relaxed) != kNoRequest;
    }

    // ---- audio thread ----
    /** Renders this block, applying any pending launch whose boundary falls
        inside it. Returns true if the session is driving the track, in which
        case the caller must not also run the arrangement sequencer.

        @p quantumSamples is how long a launch boundary is (a bar, typically);
        zero launches immediately. */
    bool renderBlock(juce::MidiBuffer& midi, const ProcessContext& context, double quantumSamples)
    {
        SlotList* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_);
            current_ = incoming;
        }

        if (! context.transport.playing)
        {
            // Stopping releases whatever was sounding but keeps any pending
            // launch, so arming a clip and then hitting play does what it
            // looks like it should.
            if (activeSlot_ >= 0)
            {
                PatternPlayback::flush(midi, activeNotes_);
                activeSlot_ = -1;
                activeSlotForUI_.store(-1, std::memory_order_relaxed);
            }
            return isEngaged();
        }

        applyPendingLaunch(midi, context, quantumSamples);

        if (activeSlot_ < 0 || current_ == nullptr || activeSlot_ >= (int) current_->size())
            return isEngaged();

        const auto& slot = (*current_)[(size_t) activeSlot_];
        if (! slot.hasClip)
            return isEngaged();

        const double blockLengthBeats = context.transport.blockLengthBeats();
        if (blockLengthBeats <= 0.0)
            return true;

        // The clip loops from where it was launched, not from the song's
        // start — that's the whole difference from an arrangement clip. The
        // launch point is remembered in samples, so it is converted here
        // rather than being assumed to sit at a fixed number of beats.
        const double launchedAtBeats = context.transport.ppqPosition
                                     - (double) (context.transport.playheadSamples - slotStartSample_)
                                           / (double) juce::jmax(1, context.numSamples)
                                           * blockLengthBeats;

        const double localStart = context.transport.ppqPosition - launchedAtBeats;
        PatternPlayback::emitBlock(midi, slot.pattern, localStart, blockLengthBeats,
                                   context.numSamples, activeNotes_);
        return true;
    }

private:
    void applyPendingLaunch(juce::MidiBuffer& midi, const ProcessContext& context, double quantumSamples)
    {
        const int requested = pending_.load(std::memory_order_relaxed);
        if (requested == kNoRequest)
            return;

        const int64_t playhead = context.transport.playheadSamples;
        const int64_t boundary = SessionMath::nextLaunchBoundary(playhead, quantumSamples);

        int offset = 0;
        if (! SessionMath::boundaryInBlock(boundary, playhead, context.numSamples, offset))
            return; // not yet — stays pending, and a newer click may replace it

        // Only clear the request if it's still the one we acted on; a launch
        // arriving between the load and here must not be swallowed.
        int expected = requested;
        pending_.compare_exchange_strong(expected, kNoRequest, std::memory_order_relaxed);

        // Whatever was sounding belongs to the outgoing clip.
        PatternPlayback::flush(midi, activeNotes_);

        activeSlot_       = requested == kStop ? -1 : requested;
        slotStartSample_  = boundary;
        activeSlotForUI_.store(activeSlot_, std::memory_order_relaxed);
    }

    SlotList* current_ = nullptr; // audio-thread owned

    rt::SpscRingBuffer<SlotList*> inbox_   { 8 };
    rt::SpscRingBuffer<SlotList*> reclaim_ { 16 };

    std::atomic<int> pending_ { kNoRequest };

    int         activeSlot_      = -1; // audio thread only
    int64_t     slotStartSample_ = 0;  // audio thread only
    ActiveNotes activeNotes_ {};

    std::atomic<int> activeSlotForUI_ { -1 }; // audio -> UI readout
};

} // namespace looper::engine
