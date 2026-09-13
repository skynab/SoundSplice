#pragma once

#include <array>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ClipSlot.h"
#include "engine/PatternPlayback.h"
#include "engine/ProcessContext.h"
#include "engine/SequencerMath.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Turns a track's ClipSlots into MIDI, emitted into the per-block buffer
    sample-accurately and slaved to the transport. Each clip's pattern loops on
    its own length within the clip's [startBeats, startBeats + lengthBeats)
    window; outside every clip's window the track is silent. Clips are expected
    not to overlap — the first whose window covers the current block wins.

    A track with a single clip is given an effectively unbounded length by the
    caller (see AudioEngine::setTrackClips), so it keeps looping indefinitely
    from its start — the "plays until Stop" behaviour every track has always
    had. Real length gating (and silence between clips) only bites once a track
    has more than one clip.

    Clip-list edits are handed over lock-free (inbox/reclaim FIFOs), and the
    audio thread never allocates. On stop, or when switching between clips (or
    into silence), any notes still sounding are flushed so voices don't hang.
*/
class Sequencer
{
public:
    using ClipList = std::vector<ClipSlot>;

    ~Sequencer()
    {
        collectRetired();
        delete current_;

        ClipList* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    // ---- message thread ----
    /** Hands ownership of @p clips (a whole new clip list for this track) to the audio thread. */
    void submitClips(ClipList* clips)
    {
        if (! inbox_.push(clips))
            delete clips;
    }

    void collectRetired()
    {
        ClipList* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    // ---- audio thread ----
    void renderBlock(juce::MidiBuffer& midi, const ProcessContext& context)
    {
        ClipList* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_);
            current_ = incoming;
        }

        if (! context.transport.playing)
        {
            if (wasPlaying_)
            {
                flushActiveNotes(midi);
                wasPlaying_      = false;
                activeClipIndex_ = -1;
            }
            return;
        }
        wasPlaying_ = true;

        if (current_ == nullptr || current_->empty()
            || context.transport.blockLengthBeats() <= 0.0 || context.sampleRate <= 0.0)
            return;

        const int    numSamples      = context.numSamples;
        const double blockStartBeats = context.transport.ppqPosition;
        const double blockEndBeats   = context.transport.ppqAtBlockEnd;

        // Find the clip whose window overlaps this block (block-granularity: a
        // block straddling a clip boundary can place a note up to one block
        // early/late — an accepted, documented imprecision at typical block
        // sizes of a few ms).
        int    foundIndex = -1;
        double localStart = 0.0;

        for (int i = 0; i < (int) current_->size(); ++i)
        {
            const auto&  slot  = (*current_)[(size_t) i];
            const double local = blockStartBeats - slot.startBeats;

            // Compared in beats, where a clip's window is actually defined.
            // Doing it in samples meant converting the clip's position through
            // one tempo, which stops meaning anything the moment there is more
            // than one.
            if (blockEndBeats > slot.startBeats && local < slot.lengthBeats)
            {
                foundIndex = i;
                localStart = local;
                break;
            }
        }

        if (foundIndex != activeClipIndex_)
        {
            // Switching clips, or moving into/out of silence — flush whatever
            // notes the previous clip left hanging before starting the next.
            flushActiveNotes(midi);
            activeClipIndex_ = foundIndex;
        }

        if (foundIndex < 0)
            return; // between clips, or before/after every clip's window

        const auto& slot = (*current_)[(size_t) foundIndex];
        PatternPlayback::emitBlock(midi, slot.pattern, localStart,
                                   context.transport.blockLengthBeats(), numSamples, activeNotes_);
    }

    /** Releases anything this sequencer has sounding and forgets which clip it
        was on. Used when something else takes over the track — a launched
        session clip — so arrangement notes can't hang behind it. */
    void reset(juce::MidiBuffer& midi)
    {
        flushActiveNotes(midi);
        activeClipIndex_ = -1;
    }

private:
    void flushActiveNotes(juce::MidiBuffer& midi) { PatternPlayback::flush(midi, activeNotes_); }

    ClipList* current_ = nullptr;
    rt::SpscRingBuffer<ClipList*> inbox_   { 16 };
    rt::SpscRingBuffer<ClipList*> reclaim_ { 32 };

    bool                  wasPlaying_      = false;
    int                   activeClipIndex_ = -1;
    ActiveNotes           activeNotes_ {};
};

} // namespace looper::engine
