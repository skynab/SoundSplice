#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/MacrosDialog.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    model::EffectSlot compressor(float threshold)
    {
        auto slot                 = model::makeEffectSlot(model::EffectKind::Compressor);
        slot.enabled              = true;
        slot.compressor.thresholdDb = threshold;
        return slot;
    }

    macros::Macro podcastMacro()
    {
        macros::Macro macro;
        macro.name = "Podcast clean-up";
        macro.steps.push_back({ "Find Zero Crossings", {} });
        macro.steps.push_back({ {}, { compressor(-20.0f), model::makeEffectSlot(model::EffectKind::Limiter) } });
        macro.steps.push_back({ "Fade In", {} });
        return macro;
    }
}

TEST_CASE("Macros keep their steps, effect settings and all, through saving", "[gui][macros]")
{
    JuceFixture fixture;

    auto effectsOnly = macros::Macro { "Loud", { { {}, { compressor(-30.0f) } } } };
    const std::vector<macros::Macro> saved { podcastMacro(), effectsOnly, macros::Macro { "Empty", {} } };

    const auto text = macros::serialize(saved);
    REQUIRE(macros::deserialize(text) == saved);

    REQUIRE(macros::deserialize("").empty());
    REQUIRE(macros::deserialize("<SOMETHING_ELSE/>").empty());
}

TEST_CASE("A macro step is a command that acts at once, or effects", "[gui][macros]")
{
    JuceFixture fixture;

    REQUIRE(macros::recordable(*commands::find(commands::fadeIn)));
    REQUIRE(macros::recordable(*commands::find(commands::splitAtPlayhead)));
    REQUIRE_FALSE(macros::recordable(*commands::find(commands::normalizePeak)));   // asks first
    REQUIRE_FALSE(macros::recordable(*commands::find(commands::applyEffects)));    // recorded as its effects
    REQUIRE_FALSE(macros::recordable(*commands::find(commands::recordMacro)));
    REQUIRE_FALSE(macros::recordable(*commands::find(commands::newProject)));

    const auto macro = podcastMacro();
    REQUIRE(macros::commandFor(macro.steps[0]) == commands::find(commands::findZeroCrossings));
    REQUIRE(macros::commandFor(macro.steps[1]) == nullptr);
    REQUIRE(macros::commandFor({ "No Such Command", {} }) == nullptr);

    REQUIRE(macros::describe(macro.steps[0]) == "Find Zero Crossings");
    REQUIRE(macros::describe(macro.steps[1]) == "Effects: Compressor, Limiter");
}

TEST_CASE("Only a macro of effects can run over files", "[gui][macros]")
{
    JuceFixture fixture;

    REQUIRE_FALSE(macros::effectsOnly(podcastMacro()).has_value());

    macros::Macro chain { "Chain", { { {}, { compressor(-10.0f) } }, { {}, { compressor(-20.0f), compressor(-30.0f) } } } };
    const auto effects = macros::effectsOnly(chain);
    REQUIRE(effects.has_value());
    REQUIRE(effects->size() == 3);
    REQUIRE((*effects)[2].compressor.thresholdDb == -30.0f);

    auto list = macros::with({}, chain);
    chain.steps.pop_back();
    list = macros::with(list, chain); // same name: replaced
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].steps.size() == 1);
}

TEST_CASE("The Macros window adds, orders and removes steps", "[gui][macros]")
{
    JuceFixture fixture;

    MacrosDialog dialog;
    std::vector<macros::Macro> latest;
    int changes = 0;
    dialog.onChanged = [&](const std::vector<macros::Macro>& m) { latest = m; ++changes; };
    int ranMacro = -1;
    dialog.onRun = [&](int index) { ranMacro = index; };

    dialog.setMacros({ podcastMacro() });
    dialog.selectMacro(0);

    // A new effects step goes in at the end when no step is selected.
    dialog.setEffects(0, -1, { compressor(-12.0f) });
    REQUIRE(changes == 1);
    REQUIRE(latest[0].steps.size() == 4);
    REQUIRE(latest[0].steps[3].effects[0].compressor.thresholdDb == -12.0f);

    // Editing it in place.
    dialog.setEffects(0, 3, { compressor(-6.0f) });
    REQUIRE(latest[0].steps.size() == 4);
    REQUIRE(latest[0].steps[3].effects[0].compressor.thresholdDb == -6.0f);

    // Out of range does nothing.
    const int before = changes;
    dialog.setEffects(5, -1, { compressor(-6.0f) });
    REQUIRE(changes == before);
    REQUIRE(dialog.macrosForTesting() == latest);
}
