#pragma once

namespace looper::engine
{
/** A small, fixed set of genres for the generative loop dialog — the same
    "enough to be useful without an open-ended taxonomy" scoping call
    already made for Scale (see Scale.h, docs/PLAN.md §25). */
enum class Genre
{
    House,
    Techno,
    HipHop,
    Trap,
    Ambient,
    LoFi,
    Synthwave,
    Cyberpunk
};

/** Display name for @p genre, as shown in the Generate Loop dialog. */
inline const char* genreName(Genre genre)
{
    switch (genre)
    {
        case Genre::House:  return "House";
        case Genre::Techno: return "Techno";
        case Genre::HipHop: return "Hip-Hop";
        case Genre::Trap:   return "Trap";
        case Genre::Ambient: return "Ambient";
        case Genre::LoFi:    return "Lo-Fi";
        case Genre::Synthwave: return "Synthwave";
        case Genre::Cyberpunk: return "Cyberpunk";
    }
    return "House";
}

/** The rhythm character a genre biases a generated loop towards:
    @p density feeds generateDrumLoop/generateMelodicLoop's own density
    parameter (see GenerativeLoop.h), @p swing feeds their swing parameter
    (0..0.9, the same range NoteOps::quantizeNotes clamps to). Hand-picked
    rather than derived, the same way the default drum loop's beat placement
    is hand-picked in DefaultContent.h — there's no formula for "how much
    does hip-hop swing," only a musically reasonable answer. */
struct GenreRhythmProfile
{
    double density;
    double swing;
};

inline GenreRhythmProfile rhythmProfileForGenre(Genre genre)
{
    switch (genre)
    {
        case Genre::House:   return { 0.55, 0.10 }; // four-on-the-floor, light shuffle
        case Genre::Techno:  return { 0.75, 0.00 }; // busy and dead straight
        case Genre::HipHop:  return { 0.40, 0.55 }; // sparse, heavy boom-bap swing
        case Genre::Trap:    return { 0.55, 0.20 }; // moderately busy, a bit of bounce
        case Genre::Ambient: return { 0.20, 0.00 }; // sparse, no groove to speak of
        case Genre::LoFi:    return { 0.35, 0.35 }; // loose, human, unhurried
        case Genre::Synthwave: return { 0.65, 0.00 }; // driving, tightly gridded sequences
        // Midtempo: sparse and dead straight. The halftime feel this genre
        // is named for comes from tempo, which a generated pattern doesn't
        // set - so the pattern earns it by leaving space instead.
        case Genre::Cyberpunk: return { 0.30, 0.00 };
    }
    return { 0.5, 0.0 };
}

} // namespace looper::engine
