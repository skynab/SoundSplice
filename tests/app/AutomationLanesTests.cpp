#include <catch2/catch_test_macros.hpp>

#include <app/AutomationLanes.h>

using namespace soundsplice;
using namespace soundsplice::automationlanes;

namespace
{
    model::AutomationLane ramp(float from, float to)
    {
        model::AutomationLane lane;
        lane.addPoint(0.0, from);
        lane.addPoint(4.0, to);
        return lane;
    }

    /** One track with a delay in slot 0. */
    model::Song songWithDelay()
    {
        model::Song song;
        auto&       track = model::addTrack(song, model::TrackType::Audio, "Synth");
        model::EffectSlot delay;
        delay.kind = model::EffectKind::Delay;
        track.effectChain.push_back(delay);
        return song;
    }
}

TEST_CASE("A lane key finds the master's, a track's or an effect's lane", "[app][automation]")
{
    auto song = songWithDelay();

    REQUIRE(laneFor(song, LaneKey::master()) == &song.masterGainDb);
    REQUIRE(laneFor(song, LaneKey::trackParam(0, model::TrackParam::Pan)) == &song.tracks[0].laneFor(model::TrackParam::Pan));

    auto* mix = laneFor(song, LaneKey::effect(0, 0, model::EffectKind::Delay, "mix"));
    REQUIRE(mix == &song.tracks[0].effectChain[0].automation["mix"]); // created on the way

    // Gone: a track that isn't there, or a slot that now holds another effect.
    REQUIRE(laneFor(song, LaneKey::trackParam(3, model::TrackParam::Gain)) == nullptr);
    REQUIRE(laneFor(song, LaneKey::effect(0, 0, model::EffectKind::Reverb, "mix")) == nullptr);
    REQUIRE(laneFor(song, LaneKey::effect(0, 1, model::EffectKind::Delay, "mix")) == nullptr);

    // Keys are equal only for the same lane.
    REQUIRE(LaneKey::trackParam(0, model::TrackParam::Gain) == LaneKey::trackParam(0, model::TrackParam::Gain));
    REQUIRE_FALSE(LaneKey::trackParam(0, model::TrackParam::Gain) == LaneKey::trackParam(0, model::TrackParam::Pan));
    REQUIRE_FALSE(LaneKey::trackParam(0, model::TrackParam::Gain) == LaneKey::master());
}

TEST_CASE("A stored lane goes where it was drawn, and an emptied one is erased", "[app][automation]")
{
    auto  song  = songWithDelay();
    auto& track = song.tracks[0];

    REQUIRE(storeLane(track, AutomationTarget::track(model::TrackParam::Gain), ramp(-12.0f, 0.0f)));
    REQUIRE(track.automation.at((int) model::TrackParam::Gain) == ramp(-12.0f, 0.0f));
    REQUIRE(storeLane(track, AutomationTarget::track(model::TrackParam::Gain), {}));
    REQUIRE(track.automation.count((int) model::TrackParam::Gain) == 0);

    const auto mix = AutomationTarget::effect(0, model::EffectKind::Delay, "mix");
    REQUIRE(storeLane(track, mix, ramp(0.0f, 1.0f)));
    REQUIRE(track.effectChain[0].automation.at("mix") == ramp(0.0f, 1.0f));
    REQUIRE(storeLane(track, mix, {}));
    REQUIRE(track.effectChain[0].automation.count("mix") == 0);

    // Drawn for an effect that's since been swapped out: refused.
    REQUIRE_FALSE(storeLane(track, AutomationTarget::effect(0, model::EffectKind::Reverb, "mix"), ramp(0.0f, 1.0f)));
    REQUIRE(track.effectChain[0].automation.empty());
}

TEST_CASE("Copying lanes moves only the lanes, to where the same tracks and effects still are", "[app][automation]")
{
    auto recorded = songWithDelay();
    recorded.masterGainDb                               = ramp(-6.0f, 0.0f);
    recorded.tracks[0].laneFor(model::TrackParam::Pan)  = ramp(-1.0f, 1.0f);
    recorded.tracks[0].effectChain[0].automation["mix"] = ramp(0.0f, 1.0f);

    // The same song, otherwise changed: its gain moved, and a track added.
    auto now = songWithDelay();
    now.tracks[0].gainDb = -3.0f;
    model::addTrack(now, model::TrackType::Audio, "Vox");
    REQUIRE(lanesDiffer(recorded, now));

    copyLanes(recorded, now);
    REQUIRE(now.masterGainDb == recorded.masterGainDb);
    REQUIRE(now.tracks[0].automation == recorded.tracks[0].automation);
    REQUIRE(now.tracks[0].effectChain[0].automation == recorded.tracks[0].effectChain[0].automation);
    REQUIRE(now.tracks[0].gainDb == -3.0f); // not a lane: left alone
    REQUIRE(now.tracks.size() == 2);
    REQUIRE_FALSE(lanesDiffer(recorded, now)); // nothing left to copy

    // A track that's a different one now, or a slot holding another effect,
    // doesn't take lanes meant for what was there.
    auto other = songWithDelay();
    other.tracks[0].id = recorded.tracks[0].id + 100;
    copyLanes(recorded, other);
    REQUIRE(other.tracks[0].automation.empty());

    auto swapped = songWithDelay();
    swapped.tracks[0].effectChain[0].kind = model::EffectKind::Reverb;
    copyLanes(recorded, swapped);
    REQUIRE(swapped.tracks[0].automation == recorded.tracks[0].automation);
    REQUIRE(swapped.tracks[0].effectChain[0].automation.empty());
}
