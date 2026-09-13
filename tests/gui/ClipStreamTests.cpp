#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <engine/AudioFilePlayerNode.h>
#include <engine/ClipStream.h>

#include <cmath>

using namespace soundsplice::engine;

namespace
{
    /** An AudioFormatReader over a buffer in memory, so a stream can be
        tested without touching the disk. */
    class BufferReader final : public juce::AudioFormatReader
    {
    public:
        BufferReader(juce::AudioBuffer<float> audio, double rate)
            : juce::AudioFormatReader(nullptr, "buffer"), audio_(std::move(audio))
        {
            sampleRate            = rate;
            numChannels           = (unsigned int) audio_.getNumChannels();
            lengthInSamples       = audio_.getNumSamples();
            bitsPerSample         = 32;
            usesFloatingPointData = true;
        }

        bool readSamples(int* const* dest, int numDest, int offset, juce::int64 start, int count) override
        {
            for (int ch = 0; ch < numDest; ++ch)
                if (dest[ch] != nullptr)
                    std::copy_n(audio_.getReadPointer(juce::jmin(ch, audio_.getNumChannels() - 1)) + start, count,
                                (float*) dest[ch] + offset);
            return true;
        }

    private:
        juce::AudioBuffer<float> audio_;
    };

    /** Stereo audio where every sample differs from its neighbours. */
    juce::AudioBuffer<float> tone(int frames)
    {
        juce::AudioBuffer<float> audio(2, frames);
        for (int i = 0; i < frames; ++i)
        {
            audio.setSample(0, i, std::sin((float) i * 0.013f) * 0.5f);
            audio.setSample(1, i, std::cos((float) i * 0.007f) * 0.25f);
        }
        return audio;
    }

    auto epochSource(std::uint64_t start = 1)
    {
        return std::make_shared<std::atomic<std::uint64_t>>(start);
    }

    std::unique_ptr<juce::AudioFormatReader> readerOver(const juce::AudioBuffer<float>& audio, double rate)
    {
        return std::make_unique<BufferReader>(audio, rate);
    }

    /** Plays @p clip from the start through a fresh player for @p blocks
        blocks of 512 at 48 kHz and 120 bpm, starting at @p fromSample. */
    juce::AudioBuffer<float> play(AudioFilePlayerNode& player, std::uint64_t epoch, bool offline, int fromSample,
                                  int blocks)
    {
        constexpr int    block          = 512;
        constexpr double rate           = 48000.0;
        constexpr double samplesPerBeat = rate / 2.0; // 120 bpm

        juce::AudioBuffer<float> out(2, block * blocks);
        out.clear();

        for (int b = 0; b < blocks; ++b)
        {
            const int at = fromSample + b * block;

            ProcessContext context;
            context.sampleRate              = rate;
            context.numSamples              = block;
            context.streamEpoch             = epoch;
            context.offline                 = offline;
            context.transport.playing       = true;
            context.transport.bpm           = 120.0;
            context.transport.playheadSamples = at;
            context.transport.ppqPosition   = (double) at / samplesPerBeat;
            context.transport.ppqAtBlockEnd = (double) (at + block) / samplesPerBeat;

            juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, b * block, block);
            juce::MidiBuffer         midi;
            player.process(view, midi, context);
        }

        return out;
    }

    void submit(AudioFilePlayerNode& player, std::shared_ptr<ClipData> clip)
    {
        player.prepare(48000.0, 512);
        auto* clips = new AudioFilePlayerNode::ClipList();
        clips->push_back({ std::move(clip), 0.0, 1.0e9 });
        player.submitClips(clips);
    }
}

TEST_CASE("A stream loads the pages around where it's played", "[gui][clipstream]")
{
    const auto audio  = tone(300000); // five pages
    auto       epoch  = epochSource();
    ClipStream stream(readerOver(audio, 48000.0), epoch);

    REQUIRE(stream.numPages() == 5);
    REQUIRE(stream.loadedPage(0) == nullptr);

    stream.noteReading(0, 70000, 1);
    REQUIRE(stream.service(100) == 5); // fifteen seconds ahead covers the whole file

    for (int page = 0; page < 5; ++page)
    {
        const auto* loaded = stream.loadedPage(page);
        REQUIRE(loaded != nullptr);

        const int start = page << ClipStream::kPageShift;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < loaded->audio.getNumSamples(); i += 997)
                REQUIRE(loaded->audio.getSample(ch, i) == audio.getSample(ch, start + i));
    }

    // Nothing more to do.
    REQUIRE(stream.service(100) == 0);
}

TEST_CASE("Pages far from every reader are let go, and freed only after the audio thread moves on", "[gui][clipstream]")
{
    // At 100 Hz the read-ahead and keep distances are far smaller than a page,
    // so which pages stay is easy to reason about.
    const auto audio = tone(20 * (int) ClipStream::kPageFrames);
    auto       epoch = epochSource();
    ClipStream stream(readerOver(audio, 100.0), epoch);

    stream.noteReading(0, 10, 1);
    stream.service(100);
    REQUIRE(stream.loadedPage(0) != nullptr);
    const int retiredBefore = stream.retiredPageCount();

    stream.noteReading(0, 10 * ClipStream::kPageFrames + 5, 1);
    stream.service(100);

    REQUIRE(stream.loadedPage(10) != nullptr);
    REQUIRE(stream.loadedPage(0) == nullptr);
    const int retired = stream.retiredPageCount();
    REQUIRE(retired > retiredBefore);

    // The block that might have been reading them hasn't finished.
    stream.freeRetired();
    REQUIRE(stream.retiredPageCount() == retired);

    epoch->fetch_add(1);
    stream.freeRetired();
    REQUIRE(stream.retiredPageCount() == 0);
}

TEST_CASE("A stream nobody is playing loads nothing, and eventually lets everything go", "[gui][clipstream]")
{
    const auto audio = tone(200000);
    auto       epoch = epochSource(10);
    ClipStream stream(readerOver(audio, 48000.0), epoch);

    REQUIRE(stream.pageNow(1) != nullptr); // an offline render loads on the spot
    REQUIRE(stream.loadedPageCount() == 1);

    stream.noteReading(0, 0, 10);
    epoch->store(10 + ClipStream::kFreshEpochs + 1);
    REQUIRE(stream.service(100) == 0);
    REQUIRE(stream.loadedPageCount() == 1); // stopped for a while: kept

    epoch->store(10 + ClipStream::kIdleEpochs + 1);
    stream.service(100);
    REQUIRE(stream.loadedPageCount() == 0);
}

TEST_CASE("A streamed clip plays exactly like the same clip in memory", "[gui][clipstream]")
{
    // A rate unlike the device's, so every output sample interpolates, and
    // long enough that the blocks below cross page edges.
    const auto audio = tone(200000);
    auto       epoch = epochSource();

    auto inMemory              = std::make_shared<ClipData>();
    inMemory->audio            = audio;
    inMemory->sourceSampleRate = 44100.0;
    inMemory->numChannels      = 2;
    inMemory->lengthSamples    = audio.getNumSamples();

    auto streamed              = std::make_shared<ClipData>();
    streamed->sourceSampleRate = 44100.0;
    streamed->numChannels      = 2;
    streamed->lengthSamples    = audio.getNumSamples();
    streamed->stream           = std::make_shared<ClipStream>(readerOver(audio, 44100.0), epoch);

    AudioFilePlayerNode memoryPlayer, streamPlayer;
    submit(memoryPlayer, inMemory);
    submit(streamPlayer, streamed);

    constexpr int from = 60000, blocks = 300;
    const auto expected = play(memoryPlayer, 1, false, from, blocks);

    SECTION("before anything is loaded, playback is silent rather than waiting")
    {
        const auto live = play(streamPlayer, 1, false, from, 4);
        REQUIRE(live.getMagnitude(0, live.getNumSamples()) == 0.0f);

        // ...and says where it was, so the loader can catch up.
        streamed->stream->service(100);
        REQUIRE(streamed->stream->loadedPage(1) != nullptr);

        const auto caughtUp = play(streamPlayer, 2, false, from, blocks);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < expected.getNumSamples(); ++i)
                REQUIRE(caughtUp.getSample(ch, i) == expected.getSample(ch, i));
    }

    SECTION("an offline render never skips")
    {
        const auto rendered = play(streamPlayer, 1, true, from, blocks);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < expected.getNumSamples(); ++i)
                REQUIRE(rendered.getSample(ch, i) == expected.getSample(ch, i));
    }
}
