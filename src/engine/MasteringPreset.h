#pragma once

namespace looper::engine
{
/** A small, fixed set of starting points for the mastering rack — the master
    bus's counterpart to GuitarTone, SynthTone and DrumKitStyle. */
enum class MasteringPreset
{
    Flat,        // every stage a no-op: the "undo all of this" button
    Transparent, // gentle catch-the-peaks, nothing else
    Loud,        // hard into the limiter, for streaming-style loudness
    Warm,        // low lift, top softened, mild glue
    Wide,        // stereo spread with a mono-safety-conscious width
    Voice        // spoken word: high-pass-ish low cut, presence, tight ceiling
};

constexpr int kNumMasteringPresets = 6;

/** Display name for @p preset, as shown on the mastering pane's buttons. */
inline const char* masteringPresetName(MasteringPreset preset)
{
    switch (preset)
    {
        case MasteringPreset::Flat:        return "Flat";
        case MasteringPreset::Transparent: return "Transparent";
        case MasteringPreset::Loud:        return "Loud";
        case MasteringPreset::Warm:        return "Warm";
        case MasteringPreset::Wide:        return "Wide";
        case MasteringPreset::Voice:       return "Voice";
    }
    return "Flat";
}

} // namespace looper::engine
