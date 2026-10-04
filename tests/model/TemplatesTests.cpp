#include <catch2/catch_test_macros.hpp>

#include "model/Serialization.h"
#include "model/Templates.h"

#include <set>
#include <string>

using namespace soundsplice::model;

namespace
{
    const Track* named(const Song& song, const std::string& name)
    {
        for (const auto& track : song.tracks)
            if (track.name == name)
                return &track;
        return nullptr;
    }
}

TEST_CASE("Every built-in template is a project with tracks and no audio", "[model][templates]")
{
    std::set<std::string> names;
    for (const auto which : templates::kAll)
    {
        INFO(templates::name(which));
        REQUIRE(names.insert(templates::name(which)).second);
        REQUIRE_FALSE(std::string(templates::description(which)).empty());

        const auto song = templates::make(which);
        REQUIRE_FALSE(song.tracks.empty());
        for (const auto& track : song.tracks)
        {
            REQUIRE(track.clips.empty());
            for (const auto& slot : track.effectChain)
                REQUIRE(slot.enabled);
        }

        // Routing only ever points at a bus that's there.
        for (const auto& track : song.tracks)
        {
            if (track.outputBusId != 0)
                REQUIRE(findTrack(song, track.outputBusId)->type == TrackType::Bus);
            for (const auto& send : track.sends)
                REQUIRE(findTrack(song, send.busId)->type == TrackType::Bus);
            for (const auto& slot : track.effectChain)
                if (slot.sidechainTrackId != 0)
                    REQUIRE(findTrack(song, slot.sidechainTrackId) != nullptr);
        }

        // And it saves and opens like any project.
        Song reopened;
        REQUIRE(deserialize(serialize(song), reopened));
        REQUIRE(reopened == song);
    }
}

TEST_CASE("The templates are set up for their work", "[model][templates]")
{
    const auto podcast = templates::make(templates::Builtin::Podcast);
    const auto* voices = named(podcast, "Voices");
    REQUIRE(voices != nullptr);
    REQUIRE(named(podcast, "Host")->outputBusId == voices->id);
    REQUIRE(named(podcast, "Guest")->outputBusId == voices->id);
    REQUIRE(named(podcast, "Host")->effectChain.size() == 3);

    const auto audiobook = templates::make(templates::Builtin::Audiobook);
    const auto& narration = named(audiobook, "Narration")->effectChain;
    REQUIRE(narration.back().kind == EffectKind::Limiter);
    REQUIRE(narration.back().limiter.ceilingDb <= -3.0f);

    const auto music  = templates::make(templates::Builtin::Music);
    const int  reverb = named(music, "Reverb")->id;
    REQUIRE(named(music, "Vocals")->sends.size() == 1);
    REQUIRE(named(music, "Vocals")->sends[0].busId == reverb);
    REQUIRE(named(music, "Drums")->sends.empty());

    const auto voiceOver = templates::make(templates::Builtin::VoiceOver);
    REQUIRE(named(voiceOver, "Music Bed")->effectChain[0].sidechainTrackId == named(voiceOver, "Voice")->id);
}

TEST_CASE("A project kept as a template loses its audio, not its setup", "[model][templates]")
{
    auto song = templates::make(templates::Builtin::Podcast);
    song.bpm  = 96.0;
    Clip clip;
    clip.audioFile   = "/audio/interview.wav";
    clip.lengthBeats = 16.0;
    addClip(song, named(song, "Host")->id, clip);
    song.markers.push_back(Marker {});
    song.projectRootFolder = "/projects/show";

    const auto kept = templates::asTemplate(song);
    for (const auto& track : kept.tracks)
        REQUIRE(track.clips.empty());
    REQUIRE(kept.markers.empty());
    REQUIRE(kept.projectRootFolder.empty());
    REQUIRE(kept.bpm == 96.0);
    REQUIRE(kept.tracks.size() == song.tracks.size());
    REQUIRE(named(kept, "Host")->effectChain == named(song, "Host")->effectChain);
}
