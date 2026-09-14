#include <catch2/catch_test_macros.hpp>

#include <engine/AudioFilePlayerNode.h>

#include <cmath>

using namespace soundsplice::engine;

namespace
{
    constexpr double kRate  = 48000.0;
    constexpr int    kBlock = 512;

    /** A mono clip of @p seconds at full scale, playing from the start. */
    std::shared_ptr<ClipData> fullScale(double seconds)
    {
        auto clip              = std::make_shared<ClipData>();
        clip->sourceSampleRate = kRate;
        clip->numChannels      = 1;
        clip->lengthSamples    = (int) (seconds * kRate);
        clip->audio.setSize(1, clip->lengthSamples);
        for (int i = 0; i < clip->lengthSamples; ++i)
            clip->audio.setSample(0, i, 1.0f);
        return clip;
    }

    /** @p blocks blocks of what @p player plays from the start of the song,
        at 120 bpm. */
    juce::AudioBuffer<float> play(AudioFilePlayerNode& player, int blocks)
    {
        constexpr double samplesPerBeat = kRate / 2.0;

        juce::AudioBuffer<float> out(2, kBlock * blocks);
        out.clear();

        for (int b = 0; b < blocks; ++b)
        {
            const int at = b * kBlock;

            ProcessContext context;
            context.sampleRate                = kRate;
            context.numSamples                = kBlock;
            context.transport.playing         = true;
            context.transport.bpm             = 120.0;
            context.transport.playheadSamples = at;
            context.transport.ppqPosition     = (double) at / samplesPerBeat;
            context.transport.ppqAtBlockEnd   = (double) (at + kBlock) / samplesPerBeat;

            juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, at, kBlock);
            juce::MidiBuffer         midi;
            player.process(view, midi, context);
        }

        return out;
    }

    void submit(AudioFilePlayerNode& player, std::shared_ptr<ClipData> clip, const ClipEnvelope& envelope)
    {
        player.prepare(kRate, kBlock);

        AudioClipSlot slot;
        slot.clipData    = std::move(clip);
        slot.startBeats  = 0.0;
        slot.lengthBeats = 1.0e9;
        slot.envelope    = envelope;

        auto* clips = new AudioFilePlayerNode::ClipList();
        clips->push_back(slot);
        player.submitClips(clips);
    }
}

TEST_CASE("A clip plays through its volume curve", "[gui][envelope]")
{
    SECTION("no curve plays at full level")
    {
        AudioFilePlayerNode player;
        submit(player, fullScale(1.0), {});
        const auto out = play(player, 20);
        REQUIRE(out.getSample(0, 5000) == 1.0f);
        REQUIRE(out.getSample(1, 5000) == 1.0f);
    }

    SECTION("a flat curve scales the whole clip")
    {
        ClipEnvelope envelope;
        envelope.addPoint(0.0, 0.5f);

        AudioFilePlayerNode player;
        submit(player, fullScale(1.0), envelope);
        const auto out = play(player, 20);
        REQUIRE(std::abs(out.getSample(0, 100) - 0.5f) < 1.0e-6f);
        REQUIRE(std::abs(out.getSample(1, 9000) - 0.5f) < 1.0e-6f);
    }

    SECTION("a ramp is followed sample by sample, in file time")
    {
        ClipEnvelope envelope;
        envelope.addPoint(0.0, 0.0f);
        envelope.addPoint(1.0, 1.0f);

        AudioFilePlayerNode player;
        submit(player, fullScale(1.0), envelope);
        const auto out = play(player, 94); // just past a second

        REQUIRE(std::abs(out.getSample(0, 0)) < 1.0e-4f);
        REQUIRE(std::abs(out.getSample(0, 24000) - 0.5f) < 1.0e-3f);
        REQUIRE(std::abs(out.getSample(0, 36000) - 0.75f) < 1.0e-3f);
    }
}
