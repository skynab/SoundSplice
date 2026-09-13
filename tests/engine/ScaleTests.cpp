#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include <engine/Scale.h>

using namespace looper::engine;

TEST_CASE("Every scale's intervals start on the root and stay within an octave", "[engine][scale]")
{
    for (auto type : { ScaleType::Major, ScaleType::NaturalMinor, ScaleType::MajorPentatonic,
                        ScaleType::MinorPentatonic, ScaleType::Dorian, ScaleType::Mixolydian,
                        ScaleType::Phrygian })
    {
        const auto intervals = intervalsForScale(type);
        REQUIRE(intervals.front() == 0);
        for (int interval : intervals)
        {
            REQUIRE(interval >= 0);
            REQUIRE(interval < 12);
        }
        // Sorted ascending, no repeats - a scale degree names one pitch class.
        for (size_t i = 1; i < intervals.size(); ++i)
            REQUIRE(intervals[i] > intervals[i - 1]);
    }
}

TEST_CASE("isInScale accepts every degreeToNote output, in every octave", "[engine][scale]")
{
    const Scale cMajor { ScaleType::Major, 60 };
    for (int degree = -14; degree <= 14; ++degree)
        REQUIRE(isInScale(degreeToNote(cMajor, degree), cMajor));
}

TEST_CASE("isInScale rejects the notes a scale actually leaves out", "[engine][scale]")
{
    const Scale cMajor { ScaleType::Major, 60 };
    // C major has no black keys: 61 (C#4), 63 (D#4), 66 (F#4), 68 (G#4), 70 (A#4).
    REQUIRE_FALSE(isInScale(61, cMajor));
    REQUIRE_FALSE(isInScale(63, cMajor));
    REQUIRE_FALSE(isInScale(66, cMajor));
    REQUIRE_FALSE(isInScale(68, cMajor));
    REQUIRE_FALSE(isInScale(70, cMajor));
}

TEST_CASE("snapToScale leaves an in-scale note untouched", "[engine][scale]")
{
    const Scale cMajor { ScaleType::Major, 60 };
    REQUIRE(snapToScale(60, cMajor) == 60); // root
    REQUIRE(snapToScale(64, cMajor) == 64); // E4, the third
}

TEST_CASE("snapToScale moves an out-of-scale note to an adjacent in-scale one", "[engine][scale]")
{
    const Scale cMajor { ScaleType::Major, 60 };
    const int snapped = snapToScale(61, cMajor); // C#4 sits between C4 and D4
    REQUIRE(isInScale(snapped, cMajor));
    REQUIRE(std::abs(snapped - 61) == 1);
}

TEST_CASE("snapToScale rounds an exact tie up", "[engine][scale]")
{
    // In C major, D#4/Eb4 (63) sits exactly one semitone from D4 (62, below)
    // and one semitone from E4 (64, above) - an equidistant tie.
    const Scale cMajor { ScaleType::Major, 60 };
    REQUIRE(snapToScale(63, cMajor) == 64);
}

TEST_CASE("degreeToNote(0) is the root", "[engine][scale]")
{
    const Scale cMajor { ScaleType::Major, 60 };
    REQUIRE(degreeToNote(cMajor, 0) == 60);
}

TEST_CASE("degreeToNote octave-wraps by exactly 12 semitones per scale length", "[engine][scale]")
{
    for (auto type : { ScaleType::Major, ScaleType::NaturalMinor, ScaleType::MajorPentatonic,
                        ScaleType::MinorPentatonic, ScaleType::Dorian, ScaleType::Mixolydian,
                        ScaleType::Phrygian })
    {
        const Scale scale { type, 60 };
        const int   size = (int) intervalsForScale(type).size();

        for (int degree = -10; degree <= 10; ++degree)
            REQUIRE(degreeToNote(scale, degree + size) == degreeToNote(scale, degree) + 12);
    }
}

TEST_CASE("degreeToNote handles negative degrees below the root", "[engine][scale]")
{
    const Scale cMajor { ScaleType::Major, 60 }; // C4
    REQUIRE(degreeToNote(cMajor, -1) == 59);      // B3, the leading tone below C4
    REQUIRE(degreeToNote(cMajor, -7) == 48);       // exactly one octave down (7-note scale)
}
