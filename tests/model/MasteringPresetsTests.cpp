#include <catch2/catch_test_macros.hpp>

#include <model/MasteringPresets.h>

#include <iterator>
#include <set>
#include <string>

using namespace looper;

namespace
{
    constexpr engine::MasteringPreset kAll[] = {
        engine::MasteringPreset::Flat,  engine::MasteringPreset::Transparent,
        engine::MasteringPreset::Loud,  engine::MasteringPreset::Warm,
        engine::MasteringPreset::Wide,  engine::MasteringPreset::Voice
    };
}

TEST_CASE("kNumMasteringPresets covers every preset", "[model][masteringpresets]")
{
    // The button row counts to kNumMasteringPresets and casts, so a preset
    // added without bumping the count is simply missing from the UI.
    REQUIRE((size_t) engine::kNumMasteringPresets == std::size(kAll));
}

TEST_CASE("Mastering preset names are distinct and non-empty", "[model][masteringpresets]")
{
    std::set<std::string> names;
    for (auto preset : kAll)
    {
        const std::string name = engine::masteringPresetName(preset);
        REQUIRE_FALSE(name.empty());
        names.insert(name);
    }
    REQUIRE(names.size() == std::size(kAll));
}

TEST_CASE("Flat is the neutral rack, and is switched off", "[model][masteringpresets]")
{
    // Flat is how you get back to nothing, so it has to be both the default
    // values *and* bypassed — leaving it enabled would keep the limiter in
    // the path of someone who just asked for it out.
    const auto flat = model::presetForMastering(engine::MasteringPreset::Flat);
    REQUIRE(flat == model::MasteringSettings {});
    REQUIRE_FALSE(flat.enabled);
}

TEST_CASE("Every other preset is switched on", "[model][masteringpresets]")
{
    // Picking a preset is an explicit request to hear it; one that applied
    // its values while leaving the rack bypassed would look broken.
    for (auto preset : kAll)
    {
        if (preset == engine::MasteringPreset::Flat)
            continue;
        INFO("preset " << engine::masteringPresetName(preset));
        REQUIRE(model::presetForMastering(preset).enabled);
    }
}

TEST_CASE("No preset asks the limiter to raise the ceiling", "[model][masteringpresets]")
{
    // A ceiling at or above 0dBFS is not a ceiling. Everything here must
    // leave at least a little headroom, or the whole point of the stage is
    // lost.
    for (auto preset : kAll)
    {
        const auto m = model::presetForMastering(preset);
        INFO("preset " << engine::masteringPresetName(preset));
        REQUIRE(m.maximizerCeilingDb <= 0.0f);
        REQUIRE(m.maximizerInputDb >= 0.0f);
        REQUIRE(m.maximizerReleaseMs > 0.0f);
    }
}

TEST_CASE("No preset pushes width past mono safety", "[model][masteringpresets]")
{
    // Beyond about 1.5 the mono sum starts losing material even with the
    // widener's mid compensation — and a master that hollows out on a phone
    // is a worse outcome than one that isn't quite as wide.
    for (auto preset : kAll)
    {
        const auto m = model::presetForMastering(preset);
        INFO("preset " << engine::masteringPresetName(preset));
        REQUIRE(m.width >= 0.0f);
        REQUIRE(m.width <= 1.5f);
    }
}

TEST_CASE("Preset filter settings are physically valid", "[model][masteringpresets]")
{
    // A zero frequency or Q would be a broken filter rather than a neutral
    // one, and the difference is silence or a blow-up, not a subtle change.
    for (auto preset : kAll)
    {
        const auto m = model::presetForMastering(preset);
        INFO("preset " << engine::masteringPresetName(preset));

        REQUIRE(m.lowShelfHz > 0.0f);
        REQUIRE(m.peakHz > 0.0f);
        REQUIRE(m.highShelfHz > 0.0f);
        REQUIRE(m.peakQ > 0.0f);
        REQUIRE(m.lowShelfHz < m.highShelfHz);
        REQUIRE(m.exciterCrossoverHz > 0.0f);

        for (float db : { m.lowShelfDb, m.peakDb, m.highShelfDb })
        {
            REQUIRE(db >= -18.0f);
            REQUIRE(db <= 18.0f);
        }
    }
}

TEST_CASE("The presets actually differ from each other", "[model][masteringpresets]")
{
    // Six buttons that all load the same rack would pass every test above.
    std::set<std::string> fingerprints;
    for (auto preset : kAll)
    {
        const auto m = model::presetForMastering(preset);
        fingerprints.insert(std::to_string(m.lowShelfDb) + "/" + std::to_string(m.peakDb) + "/"
                            + std::to_string(m.highShelfDb) + "/" + std::to_string(m.width) + "/"
                            + std::to_string(m.maximizerInputDb) + "/"
                            + std::to_string(m.exciterAmount));
    }
    REQUIRE(fingerprints.size() == std::size(kAll));
}
