#include <catch2/catch_test_macros.hpp>

#include <engine/TimeStretch.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    std::vector<float> makeTone(int samples, double hz, float amplitude = 0.7f)
    {
        std::vector<float> out((size_t) samples);
        for (int n = 0; n < samples; ++n)
            out[(size_t) n] = amplitude * (float) std::sin(2.0 * fft::kPi * hz * n / kSampleRate);
        return out;
    }

    double magnitudeAtHz(const std::vector<float>& signal, double hz, int from, int count)
    {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < count; ++n)
        {
            const double phase = 2.0 * fft::kPi * hz * (double) n / kSampleRate;
            re += signal[(size_t) (from + n)] * std::cos(phase);
            im -= signal[(size_t) (from + n)] * std::sin(phase);
        }
        return std::sqrt(re * re + im * im) / (double) count;
    }

    /** The frequency with the most energy, searched over a range — how the
        pitch ratio is actually measured rather than assumed. */
    double dominantFrequency(const std::vector<float>& signal, double lowHz, double highHz,
                             int from, int count)
    {
        double best = lowHz, bestMagnitude = 0.0;
        for (double hz = lowHz; hz <= highHz; hz += 1.0)
        {
            const double magnitude = magnitudeAtHz(signal, hz, from, count);
            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                best          = hz;
            }
        }
        return best;
    }
}

// --- changeSpeed (varispeed) -----------------------------------------------

TEST_CASE("Changing speed scales the length inversely", "[engine][timestretch]")
{
    const auto input = makeTone(48000, 440.0);

    REQUIRE(timestretch::changeSpeed(input, 2.0).size() == 24000);
    REQUIRE(timestretch::changeSpeed(input, 0.5).size() == 96000);
}

TEST_CASE("Changing speed moves the pitch with it", "[engine][timestretch]")
{
    // The defining property of varispeed, and what separates it from a pitch
    // shift: twice as fast is an octave up.
    const auto input = makeTone(48000, 440.0);
    const auto faster = timestretch::changeSpeed(input, 2.0);

    const double measured = dominantFrequency(faster, 800.0, 1000.0, 2000, 8000);
    INFO("440Hz at double speed measured " << measured << "Hz");
    REQUIRE(std::abs(measured - 880.0) < 15.0);
}

TEST_CASE("Speed at unity is a no-op", "[engine][timestretch]")
{
    const auto input = makeTone(4096, 440.0);
    const auto out   = timestretch::changeSpeed(input, 1.0);

    REQUIRE(out.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE(std::abs(out[i] - input[i]) < 1.0e-4f);
}

TEST_CASE("A nonsense speed is refused rather than dividing by zero",
          "[engine][timestretch]")
{
    const auto input = makeTone(1000, 440.0);
    REQUIRE(timestretch::changeSpeed(input, 0.0).size() == input.size());
    REQUIRE(timestretch::changeSpeed(input, -2.0).size() == input.size());
}

// --- timeStretch -----------------------------------------------------------

TEST_CASE("Time stretching scales the length", "[engine][timestretch]")
{
    const auto input = makeTone(48000, 440.0);

    const auto longer  = timestretch::timeStretch(input, 2.0);
    const auto shorter = timestretch::timeStretch(input, 0.5);

    INFO("2.0 -> " << longer.size() << ", 0.5 -> " << shorter.size());
    REQUIRE(longer.size() > input.size() * 19 / 10);
    REQUIRE(longer.size() <= input.size() * 21 / 10);
    REQUIRE(shorter.size() > input.size() * 4 / 10);
    REQUIRE(shorter.size() <= input.size() * 6 / 10);
}

TEST_CASE("Time stretching leaves the pitch alone", "[engine][timestretch]")
{
    // The whole point: longer, same note. A stretch that moved the pitch
    // would just be a slow resample.
    const auto input   = makeTone(48000, 440.0);
    const auto longer  = timestretch::timeStretch(input, 1.5);

    const double measured = dominantFrequency(longer, 400.0, 480.0, 8000, 16000);
    INFO("440Hz stretched 1.5x measured " << measured << "Hz");
    REQUIRE(std::abs(measured - 440.0) < 8.0);
}

TEST_CASE("Time stretching at unity is a no-op", "[engine][timestretch]")
{
    const auto input = makeTone(8192, 440.0);
    const auto out   = timestretch::timeStretch(input, 1.0);
    REQUIRE(out == input);
}

TEST_CASE("A buffer too short to analyse is returned unchanged",
          "[engine][timestretch]")
{
    // Shorter than one FFT frame there is nothing to measure, and a frame of
    // mostly zero padding would describe the padding.
    const auto tiny = makeTone(100, 440.0);
    REQUIRE(timestretch::timeStretch(tiny, 2.0) == tiny);
}

// --- pitchShift ------------------------------------------------------------

TEST_CASE("Pitch shifting keeps the length exactly", "[engine][timestretch]")
{
    // A clip whose length drifted by a few samples per edit would slowly
    // slip against everything else on the timeline.
    const auto input = makeTone(48000, 440.0);

    for (double semitones : { -12.0, -5.0, 3.0, 7.0, 12.0 })
    {
        INFO(semitones << " semitones");
        REQUIRE(timestretch::pitchShift(input, semitones).size() == input.size());
    }
}

TEST_CASE("Pitch shifting moves the tone by the right ratio", "[engine][timestretch]")
{
    // The claim that matters, measured rather than assumed. An octave up
    // must land on 880Hz, an octave down on 220Hz.
    const auto input = makeTone(96000, 440.0);

    const auto up = timestretch::pitchShift(input, 12.0);
    const double measuredUp = dominantFrequency(up, 820.0, 940.0, 20000, 24000);
    INFO("+12 semitones measured " << measuredUp << "Hz");
    REQUIRE(std::abs(measuredUp - 880.0) < 20.0);

    const auto down = timestretch::pitchShift(input, -12.0);
    const double measuredDown = dominantFrequency(down, 190.0, 250.0, 20000, 24000);
    INFO("-12 semitones measured " << measuredDown << "Hz");
    REQUIRE(std::abs(measuredDown - 220.0) < 10.0);
}

TEST_CASE("A small pitch shift is still accurate", "[engine][timestretch]")
{
    // Fine shifts are the common case — correcting a take by a semitone or
    // two — and are where a vocoder that quantises to bin centres shows up.
    const auto input = makeTone(96000, 440.0);

    const auto shifted = timestretch::pitchShift(input, 2.0); // a whole tone up
    const double expected = 440.0 * std::pow(2.0, 2.0 / 12.0); // ~493.9Hz

    const double measured = dominantFrequency(shifted, 470.0, 520.0, 20000, 24000);
    INFO("expected " << expected << "Hz, measured " << measured << "Hz");
    REQUIRE(std::abs(measured - expected) < 8.0);
}

TEST_CASE("Pitch shifting by nothing is a no-op", "[engine][timestretch]")
{
    const auto input = makeTone(8192, 440.0);
    REQUIRE(timestretch::pitchShift(input, 0.0) == input);
}

TEST_CASE("Pitch shifted output stays finite and bounded", "[engine][timestretch]")
{
    const auto input = makeTone(48000, 220.0, 0.9f);

    for (double semitones : { -12.0, -7.0, 5.0, 12.0 })
    {
        INFO(semitones << " semitones");
        for (float sample : timestretch::pitchShift(input, semitones))
        {
            REQUIRE(std::isfinite(sample));
            REQUIRE(std::abs(sample) < 4.0f);
        }
    }
}

TEST_CASE("Pitch shifting an empty buffer is safe", "[engine][timestretch]")
{
    REQUIRE(timestretch::pitchShift({}, 5.0).empty());
}
