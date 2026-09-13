#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <set>
#include <string>

#include <model/GenrePresets.h>

using namespace looper;

namespace
{
    constexpr engine::Genre kAllGenres[] = { engine::Genre::House,  engine::Genre::Techno,
                                              engine::Genre::HipHop, engine::Genre::Trap,
                                              engine::Genre::Ambient, engine::Genre::LoFi,
                                              engine::Genre::Synthwave, engine::Genre::Cyberpunk };
}

TEST_CASE("Every genre has a preset with a name", "[model][genrepresets]")
{
    for (auto genre : kAllGenres)
        REQUIRE_FALSE(model::presetForGenre(genre).name.empty());
}

TEST_CASE("Genre preset names are distinct", "[model][genrepresets]")
{
    std::set<std::string> names;
    for (auto genre : kAllGenres)
        names.insert(model::presetForGenre(genre).name);
    REQUIRE(names.size() == std::size(kAllGenres));
}

TEST_CASE("Genre preset waveforms are valid", "[model][genrepresets]")
{
    for (auto genre : kAllGenres)
    {
        const auto preset = model::presetForGenre(genre);
        INFO("genre " << preset.name);
        REQUIRE(preset.synth.waveform >= 0);
        REQUIRE(preset.synth.waveform <= 3);
    }
}

TEST_CASE("Every genre preset has at least one enabled effect", "[model][genrepresets]")
{
    // Not a hard musical requirement in general, but every genre preset here
    // was deliberately built with at least one effect that defines its
    // character (chorus/drive/reverb/delay) - an empty chain would mean a
    // genre preset that doesn't actually do anything.
    for (auto genre : kAllGenres)
    {
        const auto preset = model::presetForGenre(genre);
        INFO("genre " << preset.name);
        REQUIRE_FALSE(preset.effectChain.empty());
        for (const auto& slot : preset.effectChain)
            REQUIRE(slot.enabled);
    }
}

TEST_CASE("Each effect slot's active settings are enabled, matching its kind", "[model][genrepresets]")
{
    // The invariant seedFactoryPresets() also maintains: slot.enabled and
    // slot.<kind>.enabled are set together, so switching a slot's kind in
    // the UI later can't silently inherit a stale "enabled" from whichever
    // kind used to occupy it.
    for (auto genre : kAllGenres)
    {
        const auto preset = model::presetForGenre(genre);
        INFO("genre " << preset.name);
        for (const auto& slot : preset.effectChain)
        {
            switch (slot.kind)
            {
                case model::EffectKind::Filter:     REQUIRE(slot.filter.enabled); break;
                case model::EffectKind::Delay:      REQUIRE(slot.delay.enabled); break;
                case model::EffectKind::Reverb:     REQUIRE(slot.reverb.enabled); break;
                case model::EffectKind::Drive:      REQUIRE(slot.drive.enabled); break;
                case model::EffectKind::Compressor: REQUIRE(slot.compressor.enabled); break;
                case model::EffectKind::Tremolo:    REQUIRE(slot.tremolo.enabled); break;
                case model::EffectKind::Chorus:     REQUIRE(slot.chorus.enabled); break;
                case model::EffectKind::Wobble:     REQUIRE(slot.wobble.enabled); break;
                case model::EffectKind::Gate:       REQUIRE(slot.gate.enabled); break;
                case model::EffectKind::Eq:         REQUIRE(slot.eqPedal.enabled); break;
                case model::EffectKind::Plugin:     break; // no on/off flag of its own
            }
        }
    }
}
