#pragma once

#include <string>
#include <vector>

#include "model/Serialization.h"

namespace soundsplice::model
{
/**
    Favorites (Audition's): an effect chain kept under a name, applied to the
    selection in one click from the Favorites menu.

    Kept in the app's settings, not a project: a sound someone has dialled
    in is reached for across projects. Written in the project format - a
    project whose tracks are the favorites, each one's name and effect chain -
    so every effect's settings, plugins' states included, round-trip exactly
    as they do in a project, with nothing new to keep in step.
*/
struct Favorite
{
    std::string             name;
    std::vector<EffectSlot> chain;

    bool operator==(const Favorite&) const = default;
};

inline std::string serializeFavorites(const std::vector<Favorite>& favorites)
{
    Song song;
    for (const auto& favorite : favorites)
        addTrack(song, TrackType::Audio, favorite.name).effectChain = favorite.chain;
    return serialize(song);
}

/** The favorites @p text holds; none for text that isn't any. */
inline std::vector<Favorite> deserializeFavorites(const std::string& text)
{
    std::vector<Favorite> favorites;
    Song                  song;
    if (text.empty() || ! deserialize(text, song))
        return favorites;
    for (const auto& track : song.tracks)
        favorites.push_back({ track.name, track.effectChain });
    return favorites;
}

/** @p favorites with @p favorite added, replacing one of the same name. */
inline std::vector<Favorite> withFavorite(std::vector<Favorite> favorites, Favorite favorite)
{
    for (auto& existing : favorites)
        if (existing.name == favorite.name)
        {
            existing = std::move(favorite);
            return favorites;
        }
    favorites.push_back(std::move(favorite));
    return favorites;
}

} // namespace soundsplice::model
