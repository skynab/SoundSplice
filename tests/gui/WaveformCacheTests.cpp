#include <catch2/catch_test_macros.hpp>

#include <juce_audio_utils/juce_audio_utils.h>

#include <app/WaveformCache.h>

using namespace looper;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** A real wav on disk, since a thumbnail's whole job is reading one. */
    juce::File writeTestWav(double seconds, double sampleRate = 44100.0)
    {
        auto file = juce::File::createTempFile(".wav");

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer(1, numSamples);
        for (int n = 0; n < numSamples; ++n)
            buffer.setSample(0, n, 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * (float) n
                                                   / (float) sampleRate));

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        auto writer = format.createWriterFor(stream, // consumed on success, left alone on failure
            juce::AudioFormatWriterOptions{}
                .withSampleRate(sampleRate)
                .withNumChannels(1)
                .withBitsPerSample(16));

        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);

        return file;
    }

    /** Thumbnails load on a background thread, so a test has to wait for one. */
    bool waitForLoad(juce::AudioThumbnail& thumbnail, int timeoutMs = 5000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (thumbnail.isFullyLoaded())
                return true;
            juce::Thread::sleep(10);
        }
        return thumbnail.isFullyLoaded();
    }
}

TEST_CASE("A cached thumbnail reports the file's real length", "[gui][waveform]")
{
    // The length is what the drawing rule divides by, so a wrong one puts the
    // waveform at the wrong scale everywhere.
    JuceFixture fixture;

    const auto file = writeTestWav(2.0);
    REQUIRE(file.existsAsFile());

    WaveformCache cache;
    cache.ensure(file);

    auto* thumbnail = cache.find(file);
    REQUIRE(thumbnail != nullptr);
    REQUIRE(waitForLoad(*thumbnail));
    REQUIRE(std::abs(thumbnail->getTotalLength() - 2.0) < 0.05);

    file.deleteFile();
}

TEST_CASE("The same file is only scanned once", "[gui][waveform]")
{
    // ensure() runs for every audio clip on every setSong, which happens on
    // every edit. Re-scanning each time would rebuild the whole timeline's
    // audio on every keystroke.
    JuceFixture fixture;

    const auto file = writeTestWav(0.5);
    WaveformCache cache;

    cache.ensure(file);
    auto* first = cache.find(file);
    REQUIRE(first != nullptr);

    cache.ensure(file);
    cache.ensure(file);
    REQUIRE(cache.find(file) == first); // same object, not a fresh scan

    file.deleteFile();
}

TEST_CASE("A file that isn't there produces no thumbnail, and no crash", "[gui][waveform]")
{
    // A project can name audio that has been moved or deleted since. The clip
    // still exists and still has to draw as a clip.
    JuceFixture fixture;

    WaveformCache cache;
    const juce::File missing("/nonexistent/definitely-not-here.wav");

    cache.ensure(missing);
    REQUIRE(cache.find(missing) == nullptr);
}

TEST_CASE("Different files get different thumbnails", "[gui][waveform]")
{
    JuceFixture fixture;

    const auto shortFile = writeTestWav(0.5);
    const auto longFile  = writeTestWav(2.0);

    WaveformCache cache;
    cache.ensure(shortFile);
    cache.ensure(longFile);

    auto* a = cache.find(shortFile);
    auto* b = cache.find(longFile);

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);

    REQUIRE(waitForLoad(*a));
    REQUIRE(waitForLoad(*b));
    REQUIRE(b->getTotalLength() > a->getTotalLength());

    shortFile.deleteFile();
    longFile.deleteFile();
}
