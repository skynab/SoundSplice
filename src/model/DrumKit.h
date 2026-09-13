#pragma once

#include <string>
#include <vector>

namespace looper::model
{
/** One pad of a drum kit: a MIDI note number that triggers it, a display
    label, and the sample assigned to it (empty = silent, no sample loaded
    yet — the same safe default this project already uses for an audio clip
    with no file assigned), plus that pad's own mix settings.

    The mix fields default to "no change at all" (unity gain, centred, no
    transposition), so a kit that has never been touched sounds exactly as it
    did before they existed. muted/solo work per pad the same way they do per
    track — solo overrides, mute always wins — but are resolved into a single
    effective-mute flag before reaching the engine (see
    MainComponent::syncEngineTracks), since the pad map is rebuilt on every
    change anyway. */
struct DrumPad
{
    int         noteNumber = 36;
    std::string label;
    std::string samplePath;

    float gainDb         = 0.0f;
    float pan            = 0.0f; // -1 = hard left, 0 = centre, +1 = hard right
    float pitchSemitones = 0.0f; // resampling transposition, ± an octave or so
    bool  muted          = false;
    bool  solo           = false;

    bool operator==(const DrumPad&) const = default;
};

/** A track's drum kit: a small, fixed-ish list of independently replaceable
    one-shot pads (see engine::DrumKitNode), as opposed to the one melodic
    synth timbre an Instrument track's notes all share. */
struct DrumKit
{
    std::vector<DrumPad> pads;

    bool operator==(const DrumKit&) const = default;
};

/** The starting kit a new Drum track gets: Kick/Snare/Hat/Other on the usual
    GM-ish note numbers, no samples assigned yet. Not a full GM drum map —
    matches "one or more bass, snare, and other instrument types"; more pads
    can be added later without changing this shape. */
inline DrumKit makeDefaultDrumKit()
{
    DrumKit kit;
    kit.pads.push_back({ 36, "Kick", "" });
    kit.pads.push_back({ 38, "Snare", "" });
    kit.pads.push_back({ 42, "Hat", "" });
    kit.pads.push_back({ 45, "Other", "" });
    return kit;
}

} // namespace looper::model
