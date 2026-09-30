#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <juce_audio_basics/juce_audio_basics.h>

namespace soundsplice::engine
{
/**
    Retroactive recording: the input, kept in a ring while the transport
    plays, so a take nobody pressed Record for can still be saved afterwards.

    What it keeps is the latest *run*: the input since playback last started,
    or jumped (a seek, or the loop going round), because only across a run
    does a sample's place in the ring say where it belongs on the timeline.
    The ring holds the last @c seconds of it; a longer run keeps its end.

    The audio thread writes (process); the message thread copies the run out
    (copyLatestRun). A copy made while playing races only the oldest part of
    the ring, which the writer reaches last, so the copy leaves that part
    out. prepare() allocates, so it must be called with the audio thread
    stopped.
*/
class RetroRecorder
{
public:
    /** Room for @p seconds at @p sampleRate, or none for 0 (off). Message
        thread, with the audio callback stopped. */
    void prepare(double sampleRate, double seconds)
    {
        const int capacity = sampleRate > 0.0 && seconds > 0.0 ? (int) (sampleRate * seconds) : 0;
        ring_.setSize(2, capacity, false, true);
        ring_.clear();
        sampleRate_ = sampleRate;
        write_.store(0, std::memory_order_relaxed);
        runLength_.store(0, std::memory_order_relaxed);
        runStart_.store(-1, std::memory_order_relaxed);
        expectedNext_ = -1;
    }

    bool   isOn() const noexcept { return ring_.getNumSamples() > 0; }
    double sampleRate() const noexcept { return sampleRate_; }

    /** Audio thread. Keeps @p numSamples of input while @p playing, from
        the transport at @p playhead; a jump in the playhead starts a new
        run. A mono input is kept on both sides. */
    void process(const float* const* input, int numInputChannels, int numSamples, bool playing,
                 int64_t playhead) noexcept
    {
        const int capacity = ring_.getNumSamples();
        if (capacity <= 0 || numSamples <= 0)
            return;

        if (! playing)
        {
            expectedNext_ = -1; // the next block starts a run
            return;
        }

        if (playhead != expectedNext_)
        {
            runStart_.store(playhead, std::memory_order_relaxed);
            runLength_.store(0, std::memory_order_relaxed);
        }
        expectedNext_ = playhead + numSamples;

        int write = write_.load(std::memory_order_relaxed);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* source = input != nullptr && numInputChannels > 0
                                    ? input[std::min(ch, numInputChannels - 1)] : nullptr;
            auto*        ring   = ring_.getWritePointer(ch);
            int          at     = write;
            for (int n = 0; n < numSamples; ++n)
            {
                ring[at] = source != nullptr ? source[n] : 0.0f;
                if (++at == capacity)
                    at = 0;
            }
        }
        write_.store((write + numSamples) % capacity, std::memory_order_release);

        const int64_t length = runLength_.load(std::memory_order_relaxed) + numSamples;
        runLength_.store(std::min<int64_t>(length, capacity), std::memory_order_relaxed);
        if (length > capacity) // the run's start has been overwritten: it now starts later
            runStart_.store(expectedNext_ - capacity, std::memory_order_relaxed);
    }

    /** The latest run, copied into @p out (stereo), and where on the timeline
        it starts, in transport samples. False, leaving @p out alone, when
        there's nothing kept. @p margin samples of the oldest end are left out
        while it may still be written over. Message thread. */
    bool copyLatestRun(juce::AudioBuffer<float>& out, int64_t& startPlayhead, int margin = 0) const
    {
        const int capacity = ring_.getNumSamples();
        const int write    = write_.load(std::memory_order_acquire);
        int64_t   length   = runLength_.load(std::memory_order_relaxed);
        int64_t   start    = runStart_.load(std::memory_order_relaxed);
        if (capacity <= 0 || length <= 0 || start < 0)
            return false;

        const int64_t skip = std::clamp<int64_t>(margin - (capacity - length), 0, length);
        length -= skip;
        start  += skip;
        if (length <= 0)
            return false;

        out.setSize(2, (int) length);
        int from = (int) (((int64_t) write - length) % capacity);
        if (from < 0)
            from += capacity;
        for (int ch = 0; ch < 2; ++ch)
        {
            const int first = (int) std::min<int64_t>(length, capacity - from);
            out.copyFrom(ch, 0, ring_, ch, from, first);
            if (first < length)
                out.copyFrom(ch, first, ring_, ch, 0, (int) length - first);
        }
        startPlayhead = start;
        return true;
    }

private:
    juce::AudioBuffer<float> ring_;
    double                   sampleRate_ = 0.0;
    std::atomic<int>         write_ { 0 };
    std::atomic<int64_t>     runLength_ { 0 };
    std::atomic<int64_t>     runStart_ { -1 };
    int64_t                  expectedNext_ = -1; // audio thread only
};

} // namespace soundsplice::engine
