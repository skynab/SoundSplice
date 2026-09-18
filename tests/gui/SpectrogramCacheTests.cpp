#include <catch2/catch_test_macros.hpp>

#include <juce_audio_utils/juce_audio_utils.h>

#include <app/SpectrogramCache.h>

#include <cmath>

using namespace soundsplice;

namespace
{
    juce::File writeToneWav(double hz, double seconds, double sampleRate = 48000.0)
    {
        auto file = juce::File::createTempFile(".wav");

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer(1, numSamples);
        for (int n = 0; n < numSamples; ++n)
            buffer.setSample(0, n, 0.5f * (float) std::sin(2.0 * 3.14159265358979 * hz * n / sampleRate));

        juce::WavAudioFormat                format;
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                         .withSampleRate(sampleRate)
                                                         .withNumChannels(1)
                                                         .withBitsPerSample(16));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);
        return file;
    }

    /** Pictures are made on a background thread and handed back through the
        message loop, so a test runs the loop until one arrives. */
    bool waitForPicture(const SpectrogramCache& cache, const juce::File& file, int timeoutMs = 10000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (const auto* picture = cache.find(file); picture != nullptr && picture->isReady())
                return true;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        }
        return false;
    }
}

TEST_CASE("A lane's spectrogram is made in the background and shows the file's tone", "[gui][spectrogram]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    const auto file = writeToneWav(1000.0, 3.0);
    int        updates = 0;
    {
        SpectrogramCache cache;
        cache.onUpdated = [&] { ++updates; };

        REQUIRE(cache.find(file) == nullptr);
        cache.ensure(file);
        REQUIRE(cache.find(file) != nullptr);   // asked for, not yet ready
        REQUIRE(waitForPicture(cache, file));
        REQUIRE(updates == 1);

        const auto* picture = cache.find(file);
        REQUIRE(picture->image.getHeight() == SpectrogramCache::kRows);
        REQUIRE(picture->windowSeconds > 0.0);

        // The brightest row mid-file is the tone's.
        const int column = picture->image.getWidth() / 2;
        int       brightest = 0;
        float     best      = -1.0f;
        for (int row = 0; row < picture->image.getHeight(); ++row)
            if (const float b = picture->image.getPixelAt(column, row).getPerceivedBrightness(); b > best)
            {
                best      = b;
                brightest = row;
            }
        const double proportion = 1.0 - (brightest + 0.5) / picture->image.getHeight();
        const double hz         = spectrogramimage::frequencyAt(proportion, 24000.0);
        REQUIRE(hz > 800.0);
        REQUIRE(hz < 1250.0);

        // A new style makes it again; forgetting it drops it.
        cache.setStyle(spectrogramimage::Scale::Linear, {});
        REQUIRE_FALSE(cache.find(file)->isReady());
        REQUIRE(waitForPicture(cache, file));

        cache.keepOnly({});
        REQUIRE(cache.find(file) == nullptr);

        // Destroyed with work queued: the thread stops, nothing is called.
        cache.ensure(file);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    file.deleteFile();
}
