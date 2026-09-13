#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include <model/GenrePresets.h>
#include <model/SynthTonePresets.h>

using namespace looper;

namespace
{
    constexpr engine::SynthTone kAllTones[] = { engine::SynthTone::CyberBass,
                                                engine::SynthTone::CyberLead,
                                                engine::SynthTone::DarkPad };
}

TEST_CASE("Every synth tone has a preset with a name", "[model][synthtone]")
{
    for (auto tone : kAllTones)
        REQUIRE_FALSE(model::presetForSynthTone(tone).name.empty());
}

TEST_CASE("Synth tone names are distinct", "[model][synthtone]")
{
    std::set<std::string> names;
    for (auto tone : kAllTones)
        names.insert(model::presetForSynthTone(tone).name);
    REQUIRE(names.size() == (size_t) engine::kNumSynthTones);
}

TEST_CASE("kNumSynthTones covers every tone the table handles", "[model][synthtone]")
{
    // The button row is built by counting to kNumSynthTones and casting, so
    // a tone added to the enum without bumping the count is simply absent
    // from the UI - silently, and with nothing else failing.
    REQUIRE((size_t) engine::kNumSynthTones == std::size(kAllTones));

    for (int i = 0; i < engine::kNumSynthTones; ++i)
    {
        const auto tone = (engine::SynthTone) i;
        INFO("tone index " << i);
        REQUIRE_FALSE(model::presetForSynthTone(tone).name.empty());
    }
}

TEST_CASE("Synth tone waveforms are valid", "[model][synthtone]")
{
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForSynthTone(tone);
        INFO("tone " << preset.name);
        REQUIRE(preset.synth.waveform >= 0);
        REQUIRE(preset.synth.waveform <= 3);
    }
}

TEST_CASE("Each synth tone slot's active settings are enabled, matching its kind", "[model][synthtone]")
{
    // The same invariant GenrePresetsTests holds for genre presets:
    // slot.enabled and slot.<kind>.enabled are set together, so a slot whose
    // kind was changed can't inherit a stale "enabled" from the kind that
    // used to occupy it.
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForSynthTone(tone);
        INFO("tone " << preset.name);
        REQUIRE_FALSE(preset.effectChain.empty());

        for (const auto& slot : preset.effectChain)
        {
            REQUIRE(slot.enabled);
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

TEST_CASE("Unison and sub-osc settings stay in their usable ranges", "[model][synthtone]")
{
    // These tones lean on unison harder than anything else in the app, and
    // a voice count of zero would silence the oscillator entirely.
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForSynthTone(tone);
        INFO("tone " << preset.name);
        REQUIRE(preset.synth.unisonVoices >= 1);
        REQUIRE(preset.synth.unisonDetuneCents >= 0.0f);
        REQUIRE(preset.synth.subOscLevel >= 0.0f);
        REQUIRE(preset.synth.subOscLevel <= 1.0f);
        REQUIRE(preset.synth.filterCutoff > 0.0f);
    }
}

TEST_CASE("The Cyberpunk genre preset is the Cyber Bass tone", "[model][synthtone]")
{
    // presetForGenre delegates rather than restating ~20 hand-tuned fields.
    // If that delegation is ever replaced by a copy, this catches the first
    // time the two drift.
    const auto genre = model::presetForGenre(engine::Genre::Cyberpunk);
    const auto tone  = model::presetForSynthTone(engine::SynthTone::CyberBass);

    REQUIRE(genre.synth == tone.synth);
    REQUIRE(genre.effectChain == tone.effectChain);

    // Only the display name differs - a generated loop's preset is named
    // after its genre, not after the tone it borrows.
    REQUIRE(genre.name == std::string("Cyberpunk"));
}
