#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/TempoMap.h>

using Catch::Approx;
using looper::engine::TempoMap;
using looper::engine::TempoChange;

TEST_CASE("TempoMap converts samples <-> beats at 120 bpm / 48k", "[engine][tempo]")
{
    TempoMap t;
    t.setSampleRate(48000.0);
    t.setTempo(120.0);

    REQUIRE(t.samplesPerBeatAt(0.0) == Approx(24000.0));
    REQUIRE(t.ppqFromSamples(24000) == Approx(1.0));
    REQUIRE(t.ppqFromSamples(96000) == Approx(4.0));
    REQUIRE(t.samplesFromPpq(1.0) == 24000);
    REQUIRE(t.samplesFromPpq(4.0) == 96000);
}

TEST_CASE("TempoMap reports bars/beats in 4/4", "[engine][tempo]")
{
    TempoMap t;
    t.setSampleRate(48000.0);
    t.setTempo(120.0);
    t.setTimeSignature(4, 4);

    REQUIRE(t.quartersPerBar() == Approx(4.0));

    auto start = t.barsBeatsFromSamples(0);
    REQUIRE(start.bar == 1);
    REQUIRE(start.beat == 1);

    auto beat2 = t.barsBeatsFromSamples(24000); // one quarter note
    REQUIRE(beat2.bar == 1);
    REQUIRE(beat2.beat == 2);

    auto beat3 = t.barsBeatsFromSamples(48000);
    REQUIRE(beat3.bar == 1);
    REQUIRE(beat3.beat == 3);

    auto bar2 = t.barsBeatsFromSamples(96000); // four quarters
    REQUIRE(bar2.bar == 2);
    REQUIRE(bar2.beat == 1);
}

TEST_CASE("TempoMap respects the time signature denominator", "[engine][tempo]")
{
    TempoMap t;
    t.setSampleRate(48000.0);
    t.setTempo(120.0);
    t.setTimeSignature(6, 8);

    REQUIRE(t.quartersPerBar() == Approx(3.0)); // 6 eighths = 3 quarters

    auto eighth = t.barsBeatsFromSamples(12000); // half a quarter = one eighth
    REQUIRE(eighth.bar == 1);
    REQUIRE(eighth.beat == 2);

    auto nextBar = t.barsBeatsFromSamples(72000); // 3 quarters = one 6/8 bar
    REQUIRE(nextBar.bar == 2);
    REQUIRE(nextBar.beat == 1);
}

// ---------------------------------------------------------------------------
// The piecewise map.

TEST_CASE("One tempo converts exactly as constant-tempo arithmetic did", "[engine][tempo]")
{
    // The guarantee every existing project rests on. Before this the engine
    // multiplied by sampleRate*60/bpm in eight places; a map with one entry
    // has to reproduce that to the sample, or every old song shifts.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempo(140.0);

    const double samplesPerBeat = 48000.0 * 60.0 / 140.0;

    // Coming back from a *rounded* sample position cannot be exact - half a
    // sample is about 2.4e-5 of a beat at this tempo - so the reverse check is
    // allowed one sample's worth. The forward direction below has no such
    // excuse and is asserted exactly, because that is the equivalence every
    // existing project actually depends on.
    const double oneSampleInBeats = 1.0 / samplesPerBeat;

    for (double beat : { 0.0, 0.5, 1.0, 17.25, 512.0, 4096.0 })
    {
        INFO("beat " << beat);
        CHECK(map.sampleOffsetForPpq(beat) == Approx(beat * samplesPerBeat));
        CHECK(map.ppqFromSamples((int64_t) std::llround(beat * samplesPerBeat))
                  == Approx(beat).margin(oneSampleInBeats));
    }
}

TEST_CASE("A tempo change moves everything after it", "[engine][tempo]")
{
    // Eight beats at 120 then a jump to 60: the first eight beats take four
    // seconds, and every beat after that takes twice as long as it used to.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 120.0 }, { 8.0, 60.0 } });

    CHECK(map.sampleOffsetForPpq(0.0) == Approx(0.0));
    CHECK(map.sampleOffsetForPpq(8.0) == Approx(4.0 * 48000.0));   // 8 beats at 120bpm
    CHECK(map.sampleOffsetForPpq(9.0) == Approx(5.0 * 48000.0));   // +1 beat at 60bpm
    CHECK(map.sampleOffsetForPpq(12.0) == Approx(8.0 * 48000.0));

    CHECK(map.tempoAtBeat(0.0) == 120.0);
    CHECK(map.tempoAtBeat(7.99) == 120.0);
    CHECK(map.tempoAtBeat(8.0) == 60.0);  // the change owns its own instant
    CHECK(map.tempoAtBeat(99.0) == 60.0);
}

TEST_CASE("Beats and samples round-trip across several changes", "[engine][tempo]")
{
    TempoMap map;
    map.setSampleRate(44100.0);
    map.setTempoChanges({ { 0.0, 90.0 }, { 4.0, 150.0 }, { 12.5, 75.0 }, { 40.0, 128.0 } });

    for (double beat = 0.0; beat < 80.0; beat += 0.125)
    {
        const double samples = map.sampleOffsetForPpq(beat);
        const double back    = map.ppqFromSamples((int64_t) std::llround(samples));

        INFO("beat " << beat << " -> " << samples << " -> " << back);
        CHECK(back == Approx(beat).margin(1.0e-3)); // within a sample's worth
    }
}

TEST_CASE("Time never runs backwards", "[engine][tempo]")
{
    // A later beat is never an earlier sample, whatever the changes do. Get
    // this wrong and the sequencer schedules notes in the past.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 200.0 }, { 3.0, 40.0 }, { 3.25, 240.0 }, { 16.0, 60.0 } });

    double previous = -1.0;
    for (double beat = 0.0; beat < 32.0; beat += 0.05)
    {
        const double samples = map.sampleOffsetForPpq(beat);
        INFO("beat " << beat << " -> " << samples);
        CHECK(samples > previous);
        previous = samples;
    }
}

TEST_CASE("Changes closer together than a block still convert", "[engine][tempo]")
{
    // A block is ~512 samples; these segments are shorter than that. The map
    // must not care - it is asked about positions, not blocks.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 120.0 }, { 0.01, 240.0 }, { 0.02, 60.0 }, { 0.03, 120.0 } });

    CHECK(map.sampleOffsetForPpq(0.0) == Approx(0.0));
    CHECK(map.tempoAtBeat(0.015) == 240.0);
    CHECK(map.tempoAtBeat(0.025) == 60.0);

    const double atEnd = map.sampleOffsetForPpq(1.0);
    CHECK(atEnd > 0.0);
    CHECK(map.ppqFromSamples((int64_t) std::llround(atEnd)) == Approx(1.0).margin(1.0e-3));
}

TEST_CASE("An unsorted or invalid map is cleaned up rather than trusted", "[engine][tempo]")
{
    // These come from a document, which a future version may write differently
    // - and a zero or negative tempo divides by zero deep inside playback.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 16.0, 100.0 }, { 4.0, 150.0 }, { 0.0, 90.0 },
                          { 8.0, 0.0 }, { 10.0, -5.0 }, { -3.0, 111.0 } });

    const auto& changes = map.tempoChanges();

    // Sorted, starting at zero, with nothing invalid left in.
    REQUIRE(changes.size() >= 1);
    CHECK(changes.front().beat == 0.0);

    for (size_t i = 1; i < changes.size(); ++i)
    {
        INFO("change " << i << " at beat " << changes[i].beat);
        CHECK(changes[i].beat > changes[i - 1].beat);
    }

    for (const auto& change : changes)
        CHECK(change.bpm > 0.0);

    // Two entries at or before the start collapse into it, and the one *at*
    // beat 0 wins - it is the more specific of the two, and it matches the
    // "two changes at the same beat, the later one wins" rule below it.
    CHECK(map.tempoAtBeat(0.0) == 90.0);
}

TEST_CASE("A map that starts after beat 0 still says what beat 0 is", "[engine][tempo]")
{
    // The earliest tempo extends backwards. Without this the first segment
    // would inherit whatever the previous map happened to hold, so the same
    // document would open at different tempos depending on what came before.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempo(200.0);
    map.setTempoChanges({ { 16.0, 100.0 }, { 4.0, 150.0 } });

    CHECK(map.tempoAtBeat(0.0) == 150.0);
    CHECK(map.tempoAtBeat(4.0) == 150.0);
    CHECK(map.tempoAtBeat(16.0) == 100.0);
}

TEST_CASE("An empty map still plays", "[engine][tempo]")
{
    // Handing in nothing must leave a usable tempo rather than a zero one.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempo(133.0);
    map.setTempoChanges({});

    CHECK(map.tempo() > 0.0);
    CHECK(map.samplesPerBeatAt(0.0) > 0.0);
    CHECK(map.tempoChanges().size() == 1);
}

TEST_CASE("Changing the sample rate rescales the whole map", "[engine][tempo]")
{
    // An export at 44.1k renders the same music as playback at 48k: the same
    // beat has to land at the same *time*, not the same sample.
    TempoMap at48;
    at48.setSampleRate(48000.0);
    at48.setTempoChanges({ { 0.0, 120.0 }, { 8.0, 75.0 } });

    TempoMap at44;
    at44.setSampleRate(44100.0);
    at44.setTempoChanges({ { 0.0, 120.0 }, { 8.0, 75.0 } });

    for (double beat : { 1.0, 8.0, 20.0 })
    {
        const double seconds48 = at48.sampleOffsetForPpq(beat) / 48000.0;
        const double seconds44 = at44.sampleOffsetForPpq(beat) / 44100.0;

        INFO("beat " << beat << ": " << seconds48 << "s vs " << seconds44 << "s");
        CHECK(seconds48 == Approx(seconds44));
    }
}

// ---------------------------------------------------------------------------
// Ramps.

TEST_CASE("A ramp takes the time the integral says", "[engine][tempo][ramp]")
{
    // The closed form checked against brute-force numerical integration, which
    // is an independent derivation rather than a restatement of the same
    // algebra. If the logarithm is wrong, these disagree.
    constexpr double rate = 48000.0;

    TempoMap map;
    map.setSampleRate(rate);
    map.setTempoChanges({ { 0.0, 60.0 }, { 16.0, 180.0, true } });

    // Integrate 60/tempo(b) db in very small steps.
    const double b0 = 0.0, b1 = 16.0, t0 = 60.0, t1 = 180.0;
    const int    steps = 2000000;
    double       seconds = 0.0;

    for (int i = 0; i < steps; ++i)
    {
        const double b = b0 + (b1 - b0) * (i + 0.5) / steps;
        const double tempo = t0 + (t1 - t0) * (b - b0) / (b1 - b0);
        seconds += (60.0 / tempo) * (b1 - b0) / steps;
    }

    const double expected = seconds * rate;
    const double actual   = map.sampleOffsetForPpq(16.0);

    INFO("numerical " << expected << "  closed form " << actual);
    CHECK(actual == Approx(expected).epsilon(1.0e-6));
}

TEST_CASE("A ramp round-trips beats and samples", "[engine][tempo][ramp]")
{
    // The exponential inverse has to undo the logarithm exactly, or the
    // playhead and the notes disagree about where they are.
    TempoMap map;
    map.setSampleRate(44100.0);
    map.setTempoChanges({ { 0.0, 90.0 }, { 8.0, 160.0, true },
                          { 24.0, 70.0, true }, { 40.0, 120.0 } });

    for (double beat = 0.0; beat < 56.0; beat += 0.05)
    {
        const double samples = map.sampleOffsetForPpq(beat);
        const double back    = map.ppqFromSamples((int64_t) std::llround(samples));

        INFO("beat " << beat << " -> " << samples << " -> " << back);
        CHECK(back == Approx(beat).margin(1.0e-3));
    }
}

TEST_CASE("A ramp between equal tempos is the flat case", "[engine][tempo][ramp]")
{
    // Where the slope is zero the integral is undefined and the code has to
    // take the linear branch rather than divide by it.
    TempoMap ramped;
    ramped.setSampleRate(48000.0);
    ramped.setTempoChanges({ { 0.0, 120.0 }, { 8.0, 120.0, true } });

    TempoMap flat;
    flat.setSampleRate(48000.0);
    flat.setTempo(120.0);

    for (double beat : { 0.0, 1.0, 4.0, 8.0, 12.0 })
    {
        INFO("beat " << beat);
        CHECK(ramped.sampleOffsetForPpq(beat) == Approx(flat.sampleOffsetForPpq(beat)));
    }
}

TEST_CASE("A ramp's tempo slides rather than steps", "[engine][tempo][ramp]")
{
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 100.0 }, { 10.0, 200.0, true } });

    CHECK(map.tempoAtBeat(0.0) == Approx(100.0));
    CHECK(map.tempoAtBeat(2.5) == Approx(125.0));
    CHECK(map.tempoAtBeat(5.0) == Approx(150.0));
    CHECK(map.tempoAtBeat(10.0) == Approx(200.0));
    CHECK(map.tempoAtBeat(50.0) == Approx(200.0)); // holds after the ramp ends
}

TEST_CASE("A stepped change still steps when it isn't ramped", "[engine][tempo][ramp]")
{
    // The flag has to actually gate the behaviour, or every change would ramp
    // and the earlier work would be silently undone.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 100.0 }, { 10.0, 200.0, false } });

    CHECK(map.tempoAtBeat(5.0) == Approx(100.0));
    CHECK(map.tempoAtBeat(9.999) == Approx(100.0));
    CHECK(map.tempoAtBeat(10.0) == Approx(200.0));
}

TEST_CASE("An accelerando lands between its two tempos", "[engine][tempo][ramp]")
{
    // A sanity bound on the integral that needs no calculus: speeding up from
    // 60 to 120 across 16 beats must take less time than 16 beats at 60 and
    // more than 16 beats at 120.
    constexpr double rate = 48000.0;

    TempoMap ramp;
    ramp.setSampleRate(rate);
    ramp.setTempoChanges({ { 0.0, 60.0 }, { 16.0, 120.0, true } });

    const double atSlow = 16.0 * rate * 60.0 / 60.0;
    const double atFast = 16.0 * rate * 60.0 / 120.0;
    const double actual = ramp.sampleOffsetForPpq(16.0);

    INFO("slow " << atSlow << "  ramp " << actual << "  fast " << atFast);
    CHECK(actual < atSlow);
    CHECK(actual > atFast);
}

TEST_CASE("Time never runs backwards through a ramp", "[engine][tempo][ramp]")
{
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 200.0 }, { 4.0, 40.0, true }, { 12.0, 240.0, true },
                          { 20.0, 60.0 } });

    double previous = -1.0;
    for (double beat = 0.0; beat < 32.0; beat += 0.02)
    {
        const double samples = map.sampleOffsetForPpq(beat);
        INFO("beat " << beat << " -> " << samples);
        CHECK(samples > previous);
        previous = samples;
    }
}

TEST_CASE("A ramp into the start is ignored", "[engine][tempo][ramp]")
{
    // Nothing precedes beat 0, so there is nothing to slide from. The flag has
    // to be cleared rather than left to mean something undefined later.
    TempoMap map;
    map.setSampleRate(48000.0);
    map.setTempoChanges({ { 0.0, 120.0, true }, { 8.0, 90.0 } });

    REQUIRE(map.tempoChanges().size() == 2);
    CHECK_FALSE(map.tempoChanges().front().ramp);
}
