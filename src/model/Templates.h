#pragma once

#include <initializer_list>
#include <string_view>
#include <utility>

#include "model/EffectParams.h"
#include "model/Song.h"

namespace soundsplice::model
{
/**
    Project templates: a project to start from with its tracks named, routed
    and given effects for a kind of work - a podcast, an audiobook, music, a
    voice-over - and no audio yet.

    The built-in ones are made here; the user's own are projects saved with
    File > Save as Template, which keeps everything but the audio
    (templates::asTemplate).
*/
namespace templates
{
    enum class Builtin
    {
        Podcast,
        Audiobook,
        Music,
        VoiceOver,
    };

    inline constexpr Builtin kAll[] { Builtin::Podcast, Builtin::Audiobook, Builtin::Music, Builtin::VoiceOver };

    inline const char* name(Builtin which)
    {
        switch (which)
        {
            case Builtin::Podcast:   return "Podcast";
            case Builtin::Audiobook: return "Audiobook";
            case Builtin::Music:     return "Music";
            case Builtin::VoiceOver: return "Voice-Over";
        }
        return "";
    }

    inline const char* description(Builtin which)
    {
        switch (which)
        {
            case Builtin::Podcast:   return "Host and guest voices cleaned up and mixed through a Voices bus, and a music track for the theme.";
            case Builtin::Audiobook: return "One narration track, cleaned and levelled for ACX, and a room tone track to patch gaps.";
            case Builtin::Music:     return "Drums, bass, guitar, keys and vocals, and a reverb bus they send to.";
            case Builtin::VoiceOver: return "A voice track, and a music bed that ducks under it by itself.";
        }
        return "";
    }

    namespace detail
    {
        /** A built-in effect, on, with the parameters named set. */
        inline EffectSlot effect(EffectKind kind, std::initializer_list<std::pair<std::string_view, double>> values)
        {
            auto slot    = makeEffectSlot(kind);
            slot.enabled = true;
            if (const auto* descriptor = descriptorFor(kind))
                for (const auto& [id, value] : values)
                    for (const auto& param : descriptor->params)
                        if (id == param.id)
                            setParamValue(slot, param, value);
            return slot;
        }

        inline EffectSlot highPass(double hz)
        {
            return effect(EffectKind::Filter, { { "mode", 1 }, { "cutoff", hz }, { "resonance", 0.7 } });
        }

        /** What a spoken voice usually wants: rumble off, sibilance tamed,
            levels evened. */
        inline std::vector<EffectSlot> voiceChain()
        {
            return { highPass(80.0),
                     effect(EffectKind::DeEsser, { { "frequency", 6000 }, { "threshold", -24 }, { "reduction", 6 } }),
                     effect(EffectKind::Compressor, { { "threshold", -20 }, { "ratio", 3 }, { "attack", 8 },
                                                      { "release", 150 }, { "makeUp", 4 } }) };
        }

        inline EffectSlot limiter(double ceiling)
        {
            return effect(EffectKind::Limiter, { { "input", 0 }, { "ceiling", ceiling }, { "release", 80 } });
        }

        inline Track& audio(Song& song, const char* trackName, float gainDb = 0.0f)
        {
            auto& track  = addTrack(song, TrackType::Audio, trackName);
            track.gainDb = gainDb;
            return track;
        }
    }

    /** A new project for @p which. */
    inline Song make(Builtin which)
    {
        using namespace detail;
        Song song;
        switch (which)
        {
            case Builtin::Podcast:
            {
                const int voices = addTrack(song, TrackType::Bus, "Voices").id;
                song.tracks.back().effectChain = { limiter(-1.0) };
                for (const char* who : { "Host", "Guest" })
                {
                    auto& track       = audio(song, who);
                    track.effectChain = voiceChain();
                    track.outputBusId = voices;
                }
                audio(song, "Music", -12.0f);
                break;
            }

            case Builtin::Audiobook:
            {
                auto& narration       = audio(song, "Narration");
                narration.effectChain = voiceChain();
                // ACX: peaks no higher than -3 dB.
                narration.effectChain.push_back(limiter(-3.5));
                audio(song, "Room Tone");
                break;
            }

            case Builtin::Music:
            {
                const int reverb = addTrack(song, TrackType::Bus, "Reverb").id;
                song.tracks.back().effectChain = { effect(EffectKind::Reverb, { { "room", 0.7 }, { "damping", 0.4 }, { "mix", 1.0 } }) };
                for (const char* part : { "Drums", "Bass", "Guitar", "Keys", "Vocals" })
                {
                    auto& track = audio(song, part);
                    if (std::string_view(part) != "Drums" && std::string_view(part) != "Bass")
                        track.sends.push_back({ reverb, -12.0f, false });
                }
                song.tracks.back().effectChain = voiceChain(); // Vocals
                break;
            }

            case Builtin::VoiceOver:
            {
                auto& voice       = audio(song, "Voice");
                voice.effectChain = voiceChain();
                const int voiceId = voice.id;

                // Keyed by the voice: the bed dips whenever it speaks.
                auto& bed  = audio(song, "Music Bed", -8.0f);
                auto  duck = effect(EffectKind::Compressor, { { "threshold", -30 }, { "ratio", 6 }, { "attack", 20 },
                                                              { "release", 400 }, { "makeUp", 0 } });
                duck.sidechainTrackId = voiceId;
                bed.effectChain       = { duck };
                break;
            }
        }
        return song;
    }

    /** @p song as a template: its tracks, routing, effects and tempo, without
        its audio, markers or folder. */
    inline Song asTemplate(Song song)
    {
        for (auto& track : song.tracks)
            track.clips.clear();
        song.markers.clear();
        song.projectRootFolder.clear();
        return song;
    }
}

} // namespace soundsplice::model
