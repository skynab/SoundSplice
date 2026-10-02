#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <app/BatchProcess.h>

#include <cmath>

using Catch::Approx;
using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** Writes three seconds of a 440 Hz tone at @p level to @p file. */
    void writeTone(const juce::File& file, float level)
    {
        juce::AudioBuffer<float> tone(2, 3 * 48000);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                tone.setSample(ch, i, level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0));
        engine::ExportOptions options;
        options.sampleRate    = 48000.0;
        options.bitsPerSample = 32;
        REQUIRE(engine::writeAudioFile(file, tone, options));
    }

    double loudnessOf(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        engine::audioformats::registerAll(formats);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        REQUIRE(reader != nullptr);
        juce::AudioBuffer<float> audio((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);

        engine::LoudnessMeter meter;
        meter.prepare(reader->sampleRate, 2);
        const float* both[2] { audio.getReadPointer(0), audio.getReadPointer(1) };
        meter.process(both, 2, audio.getNumSamples());
        return engine::LoudnessReport::of(meter, audio.getNumSamples() / reader->sampleRate).integratedLufs;
    }
}

TEST_CASE("Batch Process brings each file in a folder to the loudness target, as new files", "[gui][batch]")
{
    JuceFixture fixture;

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("soundsplice-batch-test");
    root.deleteRecursively();
    const auto in  = root.getChildFile("in");
    const auto out = root.getChildFile("out");
    in.createDirectory();
    out.createDirectory();
    writeTone(in.getChildFile("quiet.wav"), 0.05f);
    writeTone(in.getChildFile("loud.wav"), 0.5f);
    in.getChildFile("notes.txt").replaceWithText("not audio");

    const auto inputs = batch::audioFilesIn(in);
    REQUIRE(inputs.size() == 2);
    REQUIRE(inputs[0].getFileName() == "loud.wav");

    batch::Settings settings;
    settings.loudnessLufs = -23.0;
    settings.folder       = out;
    engine::PluginHost plugins;
    for (const auto& input : inputs)
    {
        const auto result = batch::processFile(input, settings, plugins);
        REQUIRE(result.ok);
        REQUIRE(result.written.getParentDirectory() == out);
        REQUIRE(loudnessOf(result.written) == Approx(-23.0).margin(0.3));
    }

    // The originals are as they were.
    REQUIRE(loudnessOf(in.getChildFile("loud.wav")) > -15.0);

    // Writing over its own folder never writes over the input.
    settings.folder = in;
    REQUIRE(batch::outputFor(in.getChildFile("loud.wav"), settings).getFileName() == "loud processed.wav");

    root.deleteRecursively();
}

TEST_CASE("A file that can't be read is reported, not written", "[gui][batch]")
{
    JuceFixture fixture;
    const auto bogus = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("soundsplice-not-audio.wav");
    bogus.replaceWithText("not audio at all");

    batch::Settings settings;
    settings.folder = bogus.getParentDirectory().getChildFile("soundsplice-batch-out");
    engine::PluginHost plugins;
    const auto result = batch::processFile(bogus, settings, plugins);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.isNotEmpty());
    bogus.deleteFile();
}
