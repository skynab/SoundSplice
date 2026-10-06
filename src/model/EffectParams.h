#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>
#include <type_traits>
#include <vector>

#include "engine/EffectParamValues.h"
#include "model/Effects.h"

namespace soundsplice::model
{
/**
    What each built-in effect's parameters are: name, range, units, how they
    are edited, and where in an EffectSlot each one lives.

    Before this, an effect's parameters were spelled out by hand in the chain
    panel (a member, a range, a show/hide case, a read and a write for every
    one of them), and each new effect meant repeating all of that. A parameter
    missed in one of those places is a control that silently does nothing. The
    table here is the one definition the panel builds its controls from, and
    tests/model/EffectParamsTests.cpp checks every entry against the document
    it points into, so it can't point at the wrong field.

    The ids are also how everything else reaches a parameter: the engine's
    nodes read their settings by them (effectParamValues), and the project
    file saves each one under them (Serialization.h). So adding an effect is
    its settings struct and slot member (Effects.h), an entry here, and its
    node in engine/EffectChain.h; nothing else lists effects.

    JUCE-free so those checks run headless. The values themselves live in the
    settings structs, which the table reads and writes.
*/

/** How a parameter is edited. */
enum class ParamControl
{
    Slider,
    Toggle, // stored as 0 or 1
    Choice  // stored as min + the index of the chosen entry
};

/** Reads and writes one field of an EffectSlot, as a double. */
struct ParamAccess
{
    double (*get)(const EffectSlot&)   = nullptr;
    void   (*set)(EffectSlot&, double) = nullptr;
};

namespace paramdetail
{
    template <auto Group, auto Field>
    double getField(const EffectSlot& slot)
    {
        return (double) ((slot.*Group).*Field);
    }

    template <auto Group, auto Field>
    void setField(EffectSlot& slot, double value)
    {
        auto& field = (slot.*Group).*Field;
        using T = std::remove_cvref_t<decltype(field)>;

        if (std::is_same_v<T, bool>)
            field = value >= 0.5;
        else if (std::is_integral_v<T>)
            field = (T) std::llround(value);
        else
            field = (T) value;
    }
}

/** Access to `slot.*Group.*Field`, e.g.
    `fieldAccess<&EffectSlot::filter, &FilterSettings::cutoff>`. Built from
    member pointers rather than hand-written lambdas so an entry can't read
    one field and write another. */
template <auto Group, auto Field>
inline constexpr ParamAccess fieldAccess { &paramdetail::getField<Group, Field>,
                                           &paramdetail::setField<Group, Field> };

/** One parameter of a built-in effect. Ranges are in the units the document
    stores; displayScale converts for display, so a 0..1 mix can be shown as a
    percentage without the document changing. */
struct EffectParam
{
    const char*  id   = "";   // stable identifier, for presets
    const char*  name = "";   // shown beside the control
    ParamControl control = ParamControl::Slider;

    double min  = 0.0;
    double max  = 1.0;
    double step = 0.0;        // 0 for continuous

    double      displayScale = 1.0; // shown value = stored value * displayScale
    const char* unit         = "";  // shown after the value, e.g. " Hz"

    /** A displayed value to put at the slider's midpoint, for ranges that are
        read logarithmically (frequencies); 0 keeps the slider linear. */
    double skewMidpoint = 0.0;

    std::vector<const char*> choices; // ParamControl::Choice only, in stored order
    const char*              tooltip = "";

    ParamAccess access;
};

/** One built-in effect. */
struct EffectDescriptor
{
    EffectKind  kind  = EffectKind::Filter;
    const char* id    = "";
    const char* name  = "";
    const char* group = ""; // Add-menu submenu, or "" for the top level

    std::vector<EffectParam> params;
};

/** A parameter's value in @p slot. */
inline double paramValue(const EffectSlot& slot, const EffectParam& param)
{
    return param.access.get(slot);
}

/** @p value moved into @p param's range and onto its step. */
inline double clampToParam(const EffectParam& param, double value)
{
    double v = std::clamp(value, param.min, param.max);

    if (param.control == ParamControl::Toggle)
        return v >= 0.5 ? 1.0 : 0.0;

    if (param.step > 0.0)
        v = std::clamp(param.min + std::round((v - param.min) / param.step) * param.step, param.min, param.max);

    return v;
}

/** Sets a parameter in @p slot, clamped to its range and step. */
inline void setParamValue(EffectSlot& slot, const EffectParam& param, double value)
{
    param.access.set(slot, clampToParam(param, value));
}

/** Every built-in effect, in the order the Add menu offers them. */
const std::vector<EffectDescriptor>& builtInEffects();

/** The descriptor for @p kind, or nullptr for a hosted plugin, whose
    parameters belong to the plugin itself. */
inline const EffectDescriptor* descriptorFor(EffectKind kind)
{
    for (const auto& effect : builtInEffects())
        if (effect.kind == kind)
            return &effect;
    return nullptr;
}

/** The built-in with id @p id (as saved in a project), or nullptr. */
inline const EffectDescriptor* descriptorFor(std::string_view id)
{
    for (const auto& effect : builtInEffects())
        if (id == effect.id)
            return &effect;
    return nullptr;
}

/** @p effect's parameter with id @p id, or nullptr. */
inline const EffectParam* paramFor(const EffectDescriptor& effect, std::string_view id)
{
    for (const auto& param : effect.params)
        if (id == param.id)
            return &param;
    return nullptr;
}

/** A new, enabled slot of @p kind with its default settings. */
inline EffectSlot makeEffectSlot(EffectKind kind)
{
    EffectSlot slot;
    slot.kind    = kind;
    slot.enabled = true; // added because you want to hear it
    return slot;
}

/** @p slot's settings for its own kind, by parameter id: what the engine's
    node for it reads (see engine::EffectParamValues). Empty but for
    `enabled` for a plugin, whose parameters belong to the plugin.

    @p withoutAutomated leaves out the parameters that have a lane, for the
    live chain: there the lane sets them every block, and a static value
    pushed on every edit would fight it. */
inline engine::EffectParamValues effectParamValues(const EffectSlot& slot, bool withoutAutomated = false)
{
    engine::EffectParamValues values;
    values.kind    = slot.kind;
    values.enabled = slot.enabled;

    if (const auto* effect = descriptorFor(slot.kind))
        for (const auto& param : effect->params)
            if (! withoutAutomated || slot.lane(param.id) == nullptr)
                values.set(param.id, paramValue(slot, param));

    // The one setting that isn't a number, so it has no descriptor entry.
    if (slot.kind == EffectKind::Convolution)
        values.setText("irFile", slot.convolution.irFile);

    return values;
}

} // namespace soundsplice::model
