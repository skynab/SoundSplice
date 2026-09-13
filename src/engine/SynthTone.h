#pragma once

namespace looper::engine
{
/** A small, fixed set of one-click starting sounds for an Instrument track's
    synth editor — the same "enough to be useful without an open-ended
    taxonomy" scoping call already made for Genre (see Genre.h) and
    GuitarTone. Distinct from the file-based SynthPreset system: these are
    built in memory and always present, where a saved preset is a file the
    user made and can delete. */
enum class SynthTone
{
    CyberBass,
    CyberLead,
    DarkPad
};

constexpr int kNumSynthTones = 3;

/** Display name for @p tone, as shown on the synth editor's tone buttons. */
inline const char* synthToneName(SynthTone tone)
{
    switch (tone)
    {
        case SynthTone::CyberBass: return "Cyber Bass";
        case SynthTone::CyberLead: return "Cyber Lead";
        case SynthTone::DarkPad:   return "Dark Pad";
    }
    return "Cyber Bass";
}

} // namespace looper::engine
