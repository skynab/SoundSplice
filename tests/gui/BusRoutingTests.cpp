#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/InstrumentTrack.h>

using Catch::Approx;
using namespace soundsplice::engine;

namespace
{
    constexpr double kRate  = 48000.0;
    constexpr int    kBlock = 256;

    ProcessContext blockContext()
    {
        ProcessContext context;
        context.sampleRate                = kRate;
        context.numSamples                = kBlock;
        context.transport.playing         = true;
        context.transport.bpm             = 120.0;
        context.transport.ppqPosition     = 0.0;
        context.transport.ppqAtBlockEnd   = (double) kBlock / (kRate / 2.0);
        return context;
    }

    /** A track playing a constant 0.5 on both channels. */
    void playConstant(InstrumentTrack& track)
    {
        auto clip              = std::make_shared<ClipData>();
        clip->sourceSampleRate = kRate;
        clip->numChannels      = 1;
        clip->lengthSamples    = (int) kRate;
        clip->audio.setSize(1, clip->lengthSamples);
        for (int i = 0; i < clip->lengthSamples; ++i)
            clip->audio.setSample(0, i, 0.5f);

        AudioClipSlot slot;
        slot.clipData    = clip;
        slot.lengthBeats = 1.0e9;
        auto* clips = new AudioFilePlayerNode::ClipList();
        clips->push_back(slot);
        track.audioPlayer.submitClips(clips);
    }
}

TEST_CASE("A routed track feeds its bus and its sends, pre and post fader", "[gui][routing]")
{
    InstrumentTrack source, bus, reverb;
    for (auto* track : { &source, &bus, &reverb })
    {
        track->prepare(kRate, kBlock);
        track->active.store(true);
    }
    bus.isBus.store(true);
    reverb.isBus.store(true);
    playConstant(source);
    source.gainDb.store(-6.0206f); // half

    const auto context = blockContext();
    juce::MidiBuffer midi;
    bus.busInput.clear();
    reverb.busInput.clear();

    // Output to the bus; a post-fader send at half to the reverb, and a
    // pre-fader one at a quarter.
    InstrumentTrack::Destinations to;
    to.output       = &bus.busInput;
    to.sends[0]     = &reverb.busInput;
    to.sendGains[0] = 0.5f;
    to.sends[1]     = &reverb.busInput;
    to.sendGains[1] = 0.25f;
    to.sendPreFader[1] = true;
    to.sendCount    = 2;
    source.renderRouted(midi, context, false, 0.0, to);

    REQUIRE(bus.busInput.getSample(0, 100) == Approx(0.25f).margin(1.0e-4));    // 0.5 at half
    REQUIRE(reverb.busInput.getSample(1, 100) == Approx(0.125f + 0.125f).margin(1.0e-4)); // post 0.25*0.5 + pre 0.5*0.25

    // The bus mixes what reached it, through its own fader, to the master.
    juce::AudioBuffer<float> master(2, kBlock);
    master.clear();
    bus.gainDb.store(0.0f);
    InstrumentTrack::Destinations out;
    out.output = &master;
    bus.renderRouted(midi, context, false, 0.0, out);
    REQUIRE(master.getSample(0, 100) == Approx(0.25f).margin(1.0e-4));

    // Muted, or left out by solo, a bus passes nothing on.
    master.clear();
    bus.muted.store(true);
    bus.renderRouted(midi, context, false, 0.0, out);
    REQUIRE(master.getSample(0, 100) == 0.0f);
}
