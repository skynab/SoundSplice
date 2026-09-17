#pragma once

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "model/EffectParams.h"

namespace soundsplice::model
{
/**
    Effect presets: named sets of parameter values for a built-in effect.

    Values are keyed by parameter id (see EffectParam::id), not by position.
    A preset saved today still applies after an effect gains a parameter
    (which then takes its default) or loses one (whose value is ignored), and
    a preset can never land a value on the wrong control because the order
    changed.

    Factory presets ship with the app. User presets live in the app's
    settings rather than in a project, because a sound someone has dialled in
    is something they reach for across projects; serializeUserPresets is the
    text form they are stored in.
*/
struct EffectPreset
{
    std::string                                 name;
    std::vector<std::pair<std::string, double>> values; // parameter id -> stored value

    bool operator==(const EffectPreset&) const = default;
};

/** A preset the user saved, and which effect it belongs to (EffectDescriptor::id). */
struct UserEffectPreset
{
    std::string  effectId;
    EffectPreset preset;

    bool operator==(const UserEffectPreset&) const = default;
};

/** @p slot's current settings as a preset called @p name. */
inline EffectPreset capturePreset(const EffectSlot& slot, const EffectDescriptor& descriptor, std::string name)
{
    EffectPreset preset;
    preset.name = std::move(name);

    for (const auto& param : descriptor.params)
        preset.values.emplace_back(param.id, paramValue(slot, param));

    return preset;
}

/** Applies @p preset to @p slot. Parameters the preset names are set
    (clamped to their range); parameters it leaves out go back to their
    defaults, so a preset always sounds the same whatever was dialled in
    before it; ids it names that the effect doesn't have are ignored.

    Returns false, changing nothing, for a slot with no descriptor (a plugin). */
inline bool applyPreset(EffectSlot& slot, const EffectPreset& preset)
{
    const auto* descriptor = descriptorFor(slot.kind);
    if (descriptor == nullptr)
        return false;

    const auto defaults = makeEffectSlot(slot.kind);

    for (const auto& param : descriptor->params)
    {
        const auto named = std::find_if(preset.values.begin(), preset.values.end(),
                                        [&param](const auto& value) { return value.first == param.id; });

        if (named != preset.values.end())
            setParamValue(slot, param, named->second);
        else
            param.access.set(slot, paramValue(defaults, param)); // exactly the default, not snapped
    }

    return true;
}

/** The presets that ship with @p kind. Starting points, each reachable by
    hand from the controls; empty for a plugin. */
inline const std::vector<EffectPreset>& factoryPresets(EffectKind kind)
{
    static const std::vector<std::pair<EffectKind, std::vector<EffectPreset>>> table {
        { EffectKind::Filter, {
            { "Rumble Cut",   { { "mode", 1 }, { "cutoff", 80 },   { "resonance", 0.71 } } },
            { "Warm Top",     { { "mode", 0 }, { "cutoff", 6000 }, { "resonance", 0.71 } } },
            { "Telephone",    { { "mode", 2 }, { "cutoff", 1500 }, { "resonance", 1.2 } } } } },

        { EffectKind::Delay, {
            { "Slapback",     { { "time", 110 }, { "feedback", 0.1 }, { "mix", 0.25 } } },
            { "Echo",         { { "time", 380 }, { "feedback", 0.4 }, { "mix", 0.3 } } },
            { "Long Ambient", { { "time", 750 }, { "feedback", 0.6 }, { "mix", 0.25 } } } } },

        { EffectKind::Reverb, {
            { "Small Room",   { { "room", 0.3 },  { "damping", 0.6 }, { "mix", 0.18 } } },
            { "Hall",         { { "room", 0.75 }, { "damping", 0.4 }, { "mix", 0.28 } } },
            { "Cathedral",    { { "room", 0.95 }, { "damping", 0.2 }, { "mix", 0.38 } } } } },

        { EffectKind::Drive, {
            { "Light Overdrive", { { "drive", 3 },  { "tone", 0.55 }, { "level", 0.8 }, { "cabinet", 1 } } },
            { "Crunch",          { { "drive", 8 },  { "tone", 0.5 },  { "level", 0.7 }, { "asymmetry", 0.15 },
                                   { "stages", 2 }, { "cabinet", 1 } } },
            { "Fuzz",            { { "drive", 25 }, { "tone", 0.45 }, { "level", 0.6 }, { "hardClip", 1 },
                                   { "cabinet", 1 }, { "oversample", 1 } } } } },

        { EffectKind::Compressor, {
            { "Gentle Glue",      { { "threshold", -18 }, { "ratio", 2 },  { "attack", 30 },  { "release", 200 },
                                    { "makeUp", 2 } } },
            { "Vocal",            { { "threshold", -22 }, { "ratio", 4 },  { "attack", 8 },   { "release", 120 },
                                    { "makeUp", 4 } } },
            { "Podcast Leveller", { { "threshold", -26 }, { "ratio", 6 },  { "attack", 5 },   { "release", 150 },
                                    { "makeUp", 6 } } },
            { "Peak Catcher",     { { "threshold", -8 },  { "ratio", 20 }, { "attack", 0.5 }, { "release", 60 },
                                    { "makeUp", 0 } } } } },

        { EffectKind::Tremolo, {
            { "Slow Pulse", { { "rate", 3 }, { "depth", 0.5 } } },
            { "Fast Chop",  { { "rate", 9 }, { "depth", 0.9 } } } } },

        { EffectKind::Chorus, {
            { "Subtle", { { "rate", 0.4 }, { "depth", 0.3 }, { "mix", 0.35 } } },
            { "Lush",   { { "rate", 0.8 }, { "depth", 0.7 }, { "mix", 0.5 } } } } },

        { EffectKind::Wobble, {
            { "Sixteenth Wub",   { { "rate", 0.25 }, { "depth", 0.8 }, { "cutoff", 150 }, { "resonance", 1.2 },
                                   { "mix", 1 } } },
            { "Half-Note Sweep", { { "rate", 2 },    { "depth", 0.7 }, { "cutoff", 250 }, { "resonance", 0.9 },
                                   { "mix", 1 } } } } },

        { EffectKind::Gate, {
            { "Noise Floor", { { "threshold", -50 }, { "range", 40 }, { "attack", 1 },   { "hold", 30 },
                               { "release", 200 } } },
            { "Tight Drums", { { "threshold", -30 }, { "range", 80 }, { "attack", 0.5 }, { "hold", 10 },
                               { "release", 60 } } } } },

        { EffectKind::Limiter, {
            { "Streaming Ceiling", { { "input", 0 }, { "ceiling", -1 },   { "release", 100 } } },
            { "Broadcast",         { { "input", 0 }, { "ceiling", -2 },   { "release", 150 } } },
            { "Loud",              { { "input", 6 }, { "ceiling", -0.3 }, { "release", 50 } } } } },

        { EffectKind::Phaser, {
            { "Slow Swirl", { { "rate", 0.2 }, { "depth", 0.8 }, { "feedback", 0.4 }, { "stages", 3 }, { "mix", 0.5 } } },
            { "Jet",        { { "rate", 1.5 }, { "depth", 0.9 }, { "feedback", 0.75 }, { "stages", 6 }, { "mix", 0.5 } } },
            { "Subtle",     { { "rate", 0.4 }, { "depth", 0.5 }, { "feedback", 0.1 }, { "stages", 2 }, { "mix", 0.35 } } } } },

        { EffectKind::Flanger, {
            { "Classic",  { { "rate", 0.25 }, { "depth", 0.7 }, { "delay", 1 },   { "feedback", 0.5 },  { "mix", 0.5 } } },
            { "Metallic", { { "rate", 0.1 },  { "depth", 0.5 }, { "delay", 0.5 }, { "feedback", 0.85 }, { "mix", 0.5 } } },
            { "Wide",     { { "rate", 0.5 },  { "depth", 1 },   { "delay", 2 },   { "feedback", -0.4 }, { "mix", 0.5 } } } } },

        { EffectKind::BassTreble, {
            { "Bass Boost",  { { "bass", 6 },  { "treble", 0 },  { "volume", -3 } } },
            { "Brighten",    { { "bass", 0 },  { "treble", 4 },  { "volume", -1 } } },
            { "Telephone",   { { "bass", -24 }, { "treble", -18 }, { "volume", 6 } } } } },

        { EffectKind::StereoTool, {
            { "Mono Check",   { { "width", 1 },   { "balance", 0 }, { "mono", 1 }, { "swap", 0 } } },
            { "Wider",        { { "width", 1.4 }, { "balance", 0 }, { "mono", 0 }, { "swap", 0 } } },
            { "Narrower",     { { "width", 0.6 }, { "balance", 0 }, { "mono", 0 }, { "swap", 0 } } } } },

        { EffectKind::Invert, {
            { "Both Channels", { { "left", 1 }, { "right", 1 } } },
            { "Left Only",     { { "left", 1 }, { "right", 0 } } },
            { "Right Only",    { { "left", 0 }, { "right", 1 } } } } },

        { EffectKind::DcOffset, {
            { "Gentle (2 Hz)",   { { "cutoff", 2 } } },
            { "Standard (5 Hz)", { { "cutoff", 5 } } },
            { "Firm (15 Hz)",    { { "cutoff", 15 } } } } },

        { EffectKind::Amplify, {
            { "+6 dB", { { "gain", 6 } } },
            { "-6 dB", { { "gain", -6 } } } } },

        { EffectKind::Eq, {
            { "Voice Clarity", { { "lowFreq", 100 }, { "low", -4 }, { "midFreq", 3000 }, { "mid", 3 },
                                 { "midQ", 1 }, { "highFreq", 8000 }, { "high", 2 } } },
            { "Mid Scoop",     { { "lowFreq", 120 }, { "low", 3 },  { "midFreq", 700 },  { "mid", -6 },
                                 { "midQ", 0.8 }, { "highFreq", 5000 }, { "high", 3 } } },
            { "Warmth",        { { "lowFreq", 150 }, { "low", 3 },  { "midFreq", 3000 }, { "mid", -2 },
                                 { "midQ", 1 }, { "highFreq", 9000 }, { "high", -3 } } } } },
    };

    for (const auto& [presetKind, presets] : table)
        if (presetKind == kind)
            return presets;

    static const std::vector<EffectPreset> none;
    return none;
}

/** @p presets with @p preset saved for @p effectId, replacing one of the same
    name for that effect if there is one. */
inline std::vector<UserEffectPreset> withUserPreset(std::vector<UserEffectPreset> presets,
                                                    const std::string& effectId, EffectPreset preset)
{
    for (auto& user : presets)
    {
        if (user.effectId == effectId && user.preset.name == preset.name)
        {
            user.preset = std::move(preset);
            return presets;
        }
    }

    presets.push_back({ effectId, std::move(preset) });
    return presets;
}

/** @p presets without @p effectId's preset called @p name. */
inline std::vector<UserEffectPreset> withoutUserPreset(std::vector<UserEffectPreset> presets,
                                                       const std::string& effectId, const std::string& name)
{
    presets.erase(std::remove_if(presets.begin(), presets.end(),
                                 [&](const UserEffectPreset& user)
                                 { return user.effectId == effectId && user.preset.name == name; }),
                  presets.end());
    return presets;
}

/** User presets as text: a PRESET line naming the effect and the preset,
    then a VALUE line per parameter. Line-based for the same reason the
    project format is, and so a hand-edited or truncated file still loads
    everything before the damage. */
inline std::string serializeUserPresets(const std::vector<UserEffectPreset>& presets)
{
    std::ostringstream out;

    for (const auto& user : presets)
    {
        // The name is the rest of its line, so it may hold spaces but not a
        // line break.
        auto name = user.preset.name;
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::replace(name.begin(), name.end(), '\r', ' ');

        out << "PRESET " << user.effectId << " " << name << "\n";

        for (const auto& [id, value] : user.preset.values)
        {
            char number[64];
            std::snprintf(number, sizeof(number), "%.17g", value);
            out << "VALUE " << id << " " << number << "\n";
        }
    }

    return out.str();
}

/** The inverse of serializeUserPresets. Lines it can't make sense of are
    skipped, as are the values of a PRESET line missing its effect or name:
    losing one damaged preset is better than losing the library. */
inline std::vector<UserEffectPreset> deserializeUserPresets(const std::string& text)
{
    std::vector<UserEffectPreset> presets;
    bool                          collecting = false; // whether VALUE lines belong to presets.back()

    std::istringstream in(text);
    std::string        line;

    while (std::getline(in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();

        std::istringstream fields(line);
        std::string        tag;
        fields >> tag;

        if (tag == "PRESET")
        {
            UserEffectPreset user;
            fields >> user.effectId;
            std::getline(fields, user.preset.name);

            const auto first = user.preset.name.find_first_not_of(' ');
            user.preset.name = first == std::string::npos ? std::string() : user.preset.name.substr(first);

            collecting = ! user.effectId.empty() && ! user.preset.name.empty();
            if (collecting)
                presets.push_back(std::move(user));
        }
        else if (tag == "VALUE" && collecting)
        {
            std::string id;
            double      value = 0.0;
            if (fields >> id >> value)
                presets.back().preset.values.emplace_back(id, value);
        }
    }

    return presets;
}

} // namespace soundsplice::model
