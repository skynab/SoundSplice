#include <catch2/catch_test_macros.hpp>

#include <engine/AudioRecorder.h>
#include <engine/RetroRecorder.h>

#include <vector>

using namespace soundsplice::engine;

namespace
{
    /** Feeds @p blocks blocks of 100 samples, each sample its own index from
        @p firstValue on, played from @p playhead on. Mono, so it's kept on
        both sides. */
    void feed(RetroRecorder& retro, int blocks, int64_t playhead, float firstValue, bool playing = true)
    {
        std::vector<float> block(100);
        for (int b = 0; b < blocks; ++b)
        {
            for (int n = 0; n < 100; ++n)
                block[(size_t) n] = firstValue + (float) (b * 100 + n);
            const float* channels[] { block.data() };
            retro.process(channels, 1, 100, playing, playhead + b * 100);
        }
    }
}

TEST_CASE("Retroactive recording keeps the latest run, placed where it was played", "[gui][recording]")
{
    RetroRecorder retro;
    retro.prepare(1000.0, 1.0); // room for 1000 samples
    REQUIRE(retro.isOn());

    juce::AudioBuffer<float> out;
    int64_t                  start = 0;
    REQUIRE_FALSE(retro.copyLatestRun(out, start)); // nothing played yet

    feed(retro, 5, 5000, 0.0f);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(start == 5000);
    REQUIRE(out.getNumSamples() == 500);
    REQUIRE(out.getSample(0, 0) == 0.0f);
    REQUIRE(out.getSample(1, 499) == 499.0f); // the mono input on both sides

    // Stopping keeps it; playing again from somewhere else starts a new run.
    feed(retro, 2, 0, 0.0f, false);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(start == 5000);
    feed(retro, 3, 20000, 1000.0f);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(start == 20000);
    REQUIRE(out.getNumSamples() == 300);
    REQUIRE(out.getSample(0, 0) == 1000.0f);

    // A run longer than the room keeps its end, and says where that starts.
    feed(retro, 15, 40000, 0.0f);
    REQUIRE(retro.copyLatestRun(out, start));
    REQUIRE(out.getNumSamples() == 1000);
    REQUIRE(start == 40500);
    REQUIRE(out.getSample(0, 0) == 500.0f);
    REQUIRE(out.getSample(0, 999) == 1499.0f);

    // While it may still be written over, the oldest end is left out.
    REQUIRE(retro.copyLatestRun(out, start, 200));
    REQUIRE(out.getNumSamples() == 800);
    REQUIRE(start == 40700);
    REQUIRE(out.getSample(0, 0) == 700.0f);

    retro.prepare(1000.0, 0.0);
    REQUIRE_FALSE(retro.isOn());
    REQUIRE_FALSE(retro.copyLatestRun(out, start));
}

TEST_CASE("A take is recorded in the chosen format, from the chosen input", "[gui][recording]")
{
    juce::TimeSliceThread thread("Test writer");
    thread.startThread();

    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("SoundSpliceFormatTest", ".wav");

    AudioRecorder recorder;
    recorder.prepare(48000.0, 2);
    recorder.setFormat({ 16, 1, 2 }); // 16-bit mono, from the third input
    REQUIRE(recorder.arm(file, thread));

    // Four inputs, each a different constant, so which was taken shows.
    std::vector<float> inputs[4];
    const float*       channels[4];
    for (int i = 0; i < 4; ++i)
    {
        inputs[i].assign(256, 0.1f * (float) (i + 1));
        channels[i] = inputs[i].data();
    }
    for (int block = 0; block < 10; ++block)
        recorder.process(channels, 4, 256, true, block * 256);
    recorder.disarm();
    recorder.process(channels, 4, 256, true, 2560);
    REQUIRE(recorder.isFinished());
    REQUIRE(recorder.finishTake() == file);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->numChannels == 1);
    REQUIRE(reader->bitsPerSample == 16);
    REQUIRE(reader->lengthInSamples == 2560);

    juce::AudioBuffer<float> read(1, 100);
    reader->read(&read, 0, 100, 1000, true, false);
    REQUIRE(std::abs(read.getSample(0, 50) - 0.3f) < 1.0e-3f); // the third input's level

    reader.reset();
    file.deleteFile();
    thread.stopThread(1000);
}

TEST_CASE("A sound-activated take waits for sound, and stops itself on silence", "[gui][recording]")
{
    juce::TimeSliceThread thread("Test writer");
    thread.startThread();

    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("SoundSpliceTriggerTest", ".wav");

    AudioRecorder recorder;
    recorder.prepare(48000.0, 2);
    recorder.setFormat({ 24, 1, 0 });
    recorder.setSoundTrigger(0.5f, 512); // stop after two silent blocks
    REQUIRE(recorder.arm(file, thread));

    std::vector<float> block(256);
    const float*       channels[] { block.data() };
    const auto play = [&](float level, int64_t playhead)
    {
        std::fill(block.begin(), block.end(), level);
        recorder.process(channels, 1, 256, true, playhead);
    };

    play(0.1f, 0);
    play(0.1f, 256);
    REQUIRE(recorder.isWaitingForSound()); // under the threshold: nothing yet
    REQUIRE(recorder.recordedSampleCount() == 0);

    play(0.8f, 512);                        // sound: the take starts here
    REQUIRE_FALSE(recorder.isWaitingForSound());
    REQUIRE(recorder.startPlayheadSamples() == 512);
    play(0.8f, 768);
    play(0.8f, 1024);
    play(0.0f, 1280);
    REQUIRE(recorder.isArmed());            // one silent block isn't enough
    play(0.0f, 1536);
    REQUIRE_FALSE(recorder.isArmed());      // two are: it stops itself
    play(0.0f, 1792);
    REQUIRE(recorder.isFinished());
    REQUIRE(recorder.stoppedOnSilence());
    REQUIRE(recorder.recordedSampleCount() == 5 * 256);

    REQUIRE(recorder.finishTake() == file);
    file.deleteFile();
    thread.stopThread(1000);
}
