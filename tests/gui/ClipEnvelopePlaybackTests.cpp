#include <catch2/catch_test_macros.hpp>

#include <engine/AudioFilePlayerNode.h>
#include <engine/InstrumentTrack.h>

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

TEST_CASE("Overlapping clips on a track mix, and each stops at the end of its window", "[gui][envelope]")
{
    AudioFilePlayerNode player;
    player.prepare(kRate, kBlock);

    // At 120 bpm a beat is 24000 samples. The first clip's window ends at
    // 1.1 beats, inside a block; the second starts at 1.0 beat, also inside one.
    AudioClipSlot first;
    first.clipData    = fullScale(2.0);
    first.startBeats  = 0.0;
    first.lengthBeats = 1.1;
    first.gain        = 0.25f;

    AudioClipSlot second = first;
    second.startBeats    = 1.0;
    second.lengthBeats   = 1.0;
    second.gain          = 0.5f;

    auto* clips = new AudioFilePlayerNode::ClipList();
    clips->push_back(first);
    clips->push_back(second);
    player.submitClips(clips);

    const auto out = play(player, 120);
    // A sample either side of each edge, which fall inside blocks.
    REQUIRE(out.getSample(0, 23998) == 0.25f);         // the first alone
    REQUIRE(out.getSample(0, 24002) == 0.75f);         // both, once the second starts
    REQUIRE(out.getSample(0, 26398) == 0.75f);
    REQUIRE(out.getSample(0, 26402) == 0.5f);          // the first stops at 1.1 beats
    REQUIRE(out.getSample(0, 47998) == 0.5f);
    REQUIRE(out.getSample(0, 48002) == 0.0f);          // and the second at 2.0
}

TEST_CASE("A clip's own effects touch that clip and not the one overlapping it", "[gui][envelope][effects]")
{
    AudioFilePlayerNode player;
    player.prepare(kRate, kBlock);

    // Polarity flipped on both sides: exact, so the mix can be checked to the bit.
    auto chain = std::make_shared<EffectChain>();
    chain->add(makeBuiltInNode(EffectKind::Invert));
    chain->prepare(kRate, kBlock);
    EffectParamValues invert;
    invert.kind    = EffectKind::Invert;
    invert.enabled = true;
    invert.set("left", 1.0);
    invert.set("right", 1.0);
    chain->applyParams(0, invert);

    AudioClipSlot first;
    first.clipData    = fullScale(2.0);
    first.startBeats  = 0.0;
    first.lengthBeats = 1.1;
    first.gain        = 0.25f;
    first.effects     = chain;

    AudioClipSlot second = first;
    second.startBeats    = 1.0;
    second.lengthBeats   = 1.0;
    second.gain          = 0.5f;
    second.effects       = nullptr;

    auto* clips = new AudioFilePlayerNode::ClipList();
    clips->push_back(first);
    clips->push_back(second);
    player.submitClips(clips);

    const auto out = play(player, 120);
    REQUIRE(out.getSample(0, 23998) == -0.25f); // the first, inverted
    REQUIRE(out.getSample(1, 23998) == -0.25f);
    REQUIRE(out.getSample(0, 24002) == 0.25f);  // with the second, which isn't
    REQUIRE(out.getSample(0, 26402) == 0.5f);   // the second alone
}

TEST_CASE("Delay compensation lines a track with a latent effect up with one without", "[gui][envelope][effects]")
{
    // A click, played on two tracks: one through a limiter, whose lookahead
    // makes it late, and one straight. Aligned to the limiter's latency, the
    // click lands on the same sample in both.
    auto click              = std::make_shared<ClipData>();
    click->sourceSampleRate = kRate;
    click->numChannels      = 1;
    click->lengthSamples    = (int) kRate;
    click->audio.setSize(1, click->lengthSamples);
    click->audio.clear();
    click->audio.setSample(0, 1000, 0.5f);

    const auto limiterValues = []
    {
        EffectParamValues values;
        values.kind    = EffectKind::Limiter;
        values.enabled = true;
        values.set("input", 0.0);
        values.set("ceiling", 0.0);
        values.set("release", 100.0);
        return values;
    }();

    const auto peakAt = [&](bool limited, int alignTo, int& latencyOut)
    {
        InstrumentTrack track;
        track.prepare(kRate, kBlock);
        track.active.store(true);

        AudioClipSlot slot;
        slot.clipData    = click;
        slot.lengthBeats = 1.0e9;
        auto* clips = new AudioFilePlayerNode::ClipList();
        clips->push_back(slot);
        track.audioPlayer.submitClips(clips);

        if (limited)
        {
            auto* chain = new EffectChain();
            chain->add(makeBuiltInNode(EffectKind::Limiter));
            chain->prepare(kRate, kBlock);
            chain->applyParams(0, limiterValues);
            track.setEffectChain(chain);
        }
        latencyOut = track.chainLatency();

        constexpr double samplesPerBeat = kRate / 2.0;
        juce::AudioBuffer<float> out(2, kBlock * 8);
        out.clear();
        for (int b = 0; b < 8; ++b)
        {
            const int      at = b * kBlock;
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
            track.render(view, midi, context, false, false, 0.0, alignTo);
        }

        int peak = 0;
        for (int i = 1; i < out.getNumSamples(); ++i)
            if (std::abs(out.getSample(0, i)) > std::abs(out.getSample(0, peak)))
                peak = i;
        return peak;
    };

    int latency = 0, none = 0;
    peakAt(true, 0, latency);
    REQUIRE(latency > 0); // the lookahead is reported

    const int late     = peakAt(true, latency, latency);
    const int straight = peakAt(false, latency, none);
    REQUIRE(none == 0);
    REQUIRE(late == 1000 + latency);
    REQUIRE(straight == late); // held back to meet it

    int unaligned = 0;
    REQUIRE(peakAt(false, 0, unaligned) == 1000); // with nothing to meet, untouched
}
