#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>
#include <string>

#include <model/EffectPresets.h>

using namespace soundsplice::model;
using Catch::Matchers::WithinAbs;

namespace
{
    const EffectParam* paramById(const EffectDescriptor& descriptor, const std::string& id)
    {
        for (const auto& param : descriptor.params)
            if (param.id == id)
                return &param;
        return nullptr;
    }

    double differentValue(const EffectParam& param, double current)
    {
        return std::abs(current - param.max) > 1.0e-9 ? param.max : param.min;
    }
}

TEST_CASE("Factory presets only name real parameters, with values in range", "[model][presets]")
{
    for (const auto& effect : builtInEffects())
    {
        const auto& presets = factoryPresets(effect.kind);
        INFO(effect.name);
        REQUIRE_FALSE(presets.empty());

        std::set<std::string> names;
        for (const auto& preset : presets)
        {
            INFO(preset.name);
            REQUIRE(names.insert(preset.name).second);

            for (const auto& [id, value] : preset.values)
            {
                INFO(id);
                const auto* param = paramById(effect, id);
                REQUIRE(param != nullptr);

                // Already on the control's range and step, so applying the
                // preset gives exactly the numbers written here.
                REQUIRE_THAT(clampToParam(*param, value), WithinAbs(value, 1.0e-9));
            }
        }
    }

    REQUIRE(factoryPresets(EffectKind::Plugin).empty());
}

TEST_CASE("Applying a preset sets what it names and resets what it leaves out", "[model][presets]")
{
    const auto* compressor = descriptorFor(EffectKind::Compressor);
    REQUIRE(compressor != nullptr);

    auto slot = makeEffectSlot(EffectKind::Compressor);
    for (const auto& param : compressor->params)
        setParamValue(slot, param, param.max);

    const EffectPreset preset { "Just Ratio", { { "ratio", 3.0 }, { "noSuchParameter", 99.0 } } };
    REQUIRE(applyPreset(slot, preset));

    const auto defaults = makeEffectSlot(EffectKind::Compressor);
    REQUIRE(slot.compressor.ratio == 3.0f);
    REQUIRE(slot.compressor.thresholdDb == defaults.compressor.thresholdDb);
    REQUIRE(slot.compressor.attackMs == defaults.compressor.attackMs);
    REQUIRE(slot.compressor.makeUpDb == defaults.compressor.makeUpDb);

    // A plugin has no descriptor, so there is nothing a preset could mean.
    auto plugin = makeEffectSlot(EffectKind::Plugin);
    const auto before = plugin;
    REQUIRE_FALSE(applyPreset(plugin, preset));
    REQUIRE(plugin == before);
}

TEST_CASE("A captured preset brings back exactly the settings it came from", "[model][presets]")
{
    for (const auto& effect : builtInEffects())
    {
        INFO(effect.name);

        auto dialled = makeEffectSlot(effect.kind);
        for (const auto& param : effect.params)
            setParamValue(dialled, param, differentValue(param, paramValue(dialled, param)));

        const auto preset = capturePreset(dialled, effect, "Mine");
        REQUIRE(preset.values.size() == effect.params.size());

        auto fresh = makeEffectSlot(effect.kind);
        REQUIRE(applyPreset(fresh, preset));
        REQUIRE(fresh == dialled);
    }
}

TEST_CASE("User presets round-trip through their text form", "[model][presets]")
{
    const std::vector<UserEffectPreset> presets {
        { "compressor", { "My Vocal Chain", { { "ratio", 3.5 }, { "threshold", -21.25 } } } },
        { "reverb",     { "Room", { { "mix", 0.1234567890123 } } } },
        { "gate",       { "Empty", {} } },
    };

    REQUIRE(deserializeUserPresets(serializeUserPresets(presets)) == presets);

    // A line break in a name can't be stored on one line, so it becomes a space.
    const std::vector<UserEffectPreset> awkward { { "delay", { "Two\nLines", { { "time", 250 } } } } };
    const auto restored = deserializeUserPresets(serializeUserPresets(awkward));
    REQUIRE(restored.size() == 1);
    REQUIRE(restored[0].preset.name == "Two Lines");
}

TEST_CASE("Damaged preset text still loads the presets it can", "[model][presets]")
{
    const std::string text = "VALUE ratio 3\n"          // before any preset: ignored
                             "PRESET compressor Good\r\n"
                             "VALUE ratio 2\n"
                             "nonsense line\n"
                             "VALUE attack notANumber\n"
                             "PRESET \n"               // no effect or name: skipped...
                             "VALUE ratio 9\n"         // ...along with its values
                             "PRESET gate Also Fine\n";

    const auto presets = deserializeUserPresets(text);
    REQUIRE(presets.size() == 2);

    REQUIRE(presets[0].effectId == "compressor");
    REQUIRE(presets[0].preset.name == "Good");
    REQUIRE(presets[0].preset.values == std::vector<std::pair<std::string, double>> { { "ratio", 2.0 } });

    REQUIRE(presets[1].effectId == "gate");
    REQUIRE(presets[1].preset.name == "Also Fine");
    REQUIRE(presets[1].preset.values.empty());
}

TEST_CASE("Saving under an existing name replaces that preset, and only that one", "[model][presets]")
{
    std::vector<UserEffectPreset> presets;
    presets = withUserPreset(presets, "delay", { "Mine", { { "time", 100 } } });
    presets = withUserPreset(presets, "reverb", { "Mine", { { "mix", 0.5 } } });
    presets = withUserPreset(presets, "delay", { "Mine", { { "time", 400 } } });

    REQUIRE(presets.size() == 2);
    REQUIRE(presets[0].effectId == "delay");
    REQUIRE(presets[0].preset.values[0].second == 400.0);

    presets = withoutUserPreset(presets, "delay", "Mine");
    REQUIRE(presets.size() == 1);
    REQUIRE(presets[0].effectId == "reverb");
}
