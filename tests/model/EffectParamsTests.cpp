#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>
#include <string>

#include <model/EffectParams.h>
#include <model/Serialization.h>

using namespace soundsplice::model;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr EffectKind kBuiltInKinds[] {
        EffectKind::Filter, EffectKind::Delay,   EffectKind::Reverb, EffectKind::Drive,
        EffectKind::Compressor, EffectKind::Tremolo, EffectKind::Chorus, EffectKind::Wobble,
        EffectKind::Gate, EffectKind::Eq, EffectKind::Amplify, EffectKind::Invert,
        EffectKind::DcOffset, EffectKind::Limiter, EffectKind::Phaser, EffectKind::Flanger,
        EffectKind::BassTreble, EffectKind::StereoTool, EffectKind::GraphicEq, EffectKind::DeEsser,
        EffectKind::Expander, EffectKind::RingMod, EffectKind::Wah, EffectKind::Echo,
        EffectKind::Multiband, EffectKind::ParametricEq, EffectKind::Dynamics, EffectKind::GraphicEq31,
        EffectKind::Convolution, EffectKind::Vocoder
    };

    /** Every parameter of every built-in, read from @p slot. */
    std::vector<double> allValues(const EffectSlot& slot)
    {
        std::vector<double> values;
        for (const auto& effect : builtInEffects())
            for (const auto& param : effect.params)
                values.push_back(paramValue(slot, param));
        return values;
    }

    /** A value for @p param that differs from @p current and is on its step. */
    double differentValue(const EffectParam& param, double current)
    {
        return std::abs(current - param.max) > 1.0e-9 ? param.max : param.min;
    }
}

TEST_CASE("Every built-in effect has exactly one descriptor", "[model][effectparams]")
{
    std::set<int>         kinds;
    std::set<std::string> ids;

    for (const auto& effect : builtInEffects())
    {
        REQUIRE(kinds.insert((int) effect.kind).second);
        REQUIRE(ids.insert(effect.id).second);
        REQUIRE_FALSE(std::string(effect.name).empty());
        REQUIRE_FALSE(effect.params.empty());
    }

    for (const auto kind : kBuiltInKinds)
    {
        const auto* descriptor = descriptorFor(kind);
        REQUIRE(descriptor != nullptr);
        REQUIRE(descriptor->kind == kind);
    }

    REQUIRE(kinds.size() == std::size(kBuiltInKinds));

    // A plugin's parameters are its own business.
    REQUIRE(descriptorFor(EffectKind::Plugin) == nullptr);
}

TEST_CASE("Every parameter's range is sane and holds its default", "[model][effectparams]")
{
    for (const auto& effect : builtInEffects())
    {
        const auto            slot = makeEffectSlot(effect.kind);
        std::set<std::string> ids;

        for (const auto& param : effect.params)
        {
            INFO(effect.name << " / " << param.name);

            REQUIRE(ids.insert(param.id).second);
            REQUIRE(param.min < param.max);
            REQUIRE(param.displayScale > 0.0); // the panel divides by it
            REQUIRE(param.access.get != nullptr);
            REQUIRE(param.access.set != nullptr);

            // A default the control can't show would snap the moment the
            // panel was opened, changing the sound without anyone touching it.
            const double value = paramValue(slot, param);
            REQUIRE(value >= param.min);
            REQUIRE(value <= param.max);

            if (param.control == ParamControl::Choice)
                REQUIRE((int) param.choices.size() == (int) std::llround(param.max - param.min) + 1);
            else
                REQUIRE(param.choices.empty());

            if (param.control == ParamControl::Toggle)
            {
                REQUIRE(param.min == 0.0);
                REQUIRE(param.max == 1.0);
            }
        }
    }
}

TEST_CASE("Setting a parameter changes that parameter and nothing else", "[model][effectparams]")
{
    // The failure this guards against is an entry that points at the wrong
    // field: its control would move some other setting, or several.
    for (const auto& effect : builtInEffects())
    {
        for (const auto& param : effect.params)
        {
            INFO(effect.name << " / " << param.name);

            auto       slot   = makeEffectSlot(effect.kind);
            const auto before = allValues(slot);
            const auto target = differentValue(param, paramValue(slot, param));

            setParamValue(slot, param, target);
            REQUIRE_THAT(paramValue(slot, param), WithinAbs(target, 1.0e-6));

            const auto after = allValues(slot);
            int        changed = 0;
            for (size_t i = 0; i < before.size(); ++i)
                if (std::abs(before[i] - after[i]) > 1.0e-9)
                    ++changed;

            REQUIRE(changed == 1);
        }
    }
}

TEST_CASE("Every parameter survives a save and reload", "[model][effectparams]")
{
    // A control for a field the project file doesn't store would work until
    // the project was reopened.
    Song  song;
    auto& track = addTrack(song, TrackType::Audio, "Effects");

    for (const auto& effect : builtInEffects())
    {
        auto slot = makeEffectSlot(effect.kind);
        for (const auto& param : effect.params)
            setParamValue(slot, param, differentValue(param, paramValue(slot, param)));
        track.effectChain.push_back(slot);
    }

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(song), restored, &error));
    REQUIRE(restored.tracks.size() == 1);

    const auto& chain = restored.tracks[0].effectChain;
    REQUIRE(chain.size() == builtInEffects().size());
    REQUIRE(chain == song.tracks[0].effectChain);

    for (size_t i = 0; i < chain.size(); ++i)
    {
        const auto& effect = builtInEffects()[i];
        for (const auto& param : effect.params)
        {
            INFO(effect.name << " / " << param.name);
            REQUIRE_THAT(paramValue(chain[i], param),
                         WithinAbs(paramValue(song.tracks[0].effectChain[i], param), 1.0e-6));
        }
    }
}

TEST_CASE("Values are clamped to a parameter's range and step", "[model][effectparams]")
{
    EffectParam slider;
    slider.min  = 0.0;
    slider.max  = 1.0;
    slider.step = 0.25;

    REQUIRE(clampToParam(slider, -3.0) == 0.0);
    REQUIRE(clampToParam(slider, 7.0) == 1.0);
    REQUIRE(clampToParam(slider, 0.3) == 0.25);
    REQUIRE(clampToParam(slider, 0.4) == 0.5);

    EffectParam toggle;
    toggle.control = ParamControl::Toggle;
    toggle.min     = 0.0;
    toggle.max     = 1.0;

    REQUIRE(clampToParam(toggle, 0.2) == 0.0);
    REQUIRE(clampToParam(toggle, 0.7) == 1.0);
}

TEST_CASE("A new slot is enabled, with its own kind's flag set", "[model][effectparams]")
{
    const auto slot = makeEffectSlot(EffectKind::Compressor);

    REQUIRE(slot.kind == EffectKind::Compressor);
    REQUIRE(slot.enabled);
    REQUIRE(slot.compressor.enabled);
    REQUIRE_FALSE(slot.filter.enabled);
    REQUIRE_FALSE(slot.drive.enabled);
}
