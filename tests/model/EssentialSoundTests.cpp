#include <catch2/catch_test_macros.hpp>

#include <model/EssentialSound.h>
#include <model/Serialization.h>

using namespace soundsplice::model;

namespace
{
    Clip audioClip()
    {
        Clip clip;
        clip.type = ClipType::Audio;
        clip.effects.push_back(makeEffectSlot(EffectKind::Reverb)); // added by hand
        return clip;
    }
}

TEST_CASE("Essential Sound amounts become effects in front of the clip's own", "[model][essential]")
{
    auto clip = audioClip();
    essential::setRole(clip, SoundRole::Dialogue);
    REQUIRE(clip.effects.size() == 1); // nothing yet: no amounts

    essential::setAmount(clip, "rumble", 5.0f);
    essential::setAmount(clip, "dynamics", 3.0f);
    REQUIRE(clip.effects.size() == 3);
    REQUIRE(clip.effects[0].kind == EffectKind::Filter);
    REQUIRE(clip.effects[0].essential);
    REQUIRE(clip.effects[0].filter.cutoff == 100.0f); // 40 + 12 * 5
    REQUIRE(clip.effects[1].kind == EffectKind::Compressor);
    REQUIRE(clip.effects[2].kind == EffectKind::Reverb);
    REQUIRE_FALSE(clip.effects[2].essential);

    // Back to 0 takes the effect out; the hand-added one stays.
    essential::setAmount(clip, "rumble", 0.0f);
    REQUIRE(clip.effects.size() == 2);
    REQUIRE(clip.effects[0].kind == EffectKind::Compressor);

    // A new role starts over.
    essential::setRole(clip, SoundRole::Ambience);
    REQUIRE(clip.effects.size() == 1);
    REQUIRE(clip.essential.amounts.empty());
}

TEST_CASE("Each role offers the tasks that make sense for it", "[model][essential]")
{
    REQUIRE(essential::tasksFor(SoundRole::Dialogue).size() == 5);
    REQUIRE(essential::tasksFor(SoundRole::None).empty());

    // An amount for a task the role doesn't offer makes nothing.
    auto clip = audioClip();
    essential::setRole(clip, SoundRole::Music);
    essential::setAmount(clip, "noise", 8.0f);
    REQUIRE(clip.effects.size() == 1);
}

TEST_CASE("Essential Sound round-trips with its effects", "[model][essential][io]")
{
    Song song;
    const int id = addTrack(song, TrackType::Audio, "vo").id;
    auto      clip = audioClip();
    essential::setRole(clip, SoundRole::Dialogue);
    essential::setAmount(clip, "deEss", 4.5f);
    essential::setAmount(clip, "clarity", 6.0f);
    addClip(song, id, clip);

    Song restored;
    REQUIRE(deserialize(serialize(song), restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[0].clips[0].effects[0].essential);
    REQUIRE(restored.tracks[0].clips[0].essential.amounts.at("deEss") == 4.5f);
}
