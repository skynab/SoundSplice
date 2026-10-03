#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <app/RecordingPlan.h>

using namespace soundsplice;
using namespace soundsplice::app::recording;
using Catch::Approx;

TEST_CASE("A take goes onto every armed audio track, or else the selected one", "[app][recording]")
{
    model::Song song;
    const int audioA = model::addTrack(song, model::TrackType::Audio, "A").id;      // 0
    const int synth  = model::addTrack(song, model::TrackType::Instrument, "S").id; // 1
    const int audioB = model::addTrack(song, model::TrackType::Audio, "B").id;      // 2

    // Armed: in track order, whatever order they were armed in, and only
    // the audio ones.
    REQUIRE(takeTargets(song, { audioB, audioA, synth }, 1) == std::vector<int> { 0, 2 });

    // None armed: the selected track if it can hold audio...
    REQUIRE(takeTargets(song, {}, 2) == std::vector<int> { 2 });

    // ...otherwise a new track.
    REQUIRE(takeTargets(song, {}, 1) == std::vector<int> { -1 });
    REQUIRE(takeTargets(song, {}, -1) == std::vector<int> { -1 });
    REQUIRE(takeTargets(song, { synth }, 7) == std::vector<int> { -1 });
}

TEST_CASE("Recordings are moved back by the round trip measured at this rate, else the reported one", "[app][recording]")
{
    Latency latency;
    REQUIRE(latencySamples(latency, 48000.0, 256) == 256);

    latency.adjustMs = 1.0; // 48 samples at 48k
    REQUIRE(latencySamples(latency, 48000.0, 256) == 304);

    latency.measuredSamples = 400;
    latency.measuredRate    = 48000.0;
    REQUIRE(usesMeasured(latency, 48000.0));
    REQUIRE(latencySamples(latency, 48000.0, 256) == 448);

    // Measured at another rate: the device's figure again.
    REQUIRE_FALSE(usesMeasured(latency, 44100.0));
    REQUIRE(latencySamples(latency, 44100.0, 256) == 256 + 44);

    // Never early, and nothing at all with compensation off.
    latency.adjustMs = -100.0;
    REQUIRE(latencySamples(latency, 48000.0, 256) == 0);
    latency.compensate = false;
    latency.adjustMs   = 0.0;
    REQUIRE(latencySamples(latency, 48000.0, 256) == 0);
}

TEST_CASE("Sound-activated recording waits for a level and stops on a silence", "[app][recording]")
{
    const auto off = soundTrigger(false, -20.0, 2.0, 48000.0);
    REQUIRE(off.thresholdGain == 0.0f);
    REQUIRE(off.stopAfterSamples == 0);

    const auto on = soundTrigger(true, -20.0, 2.0, 48000.0);
    REQUIRE(on.thresholdGain == Approx(0.1f));
    REQUIRE(on.stopAfterSamples == 96000);

    REQUIRE(soundTrigger(true, -20.0, 0.0, 48000.0).stopAfterSamples == 0); // never stops by itself
    REQUIRE(soundTrigger(true, -20.0, 1.0, 0.0).stopAfterSamples == 48000); // no device yet: 48k
}

TEST_CASE("A timed take starts once, and stops once its length is up", "[app][recording]")
{
    using Action = TimerRecord::Action;
    TimerRecord timer;
    REQUIRE(timer.tick(0, false) == Action::None);

    timer.schedule(1000, 5.0, 2.0); // starts at 301000, stops at 421000
    REQUIRE(timer.isPending());
    REQUIRE(timer.startMs() == 301000);
    REQUIRE(timer.stopMs() == 421000);

    REQUIRE(timer.tick(300999, false) == Action::None);
    REQUIRE(timer.tick(301000, false) == Action::Start);
    REQUIRE_FALSE(timer.isPending());
    REQUIRE(timer.tick(301001, true) == Action::None);
    REQUIRE(timer.tick(421000, true) == Action::Stop);
    REQUIRE(timer.tick(500000, true) == Action::None); // once

    // Already recording when it comes round: nothing to start. Stopped by
    // hand before its end: nothing to stop.
    timer.schedule(0, 0.0, 1.0);
    REQUIRE(timer.tick(0, true) == Action::None);
    REQUIRE(timer.tick(60000, false) == Action::None);

    // No length: it records until stopped.
    timer.schedule(0, 1.0, 0.0);
    REQUIRE_FALSE(timer.stopMs());

    timer.cancel();
    REQUIRE_FALSE(timer.isPending());
    REQUIRE(timer.tick(60000, false) == Action::None);
}

TEST_CASE("A MIDI take becomes a clip of whole bars where it was played", "[app][recording]")
{
    engine::TempoMap tempo; // 4/4
    tempo.setSampleRate(48000.0);
    tempo.setTempo(120.0); // a beat every 24000 samples

    // From beat 4: a note on beat 4.5 held for a beat, and a stray note-off
    // from a key already down when the take began.
    const std::vector<engine::RecordedMidiEvent> events {
        { 96000, 64, 0.0f, false },
        { 108000, 60, 0.8f, true },
        { 132000, 60, 0.0f, false },
    };
    const auto take = clipFromMidiTake(events, 96000, 144000, tempo);
    REQUIRE(take);
    REQUIRE(take->startBeats == Approx(4.0));
    REQUIRE(take->lengthBeats == Approx(4.0)); // two beats played, one bar
    REQUIRE(take->notes.size() == 1);
    REQUIRE(take->notes[0].startBeats == Approx(0.5));
    REQUIRE(take->notes[0].lengthBeats == Approx(1.0));

    // Nothing played: no clip.
    REQUIRE_FALSE(clipFromMidiTake({ { 96000, 64, 0.0f, false } }, 96000, 144000, tempo));
}
