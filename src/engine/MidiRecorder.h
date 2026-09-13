#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "engine/MidiCapture.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Captures incoming MIDI note events while armed and the transport is
    playing.

    Deliberately mirrors AudioRecorder's lifecycle — arm/disarm, a skipped
    count-in lead-in, a latched start playhead, an isFinished() the message
    thread waits on, and a *counted* drop path — so there is one recording
    discipline in this codebase rather than two that drift apart. What differs
    is only what is being captured: a take is a few thousand 16-byte events,
    not megabytes of samples, so there is no disk streaming and no
    ThreadedWriter here. A pre-allocated `rt::SpscRingBuffer` is the whole
    mechanism.

    **Hand-off is RT-safe without a lock**, by the ring buffer's SPSC contract:
    process() (audio thread) is the only producer, drain() (message thread) is
    the only consumer. Nothing here allocates on the audio thread.

    **The message thread drains continuously, not just at the end of a take.**
    A ring sized once for a whole take would be exactly the hidden cap
    AudioRecorder's own history warns about — the take that comes back short
    with no error. Draining on a timer means the ring only ever has to hold one
    interval's worth of playing, while the take itself (a plain vector on the
    message thread) is unbounded.

    JUCE-free: AudioEngine translates `juce::MidiMessage`s into
    RecordedMidiEvent on the way in, which keeps this — and the pairing in
    MidiCapture — unit-testable headless.
*/
class MidiRecorder
{
public:
    /** Events the ring can hold between drains. About a second and a half of
        dense two-handed playing at a 30 Hz drain, so overflow means something
        has gone badly wrong (a stalled message thread) rather than "you played
        fast". Overflow is counted either way — see droppedEventCount. */
    static constexpr std::size_t kEventCapacity = 4096;

    MidiRecorder() : events_(kEventCapacity) {}

    // ---- message thread ----
    /**
        Starts a new take.

        @p leadInSamples rolls the transport for that long before capture
        begins — a count-in. The lead-in is *skipped*, not captured, so the
        take starts at the first note actually played.
    */
    void arm(int64_t leadInSamples = 0)
    {
        // A previous take's leftovers, if it was never collected: drained here
        // rather than left to contaminate this one.
        RecordedMidiEvent discarded;
        while (events_.pop(discarded))
        {
        }

        captured_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        startPlayhead_.store(-1, std::memory_order_relaxed);
        endPlayhead_.store(-1, std::memory_order_relaxed);
        leadInRemaining_.store(leadInSamples > 0 ? leadInSamples : 0, std::memory_order_relaxed);
        finished_.store(false, std::memory_order_relaxed);

        armed_.store(true, std::memory_order_release);
    }

    /** Samples of count-in still to elapse before capture starts. */
    int64_t leadInRemaining() const noexcept { return leadInRemaining_.load(std::memory_order_relaxed); }

    /** Signals the audio thread to stop capturing; the take finishes on the
        next block it processes (or immediately if the transport already isn't
        playing). */
    void disarm() { armed_.store(false, std::memory_order_release); }

    bool isArmed() const noexcept { return armed_.load(std::memory_order_relaxed); }

    /** True once the audio thread has confirmed it will no longer produce
        events — only then is the take complete. */
    bool isFinished() const noexcept { return finished_.load(std::memory_order_acquire); }

    /** Events captured so far — safe to poll live. */
    int64_t capturedEventCount() const noexcept { return captured_.load(std::memory_order_relaxed); }

    /**
        Events lost because the ring was full, i.e. the message thread stalled
        long enough to stop draining.

        Exists for the same reason AudioRecorder::droppedSampleCount does: a
        take quietly missing a note looks fine until it is played, which is
        worse than one that failed outright.
    */
    int64_t droppedEventCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    /** Where the transport was when capture actually began, in samples, or -1
        if nothing was captured. Latched on the audio thread rather than
        recomputed from bars and tempo on the message thread, because the
        count-in means capture starts later than arming and re-deriving that
        offset is arithmetic that can disagree with what really happened. */
    int64_t startPlayheadSamples() const noexcept { return startPlayhead_.load(std::memory_order_relaxed); }

    /** Where the transport was when capture stopped, in samples, or -1. This
        is what bounds a note still held at the end of the take. */
    int64_t endPlayheadSamples() const noexcept { return endPlayhead_.load(std::memory_order_relaxed); }

    /** Moves everything captured since the last call onto @p destination.
        Called on a timer during the take and once more after isFinished(). */
    void drain(std::vector<RecordedMidiEvent>& destination)
    {
        RecordedMidiEvent event;
        while (events_.pop(event))
            destination.push_back(event);
    }

    // ---- audio thread ----
    /**
        Offers this block's note events for capture.

        @p events are timed in samples *within the block*; @p playheadSamples
        is where the block starts, so the two combine into a position on the
        transport's timeline. Non-note messages must be filtered out by the
        caller — this records notes, not controllers.
    */
    void process(const RecordedMidiEvent* events, int numEvents, int numSamples,
                 bool transportPlaying, int64_t playheadSamples) noexcept
    {
        const bool armedNow = armed_.load(std::memory_order_acquire);

        // Count-in: rolling and armed, but capture hasn't started. Not treated
        // as recording, so a take stopped during its own count-in finishes
        // empty rather than reporting as a zero-length take that began.
        //
        // The lead-in almost never ends on a block boundary, and the block it
        // ends *inside* is a real part of the take: it contains the downbeat
        // the count-in was counting to, which is exactly where a player puts
        // their first note. Discarding that whole block — the obvious
        // implementation, and the one the bounce tool caught here — loses the
        // most common note anyone records. So a partial lead-in is subtracted
        // within the block instead, and capture starts mid-block at
        // `skipSamples`.
        int64_t skipSamples = 0;
        int64_t leadIn = leadInRemaining_.load(std::memory_order_relaxed);
        if (armedNow && transportPlaying && leadIn > 0)
        {
            skipSamples = leadIn < (int64_t) numSamples ? leadIn : (int64_t) numSamples;
            leadIn -= (int64_t) numSamples;
            leadInRemaining_.store(leadIn > 0 ? leadIn : 0, std::memory_order_relaxed);

            if (skipSamples >= (int64_t) numSamples)
                return; // the whole block was count-in
        }

        const bool recordingNow = armedNow && transportPlaying;

        // The take ends when capture stops...
        if (wasRecording_ && ! recordingNow)
        {
            endPlayhead_.store(playheadSamples, std::memory_order_relaxed);
            finished_.store(true, std::memory_order_release);
        }
        wasRecording_ = recordingNow;

        // ...and also whenever we're disarmed without having captured
        // anything — stopped during a count-in, or armed and stopped before
        // the transport rolled. Without this, finished_ is never published and
        // the owner waits forever on a take that will never arrive, which
        // leaves the UI stuck mid-record. (finished_ starts true, so this only
        // fires on the transition out of an armed take.)
        if (! armedNow && ! finished_.load(std::memory_order_relaxed))
            finished_.store(true, std::memory_order_release);

        if (! recordingNow)
            return;

        // The exact sample the count-in ended on, not the start of the block
        // that contained it — this is the take's musical downbeat, and it is
        // what positions the recorded clip.
        if (startPlayhead_.load(std::memory_order_relaxed) < 0)
            startPlayhead_.store(playheadSamples + skipSamples, std::memory_order_relaxed);

        // Published every block, not only at the end: a take whose transport
        // is stopped by something that doesn't run another block still needs a
        // sensible end for its held notes to clamp to.
        endPlayhead_.store(playheadSamples + (int64_t) numSamples, std::memory_order_relaxed);

        for (int i = 0; i < numEvents; ++i)
        {
            RecordedMidiEvent event = events[i];

            // Anything still inside the count-in part of this block belongs to
            // the wait, not to the take.
            if (event.timeSamples < skipSamples)
                continue;

            event.timeSamples += playheadSamples;

            if (events_.push(event))
                captured_.fetch_add(1, std::memory_order_relaxed);
            else
                dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    rt::SpscRingBuffer<RecordedMidiEvent> events_;

    std::atomic<int64_t> captured_        { 0 };
    std::atomic<int64_t> dropped_         { 0 };
    std::atomic<int64_t> startPlayhead_   { -1 };
    std::atomic<int64_t> endPlayhead_     { -1 };
    std::atomic<int64_t> leadInRemaining_ { 0 };
    std::atomic<bool>    armed_    { false };
    std::atomic<bool>    finished_ { true }; // true initially: no take pending

    bool wasRecording_ = false;              // audio-thread only
};

} // namespace looper::engine
