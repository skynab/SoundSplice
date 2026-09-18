#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <app/SpectralRender.h>

#include <cmath>

using namespace soundsplice;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    juce::File writeTwoTones(double seconds)
    {
        auto file = juce::File::createTempFile(".wav");
        const int n = (int) (seconds * kRate);
        juce::AudioBuffer<float> buffer(2, n);
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample(ch, i, 0.3f * (float) std::sin(2.0 * kPi * 1000.0 * i / kRate)
                                        + 0.3f * (float) std::sin(2.0 * kPi * 6000.0 * i / kRate));

        juce::WavAudioFormat                wav;
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(kRate)
                                                      .withNumChannels(2)
                                                      .withBitsPerSample(32)
                                                      .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer(buffer, 0, n);
        return file;
    }

    juce::AudioBuffer<float> readAll(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr)
            return {};
        juce::AudioBuffer<float> buffer((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read(&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
        return buffer;
    }

    double levelAt(const juce::AudioBuffer<float>& audio, int channel, double hz, int from, int to)
    {
        double re = 0.0, im = 0.0;
        for (int i = from; i < to; ++i)
        {
            re += audio.getSample(channel, i) * std::cos(2.0 * kPi * hz * i / kRate);
            im += audio.getSample(channel, i) * std::sin(2.0 * kPi * hz * i / kRate);
        }
        return 2.0 * std::hypot(re, im) / (to - from);
    }
}

TEST_CASE("A clip's spectral edits play from a cached render, the original untouched", "[gui][spectral]")
{
    juce::ScopedJuceInitialiser_GUI init;

    const auto source   = writeTwoTones(3.0);
    const auto original = readAll(source);

    // No edits: the file itself.
    REQUIRE(spectralrender::fileFor(source, {}) == source);

    engine::SpectralRegion region;
    region.startSeconds = 1.0;
    region.endSeconds   = 2.0;
    region.lowHz        = 4000.0;
    region.highHz       = 8000.0;
    region.gainDb       = engine::SpectralRegion::kSilenceDb;

    const auto rendered = spectralrender::fileFor(source, { region });
    REQUIRE(rendered != source);
    REQUIRE(rendered.existsAsFile());
    REQUIRE(spectralrender::fileFor(source, { region }) == rendered); // found again, not made again

    const auto edited = readAll(rendered);
    REQUIRE(edited.getNumChannels() == 2);
    REQUIRE(edited.getNumSamples() == original.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
    {
        REQUIRE(levelAt(edited, ch, 6000.0, 56000, 88000) < 0.01);
        REQUIRE(std::abs(levelAt(edited, ch, 1000.0, 56000, 88000) - 0.3) < 0.01);
    }

    // Outside the edit's reach, every sample is the original's.
    for (int i = 0; i < 20000; i += 7)
        REQUIRE(edited.getSample(0, i) == original.getSample(0, i));
    for (int i = 120000; i < original.getNumSamples(); i += 7)
        REQUIRE(edited.getSample(1, i) == original.getSample(1, i));

    // The source is as it was.
    const auto after = readAll(source);
    REQUIRE(after.getSample(0, 70000) == original.getSample(0, 70000));

    // A different edit is a different file.
    region.gainDb = -6.0f;
    const auto other = spectralrender::fileFor(source, { region });
    REQUIRE(other != rendered);

    rendered.deleteFile();
    other.deleteFile();
    source.deleteFile();
}
