#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/LoudnessReadout.h>
#include <engine/MasterBusNode.h>

#include <cmath>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

namespace
{
    /** Plays @p seconds of a 1 kHz sine at @p dbfs through @p master in 512-sample blocks. */
    void play(engine::MasterBusNode& master, double dbfs, double seconds, bool offline, double& phase)
    {
        constexpr double rate  = 48000.0;
        constexpr int    block = 512;

        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer         midi;
        engine::ProcessContext   context;
        context.sampleRate = rate;
        context.numSamples = block;
        context.offline    = offline;

        const double amplitude = std::pow(10.0, dbfs / 20.0);
        for (int done = 0; done < (int) (seconds * rate); done += block)
        {
            for (int i = 0; i < block; ++i)
            {
                const auto value = (float) (amplitude * std::sin(phase));
                buffer.setSample(0, i, value);
                buffer.setSample(1, i, value);
                phase += 2.0 * juce::MathConstants<double>::pi * 1000.0 / rate;
            }
            master.process(buffer, midi, context);
        }
    }
}

TEST_CASE("The master bus measures what it plays, not what it exports", "[gui][loudness]")
{
    engine::MasterBusNode master;
    master.prepare(48000.0, 512);
    double phase = 0.0;

    play(master, -23.0, 5.0, false, phase);
    auto reading = master.loudness();
    REQUIRE_THAT(reading.momentaryLufs, WithinAbs(-23.0, 0.1));
    REQUIRE_THAT(reading.shortTermLufs, WithinAbs(-23.0, 0.1));
    REQUIRE_THAT(reading.integratedLufs, WithinAbs(-23.0, 0.1));
    REQUIRE_THAT(reading.truePeakDb, WithinAbs(-23.0, 0.2)); // the sine's peak is -23 dBFS

    // An export's louder audio passes through without counting.
    play(master, -6.0, 5.0, true, phase);
    REQUIRE_THAT(master.loudness().integratedLufs, WithinAbs(-23.0, 0.1));

    // Preparing again at the same rate, as every export does, keeps the measurement.
    master.prepare(48000.0, 512);
    REQUIRE_THAT(master.loudness().integratedLufs, WithinAbs(-23.0, 0.1));

    // A reset starts again from the next block.
    master.resetLoudness();
    play(master, -33.0, 5.0, false, phase);
    REQUIRE_THAT(master.loudness().integratedLufs, WithinAbs(-33.0, 0.1));
}

TEST_CASE("The loudness readout shows the reading and flags a hot true peak", "[gui][loudness]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    LoudnessReadout readout;
    REQUIRE(readout.text() == "M -inf  S -inf  I -inf LUFS  LRA 0.0  TP -inf");
    REQUIRE_FALSE(readout.truePeakOverCeiling());

    engine::LiveLoudness reading;
    reading.momentaryLufs   = -14.26;
    reading.shortTermLufs   = -15.0;
    reading.integratedLufs  = -16.04;
    reading.loudnessRangeLu = 7.3;
    reading.truePeakDb      = -0.4;
    readout.setReading(reading);

    REQUIRE(readout.text() == "M -14.3  S -15.0  I -16.0 LUFS  LRA 7.3  TP -0.4");
    REQUIRE(readout.truePeakOverCeiling());
}
