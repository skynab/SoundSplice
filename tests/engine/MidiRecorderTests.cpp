#include <catch2/catch_test_macros.hpp>

#include <engine/MidiRecorder.h>

using namespace looper::engine;

namespace
{
    constexpr int kBlock = 512;

    RecordedMidiEvent noteOn(int64_t offsetInBlock, int noteNumber = 60)
    {
        return { offsetInBlock, noteNumber, 0.8f, true };
    }

    RecordedMidiEvent noteOff(int64_t offsetInBlock, int noteNumber = 60)
    {
        return { offsetInBlock, noteNumber, 0.0f, false };
    }

    /** One block through the recorder. */
    void runBlock(MidiRecorder& recorder, std::vector<RecordedMidiEvent> events,
                  bool playing, int64_t playhead)
    {
        recorder.process(events.data(), (int) events.size(), kBlock, playing, playhead);
    }
}

TEST_CASE("A fresh recorder is finished and captures nothing", "[engine][midirecorder]")
{
    MidiRecorder recorder;

    REQUIRE(recorder.isFinished());
    REQUIRE_FALSE(recorder.isArmed());

    // Not armed: events offered are simply not ours to take.
    runBlock(recorder, { noteOn(0) }, true, 0);
    REQUIRE(recorder.capturedEventCount() == 0);
}

TEST_CASE("Armed and playing captures events at transport positions", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();
    REQUIRE_FALSE(recorder.isFinished());

    runBlock(recorder, { noteOn(100, 64) }, true, 1000);
    runBlock(recorder, { noteOff(50, 64) }, true, 1000 + kBlock);

    REQUIRE(recorder.capturedEventCount() == 2);
    REQUIRE(recorder.droppedEventCount() == 0);
    REQUIRE(recorder.startPlayheadSamples() == 1000);

    std::vector<RecordedMidiEvent> take;
    recorder.drain(take);

    REQUIRE(take.size() == 2);
    // Block-relative offsets become timeline positions.
    REQUIRE(take[0].timeSamples == 1100);
    REQUIRE(take[0].noteOn);
    REQUIRE(take[1].timeSamples == 1000 + kBlock + 50);
    REQUIRE_FALSE(take[1].noteOn);
}

TEST_CASE("Armed but stopped captures nothing", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();

    runBlock(recorder, { noteOn(0) }, false, 0);

    REQUIRE(recorder.capturedEventCount() == 0);
    REQUIRE(recorder.startPlayheadSamples() == -1);
}

TEST_CASE("The count-in is skipped, not captured", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm(kBlock * 2); // two blocks of lead-in

    runBlock(recorder, { noteOn(0) }, true, 0);
    REQUIRE(recorder.capturedEventCount() == 0);
    REQUIRE(recorder.leadInRemaining() == kBlock);

    runBlock(recorder, { noteOn(0) }, true, kBlock);
    REQUIRE(recorder.capturedEventCount() == 0);
    REQUIRE(recorder.leadInRemaining() == 0);

    // Capture starts at the first block after the lead-in, and the take's
    // start is where the transport actually was then — not where it was armed.
    runBlock(recorder, { noteOn(10) }, true, kBlock * 2);
    REQUIRE(recorder.capturedEventCount() == 1);
    REQUIRE(recorder.startPlayheadSamples() == kBlock * 2);
}

TEST_CASE("A note on the count-in downbeat is captured, not swallowed", "[engine][midirecorder]")
{
    // The lead-in almost never ends on a block boundary, and the block it ends
    // inside holds the downbeat the count-in was counting to — which is
    // exactly where a player puts their first note. Discarding that whole
    // block loses the most common note anyone records.
    MidiRecorder recorder;
    const int64_t leadIn = kBlock + 200; // ends 200 samples into the second block
    recorder.arm(leadIn);

    runBlock(recorder, { noteOn(0) }, true, 0); // wholly count-in
    REQUIRE(recorder.capturedEventCount() == 0);

    // Second block: one note still inside the count-in, one on the downbeat.
    runBlock(recorder, { noteOn(199, 60), noteOn(200, 64) }, true, kBlock);

    REQUIRE(recorder.capturedEventCount() == 1);
    REQUIRE(recorder.startPlayheadSamples() == leadIn); // the exact downbeat, not the block start

    std::vector<RecordedMidiEvent> take;
    recorder.drain(take);

    REQUIRE(take.size() == 1);
    REQUIRE(take[0].noteNumber == 64);
    REQUIRE(take[0].timeSamples == leadIn);
}

TEST_CASE("Disarming finishes the take and latches its end", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();

    runBlock(recorder, { noteOn(0) }, true, 0);
    REQUIRE_FALSE(recorder.isFinished());

    recorder.disarm();
    runBlock(recorder, {}, true, kBlock);

    REQUIRE(recorder.isFinished());
    REQUIRE(recorder.endPlayheadSamples() >= kBlock);
}

TEST_CASE("Stopping the transport finishes the take", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();

    runBlock(recorder, { noteOn(0) }, true, 0);
    runBlock(recorder, {}, false, kBlock); // transport stopped, still armed

    REQUIRE(recorder.isFinished());
}

TEST_CASE("A take stopped during its own count-in still finishes", "[engine][midirecorder]")
{
    // Otherwise isFinished() never becomes true, the owner waits on a take
    // that will never arrive, and the UI is stuck mid-record.
    MidiRecorder recorder;
    recorder.arm(kBlock * 4);

    runBlock(recorder, {}, true, 0);
    REQUIRE_FALSE(recorder.isFinished());

    recorder.disarm();
    runBlock(recorder, {}, true, kBlock);

    REQUIRE(recorder.isFinished());
    REQUIRE(recorder.capturedEventCount() == 0);
    REQUIRE(recorder.startPlayheadSamples() == -1);
}

TEST_CASE("A take armed and stopped before the transport rolled finishes", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();
    recorder.disarm();

    runBlock(recorder, {}, false, 0);

    REQUIRE(recorder.isFinished());
}

TEST_CASE("Overflowing the ring is counted, not swallowed", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();

    // More events than the ring can hold without a drain — what a stalled
    // message thread would produce.
    std::vector<RecordedMidiEvent> flood;
    const int count = (int) MidiRecorder::kEventCapacity + 100;
    flood.reserve((size_t) count);
    for (int i = 0; i < count; ++i)
        flood.push_back(noteOn(0, 60));

    runBlock(recorder, flood, true, 0);

    REQUIRE(recorder.capturedEventCount() == (int64_t) MidiRecorder::kEventCapacity);
    REQUIRE(recorder.droppedEventCount() == 100);
}

TEST_CASE("Draining during a take keeps the ring from filling", "[engine][midirecorder]")
{
    // The reason the ring can be small: the message thread empties it on a
    // timer, so the take itself is unbounded.
    MidiRecorder recorder;
    recorder.arm();

    std::vector<RecordedMidiEvent> take;
    const int blocks = 40;
    const int perBlock = 200; // 8000 events total, well past kEventCapacity

    for (int block = 0; block < blocks; ++block)
    {
        std::vector<RecordedMidiEvent> events;
        for (int i = 0; i < perBlock; ++i)
            events.push_back(noteOn(i, 60));

        runBlock(recorder, events, true, (int64_t) block * kBlock);
        recorder.drain(take); // the timer
    }

    REQUIRE(recorder.droppedEventCount() == 0);
    REQUIRE(take.size() == (size_t) (blocks * perBlock));
}

TEST_CASE("Arming again discards an uncollected previous take", "[engine][midirecorder]")
{
    MidiRecorder recorder;
    recorder.arm();
    runBlock(recorder, { noteOn(0, 60) }, true, 0);
    recorder.disarm();
    runBlock(recorder, {}, false, kBlock);

    recorder.arm(); // never drained the first take

    std::vector<RecordedMidiEvent> take;
    recorder.drain(take);

    REQUIRE(take.empty());
    REQUIRE(recorder.capturedEventCount() == 0);
    REQUIRE(recorder.startPlayheadSamples() == -1);
}
