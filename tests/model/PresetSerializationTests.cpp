#include <catch2/catch_test_macros.hpp>

#include <model/PresetSerialization.h>

using namespace looper::model;

namespace
{
    SynthPreset makeSamplePreset()
    {
        SynthPreset preset;
        preset.name = "Warm Pad + Tube Drive";

        preset.synth.waveform        = 1;
        preset.synth.attackMs        = 250.0f;
        preset.synth.decayMs         = 400.0f;
        preset.synth.sustain         = 0.6f;
        preset.synth.releaseMs       = 800.0f;
        preset.synth.filterEnabled   = true;
        preset.synth.filterMode      = 1;
        preset.synth.filterCutoff    = 2500.0f;
        preset.synth.filterResonance = 1.2f;
        preset.synth.gainDb          = -3.0f;

        EffectSlot drive;
        drive.kind           = EffectKind::Drive;
        drive.enabled        = true;
        drive.drive.enabled  = true;
        drive.drive.drive    = 12.5f;
        drive.drive.tone     = 0.65f;
        drive.drive.level    = 0.55f;
        drive.drive.hardClip = true;
        drive.drive.cabinet  = false;
        drive.drive.asymmetry  = 0.35f;
        drive.drive.oversample = true;

        EffectSlot plugin;
        plugin.kind             = EffectKind::Plugin;
        plugin.enabled          = true;
        plugin.plugin.format    = PluginFormat::VST3;
        plugin.plugin.identifier = "com.example.DistortoMax";
        plugin.plugin.name      = "DistortoMax";
        plugin.plugin.state     = "QUJDRA==";

        preset.effectChain = { drive, plugin };
        return preset;
    }
}

TEST_CASE("A preset survives a serialize/deserialize round trip", "[model][preset]")
{
    const auto original = makeSamplePreset();

    SynthPreset restored;
    REQUIRE(deserializePreset(serializePreset(original), restored));
    REQUIRE(restored == original);
}

TEST_CASE("A preset's name and chain order survive the round trip", "[model][preset]")
{
    const auto original = makeSamplePreset();

    SynthPreset restored;
    REQUIRE(deserializePreset(serializePreset(original), restored));

    REQUIRE(restored.name == "Warm Pad + Tube Drive");
    REQUIRE(restored.effectChain.size() == 2);
    REQUIRE(restored.effectChain[0].kind == EffectKind::Drive);
    REQUIRE(restored.effectChain[1].kind == EffectKind::Plugin);
}

TEST_CASE("A plugin slot's state round-trips exactly", "[model][preset]")
{
    // The whole reason a preset can capture "distortion via an external
    // plugin" rather than only the built-in drive pedal — if the state blob
    // didn't survive, a preset referencing a hosted distortion plugin would
    // load the plugin back at its default settings, not the ones it was
    // saved with.
    const auto original = makeSamplePreset();

    SynthPreset restored;
    REQUIRE(deserializePreset(serializePreset(original), restored));

    const auto& plugin = restored.effectChain[1];
    REQUIRE(plugin.plugin.format == PluginFormat::VST3);
    REQUIRE(plugin.plugin.identifier == "com.example.DistortoMax");
    REQUIRE(plugin.plugin.name == "DistortoMax");
    REQUIRE(plugin.plugin.state == "QUJDRA==");
}

TEST_CASE("An empty effect chain round-trips as an empty chain", "[model][preset]")
{
    SynthPreset preset;
    preset.name = "Plain Synth";

    SynthPreset restored;
    REQUIRE(deserializePreset(serializePreset(preset), restored));
    REQUIRE(restored == preset);
    REQUIRE(restored.effectChain.empty());
}

TEST_CASE("A preset name may contain spaces", "[model][preset]")
{
    SynthPreset preset;
    preset.name = "Aggressive Bass with Wobble and Grit";

    SynthPreset restored;
    REQUIRE(deserializePreset(serializePreset(preset), restored));
    REQUIRE(restored.name == "Aggressive Bass with Wobble and Grit");
}

TEST_CASE("deserializePreset rejects malformed input", "[model][preset]")
{
    SynthPreset out;
    std::string error;
    REQUIRE_FALSE(deserializePreset("", out));
    REQUIRE_FALSE(deserializePreset("not a preset at all", out, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("A preset from a newer build is refused, not part-parsed", "[model][preset]")
{
    SynthPreset out;
    const std::string fromTheFuture = "LOOPERPRESET " + std::to_string(kPresetFormatVersion + 1) + "\nNAME x\n";
    REQUIRE_FALSE(deserializePreset(fromTheFuture, out));
}
