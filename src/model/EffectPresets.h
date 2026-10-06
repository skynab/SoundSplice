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
const std::vector<EffectPreset>& factoryPresets(EffectKind kind);

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
