#pragma once

namespace looper::engine
{
/** A small, fixed set of one-click starting tones for a Guitar track's
    fretboard pane — the same "enough to be useful without an open-ended
    taxonomy" scoping call already made for Genre (see Genre.h) and Scale. */
enum class GuitarTone
{
    ModernMetal,
    CleanJazz,
    ClassicRockCrunch,
    AmbientShoegaze,
    FunkPercussive,
    IndustrialCyber
};

constexpr int kNumGuitarTones = 6;

/** Display name for @p tone, as shown on the fretboard pane's tone buttons. */
inline const char* guitarToneName(GuitarTone tone)
{
    switch (tone)
    {
        case GuitarTone::ModernMetal:        return "Modern Metal";
        case GuitarTone::CleanJazz:          return "Clean / Jazz";
        case GuitarTone::ClassicRockCrunch:  return "Classic Rock";
        case GuitarTone::AmbientShoegaze:    return "Ambient";
        case GuitarTone::FunkPercussive:     return "Funk";
        case GuitarTone::IndustrialCyber:    return "Industrial";
    }
    return "Modern Metal";
}

} // namespace looper::engine
