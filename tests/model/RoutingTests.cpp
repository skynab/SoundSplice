#include <catch2/catch_test_macros.hpp>

#include <model/Routing.h>
#include <model/Serialization.h>
#include <model/TimeSelection.h>

using namespace soundsplice::model;

namespace
{
    struct Ids
    {
        int drums = 0, vocal = 0, band = 0, reverb = 0;
    };

    Song mix(Ids& ids)
    {
        Song song;
        ids.drums  = addTrack(song, TrackType::Audio, "Drums").id;
        ids.vocal  = addTrack(song, TrackType::Audio, "Vocal").id;
        ids.band   = addTrack(song, TrackType::Bus, "Band").id;
        ids.reverb = addTrack(song, TrackType::Bus, "Reverb").id;
        return song;
    }
}

TEST_CASE("Tracks route to buses and send to them, never in a loop", "[model][routing]")
{
    Ids  ids;
    auto song = mix(ids);

    REQUIRE(routing::setOutput(song, ids.drums, ids.band));
    REQUIRE(routing::addSend(song, ids.vocal, ids.reverb, -6.0f));
    REQUIRE(routing::addSend(song, ids.band, ids.reverb, -12.0f, true));
    REQUIRE(routing::feeds(song, ids.drums, ids.reverb)); // through the band bus

    // The reverb can't feed what already feeds it, nor a bus itself; and a
    // second send to the same bus is refused.
    REQUIRE_FALSE(routing::setOutput(song, ids.reverb, ids.band));
    REQUIRE_FALSE(routing::addSend(song, ids.reverb, ids.reverb));
    REQUIRE_FALSE(routing::addSend(song, ids.vocal, ids.reverb));
    REQUIRE_FALSE(routing::setOutput(song, ids.drums, ids.vocal)); // not a bus

    REQUIRE(routing::busesFor(song, ids.reverb).empty());
    REQUIRE(routing::busesFor(song, ids.vocal) == std::vector<int> { ids.band, ids.reverb });
}

TEST_CASE("A routing to a bus that has gone is the master", "[model][routing]")
{
    Ids  ids;
    auto song = mix(ids);
    routing::setOutput(song, ids.drums, ids.band);
    REQUIRE(routing::outputIndex(song, 0) == 2);

    removeTrack(song, ids.band);
    REQUIRE(routing::outputIndex(song, 0) == -1);
    REQUIRE(routing::busById(song, ids.band) == nullptr);
}

TEST_CASE("A bus holds no clips", "[model][routing]")
{
    Ids  ids;
    auto song = mix(ids);
    REQUIRE_FALSE(routing::holdsClips(song.tracks[2]));
    REQUIRE_FALSE(rangeedit::fits(song.tracks[2], ClipType::Instrument));
    REQUIRE_FALSE(rangeedit::fits(song.tracks[2], ClipType::Audio));
    REQUIRE_FALSE(rangeedit::appliesTo(song.tracks[2]));
}

TEST_CASE("Buses, outputs and sends round-trip", "[model][routing][io]")
{
    Ids  ids;
    auto song = mix(ids);
    routing::setOutput(song, ids.drums, ids.band);
    routing::addSend(song, ids.vocal, ids.reverb, -6.5f);
    routing::addSend(song, ids.band, ids.reverb, -12.0f, true);

    const auto text = serialize(song);
    Song       restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[2].type == TrackType::Bus);
    REQUIRE(restored.tracks[2].sends[0].preFader);
}

TEST_CASE("A compressor's sidechain round-trips, and its own input writes nothing", "[model][routing][io]")
{
    Ids  ids;
    auto song = mix(ids);
    auto& vocal = song.tracks[1];
    vocal.effectChain.push_back(makeEffectSlot(EffectKind::Compressor));
    vocal.effectChain.push_back(makeEffectSlot(EffectKind::Gate));
    vocal.effectChain[0].sidechainTrackId = ids.drums;

    const auto text = serialize(song);
    REQUIRE(text.find("FXKEY " + std::to_string(ids.drums)) != std::string::npos);
    REQUIRE(text.find("FXKEY") == text.rfind("FXKEY"));

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
    REQUIRE(canBeKeyed(EffectKind::Gate));
    REQUIRE_FALSE(canBeKeyed(EffectKind::Reverb));
}
