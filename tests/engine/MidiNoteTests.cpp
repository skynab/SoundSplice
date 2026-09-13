#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/MidiNote.h>

using Catch::Approx;
using looper::engine::isBlackKey;
using looper::engine::midiNoteName;
using looper::engine::midiNoteToHertz;

TEST_CASE("midiNoteToHertz maps reference pitches", "[engine][midi]")
{
    REQUIRE(midiNoteToHertz(69) == Approx(440.0));            // A4
    REQUIRE(midiNoteToHertz(57) == Approx(220.0));            // A3, one octave down
    REQUIRE(midiNoteToHertz(81) == Approx(880.0));            // A5, one octave up
    REQUIRE(midiNoteToHertz(60) == Approx(261.6255653).epsilon(0.0001)); // middle C
    REQUIRE(midiNoteToHertz(72) == Approx(523.2511306).epsilon(0.0001)); // C5
}

TEST_CASE("midiNoteToHertz honours a custom concert pitch", "[engine][midi]")
{
    REQUIRE(midiNoteToHertz(69, 432.0) == Approx(432.0));
    REQUIRE(midiNoteToHertz(81, 432.0) == Approx(864.0));
}

TEST_CASE("midiNoteName follows scientific pitch notation with middle C as C4", "[engine][midi]")
{
    REQUIRE(midiNoteName(60) == "C4");  // middle C
    REQUIRE(midiNoteName(61) == "C#4");
    REQUIRE(midiNoteName(69) == "A4");
    REQUIRE(midiNoteName(48) == "C3");
    REQUIRE(midiNoteName(72) == "C5");
    REQUIRE(midiNoteName(0)  == "C-1");
    REQUIRE(midiNoteName(127) == "G9");
}

TEST_CASE("isBlackKey identifies the five black keys per octave", "[engine][midi]")
{
    REQUIRE_FALSE(isBlackKey(60)); // C
    REQUIRE(isBlackKey(61));       // C#
    REQUIRE_FALSE(isBlackKey(62)); // D
    REQUIRE(isBlackKey(63));       // D#
    REQUIRE_FALSE(isBlackKey(64)); // E
    REQUIRE_FALSE(isBlackKey(65)); // F
    REQUIRE(isBlackKey(66));       // F#
    REQUIRE_FALSE(isBlackKey(67)); // G
    REQUIRE(isBlackKey(68));       // G#
    REQUIRE_FALSE(isBlackKey(69)); // A
    REQUIRE(isBlackKey(70));       // A#
    REQUIRE_FALSE(isBlackKey(71)); // B

    // Same pattern one octave up/down.
    REQUIRE(isBlackKey(61 + 12));
    REQUIRE(isBlackKey(61 - 12));
}
