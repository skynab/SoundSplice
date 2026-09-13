#include <catch2/catch_test_macros.hpp>

#include <app/PianoRoll.h>
#include <engine/PatternPlayback.h>

using namespace looper;

namespace
{
struct JuceFixture
{
    juce::ScopedJuceInitialiser_GUI juce;
};

/** Every note-on in @p midi, as (channel, noteNumber). */
std::vector<std::pair<int, int>> noteOnsIn(const juce::MidiBuffer& midi)
{
    std::vector<std::pair<int, int>> result;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn())
            result.push_back({ message.getChannel(), message.getNoteNumber() });
    }
    return result;
}
}

TEST_CASE("A palm-muted note is emitted on its own MIDI channel", "[engine][articulation]")
{
    // The articulation reaches the engine on the channel, which was hardcoded
    // to 1 and read by nothing. If this stops working the note still sounds —
    // it just stops chugging, silently — so it is worth pinning directly.
    JuceFixture fixture;

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.notes.push_back({ 0.0, 1.0, 40, 0.9f, engine::Articulation::PalmMute });
    pattern.notes.push_back({ 1.0, 1.0, 47, 0.9f, engine::Articulation::Normal });

    constexpr double samplesPerBeat = 22050.0; // 120bpm at 44.1k
    engine::ActiveNotes active {};

    juce::MidiBuffer midi;
    engine::PatternPlayback::emitBlock(midi, pattern, 0, samplesPerBeat,
                                       (int) (samplesPerBeat * 2.0), active);

    const auto ons = noteOnsIn(midi);
    REQUIRE(ons.size() == 2);

    // Palm-muted on 2, open on 1 — and the note numbers confirm which is which
    // rather than relying on emission order.
    for (const auto& [channel, note] : ons)
    {
        INFO("note " << note << " on channel " << channel);
        CHECK(channel == (note == 40 ? 2 : 1));
    }
}

TEST_CASE("An open note still goes out on channel 1", "[engine][articulation]")
{
    // The default has to be exactly what it always was: every other instrument
    // ignores the channel, but a change here would be invisible until someone
    // hit a synth patch that didn't.
    JuceFixture fixture;

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.notes.push_back({ 0.0, 1.0, 60, 0.8f });

    engine::ActiveNotes active {};
    juce::MidiBuffer midi;
    engine::PatternPlayback::emitBlock(midi, pattern, 0, 22050.0, 22050, active);

    const auto ons = noteOnsIn(midi);
    REQUIRE(ons.size() == 1);
    CHECK(ons[0].first == 1);
}

TEST_CASE("M toggles palm muting on the selected notes", "[gui][articulation]")
{
    JuceFixture fixture;

    PianoRoll roll;
    roll.setBounds(0, 0, 800, 500);

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.notes.push_back({ 0.0, 1.0, 40, 0.9f });
    pattern.notes.push_back({ 1.0, 1.0, 42, 0.9f });
    pattern.notes.push_back({ 2.0, 1.0, 44, 0.9f });
    roll.setPattern(pattern);

    int reported = 0;
    roll.onChange = [&](const engine::Pattern&) { ++reported; };

    roll.selectForTesting({ 0, 2 });

    REQUIRE(roll.togglePalmMuteOnSelection());
    CHECK(reported == 1);

    CHECK(roll.pattern().notes[0].articulation == engine::Articulation::PalmMute);
    CHECK(roll.pattern().notes[1].articulation == engine::Articulation::Normal); // untouched
    CHECK(roll.pattern().notes[2].articulation == engine::Articulation::PalmMute);

    // Again opens them back up: a mixed selection becomes all-muted, an
    // all-muted one becomes open. Toggling each note independently would make
    // a mixed selection scramble rather than change.
    REQUIRE(roll.togglePalmMuteOnSelection());
    CHECK(reported == 2);
    CHECK(roll.pattern().notes[0].articulation == engine::Articulation::Normal);
    CHECK(roll.pattern().notes[2].articulation == engine::Articulation::Normal);
}

TEST_CASE("A mixed selection becomes all muted, not scrambled", "[gui][articulation]")
{
    JuceFixture fixture;

    PianoRoll roll;
    roll.setBounds(0, 0, 800, 500);

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.notes.push_back({ 0.0, 1.0, 40, 0.9f, engine::Articulation::PalmMute });
    pattern.notes.push_back({ 1.0, 1.0, 42, 0.9f, engine::Articulation::Normal });
    roll.setPattern(pattern);

    roll.selectForTesting({ 0, 1 });
    REQUIRE(roll.togglePalmMuteOnSelection());

    for (const auto& note : roll.pattern().notes)
        CHECK(note.articulation == engine::Articulation::PalmMute);
}

TEST_CASE("M with nothing selected changes nothing", "[gui][articulation]")
{
    // And is left unconsumed, so the keystroke can mean something else to the
    // owner rather than silently doing nothing to everything.
    JuceFixture fixture;

    PianoRoll roll;
    roll.setBounds(0, 0, 800, 500);

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pattern.notes.push_back({ 0.0, 1.0, 40, 0.9f });
    roll.setPattern(pattern);

    int reported = 0;
    roll.onChange = [&](const engine::Pattern&) { ++reported; };

    CHECK_FALSE(roll.togglePalmMuteOnSelection());
    CHECK(reported == 0);
    CHECK(roll.pattern().notes[0].articulation == engine::Articulation::Normal);
}
