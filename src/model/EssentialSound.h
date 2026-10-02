#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "model/Clip.h"
#include "model/EffectParams.h"

namespace soundsplice::model
{
/**
    Essential Sound (Audition's): tag a clip as Dialogue, Music, SFX or
    Ambience and get a few tasks that make sense for it - reduce rumble,
    reduce noise, tame sibilance, add clarity, even out dynamics, widen - each
    one amount from 0 to 10.

    The amounts drive real effects: the slots they make go at the front of
    the clip's own chain (Clip::effects), marked EffectSlot::essential so
    they can be rebuilt whenever an amount moves while effects added by hand
    are never touched. Everything stays visible and editable in Clip FX.
    Loudness and ducking, which act across clips, are the app's (see
    MainComponent_Finishing.cpp).
*/
namespace essential
{
    /** Each task's id, its name, and the role(s) it's offered for. */
    struct Task
    {
        const char* id;
        const char* name;
        const char* tooltip;
    };

    inline constexpr Task kRumble   { "rumble", "Reduce Rumble", "Cuts low frequencies: handling noise, traffic, air conditioning" };
    inline constexpr Task kNoise    { "noise", "Reduce Noise", "Turns quiet background down between phrases" };
    inline constexpr Task kDeEss    { "deEss", "Reduce Sibilance", "Tames harsh S and T sounds" };
    inline constexpr Task kClarity  { "clarity", "Clarity", "Less mud, more presence" };
    inline constexpr Task kDynamics { "dynamics", "Even Out Dynamics", "Compression: quiet and loud parts closer together" };
    inline constexpr Task kWidth    { "width", "Stereo Width", "Wider, for a more enveloping bed" };

    /** The tasks offered for @p role, in the order their effects run. */
    inline std::vector<Task> tasksFor(SoundRole role)
    {
        switch (role)
        {
            case SoundRole::Dialogue: return { kRumble, kNoise, kDeEss, kClarity, kDynamics };
            case SoundRole::Music:    return { kClarity, kDynamics, kWidth };
            case SoundRole::Sfx:      return { kRumble, kClarity, kDynamics };
            case SoundRole::Ambience: return { kRumble, kWidth };
            case SoundRole::None:     break;
        }
        return {};
    }

    inline const char* roleName(SoundRole role)
    {
        switch (role)
        {
            case SoundRole::Dialogue: return "Dialogue";
            case SoundRole::Music:    return "Music";
            case SoundRole::Sfx:      return "SFX";
            case SoundRole::Ambience: return "Ambience";
            case SoundRole::None:     break;
        }
        return "None";
    }

    namespace detail
    {
        inline void set(EffectSlot& slot, std::string_view id, double value)
        {
            if (const auto* descriptor = descriptorFor(slot.kind))
                for (const auto& param : descriptor->params)
                    if (id == param.id)
                        setParamValue(slot, param, value);
        }

        /** The effect task @p id makes at @p amount (0-10). */
        inline EffectSlot slotFor(std::string_view id, double amount)
        {
            const double a = std::clamp(amount, 0.0, 10.0);
            if (id == "rumble")
            {
                auto slot = makeEffectSlot(EffectKind::Filter);
                set(slot, "mode", 1); // high-pass
                set(slot, "cutoff", 40.0 + 12.0 * a);
                set(slot, "resonance", 0.7);
                return slot;
            }
            if (id == "noise")
            {
                auto slot = makeEffectSlot(EffectKind::Expander);
                set(slot, "threshold", -70.0 + 3.0 * a);
                set(slot, "ratio", 1.5 + 0.35 * a);
                set(slot, "range", 6.0 + 2.4 * a);
                set(slot, "attack", 2.0);
                set(slot, "release", 120.0);
                return slot;
            }
            if (id == "deEss")
            {
                auto slot = makeEffectSlot(EffectKind::DeEsser);
                set(slot, "frequency", 5500.0);
                set(slot, "threshold", -18.0 - 2.0 * a);
                set(slot, "reduction", 1.2 * a);
                return slot;
            }
            if (id == "clarity")
            {
                auto slot = makeEffectSlot(EffectKind::ParametricEq);
                set(slot, "band1Type", 1); // bell: less mud
                set(slot, "band1Hz", 300.0);
                set(slot, "band1Gain", -0.4 * a);
                set(slot, "band1Q", 1.0);
                set(slot, "band2Type", 1); // bell: more presence
                set(slot, "band2Hz", 3500.0);
                set(slot, "band2Gain", 0.6 * a);
                set(slot, "band2Q", 0.8);
                return slot;
            }
            if (id == "dynamics")
            {
                auto slot = makeEffectSlot(EffectKind::Compressor);
                set(slot, "threshold", -8.0 - 2.2 * a);
                set(slot, "ratio", 1.5 + 0.35 * a);
                set(slot, "attack", 10.0);
                set(slot, "release", 150.0);
                set(slot, "makeUp", 0.5 * a);
                return slot;
            }
            // width
            auto slot = makeEffectSlot(EffectKind::StereoTool);
            set(slot, "width", 1.0 + 0.08 * a);
            return slot;
        }
    }

    /** The effects @p settings make, marked as Essential Sound's: one per
        task with an amount over 0, in tasksFor's order. */
    inline std::vector<EffectSlot> slotsFor(const EssentialSettings& settings)
    {
        std::vector<EffectSlot> slots;
        for (const auto& task : tasksFor(settings.role))
        {
            const auto it = settings.amounts.find(task.id);
            if (it == settings.amounts.end() || it->second <= 0.0f)
                continue;
            auto slot      = detail::slotFor(task.id, it->second);
            slot.enabled   = true;
            slot.essential = true;
            slots.push_back(std::move(slot));
        }
        return slots;
    }

    /** Rebuilds @p clip's Essential Sound effects from its settings, in front
        of the effects added by hand. */
    inline void apply(Clip& clip)
    {
        std::vector<EffectSlot> chain = slotsFor(clip.essential);
        for (const auto& slot : clip.effects)
            if (! slot.essential)
                chain.push_back(slot);
        clip.effects = std::move(chain);
    }

    /** Tags @p clip with @p role. A new role starts with no amounts: the
        old role's tasks may not be offered any more. */
    inline void setRole(Clip& clip, SoundRole role)
    {
        if (clip.essential.role == role)
            return;
        clip.essential.role = role;
        clip.essential.amounts.clear();
        apply(clip);
    }

    inline void setAmount(Clip& clip, const std::string& task, float amount)
    {
        clip.essential.amounts[task] = std::clamp(amount, 0.0f, 10.0f);
        apply(clip);
    }
}

} // namespace soundsplice::model
