#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include <model/GuitarTonePresets.h>

using namespace looper;

namespace
{
    constexpr engine::GuitarTone kAllTones[] = { engine::GuitarTone::ModernMetal,
                                                 engine::GuitarTone::CleanJazz,
                                                 engine::GuitarTone::ClassicRockCrunch,
                                                 engine::GuitarTone::AmbientShoegaze,
                                                 engine::GuitarTone::FunkPercussive,
                                                 engine::GuitarTone::IndustrialCyber };
}

TEST_CASE("kNumGuitarTones covers every tone the table handles", "[model][guitartone]")
{
    // FretboardPane builds its button row by counting to kNumGuitarTones and
    // casting, so a tone added to the enum without bumping the count is
    // simply missing from the UI - silently, with nothing else failing.
    REQUIRE((size_t) engine::kNumGuitarTones == std::size(kAllTones));
}

TEST_CASE("Guitar tone names are distinct and non-empty", "[model][guitartone]")
{
    std::set<std::string> names;
    for (auto tone : kAllTones)
    {
        const std::string name = engine::guitarToneName(tone);
        REQUIRE_FALSE(name.empty());
        names.insert(name);
    }
    REQUIRE(names.size() == std::size(kAllTones));
}

TEST_CASE("Every guitar tone's tuning is playable", "[model][guitartone]")
{
    // A tuning is the one guitar setting that can make a preset silent
    // rather than merely wrong: notes below the lowest open string can't be
    // fretted at all (see engine::GuitarNode), so a tuning that ran off the
    // bottom would just not sound. Strings must also ascend, or the
    // fretboard's low-to-high layout lies about the instrument.
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForGuitarTone(tone);
        INFO("tone " << engine::guitarToneName(tone));

        for (size_t s = 0; s < preset.guitar.tuning.size(); ++s)
        {
            REQUIRE(preset.guitar.tuning[s] >= 0);
            REQUIRE(preset.guitar.tuning[s] <= 127);
            if (s > 0)
                REQUIRE(preset.guitar.tuning[s] > preset.guitar.tuning[s - 1]);
        }
    }
}

TEST_CASE("Guitar tone settings stay in their documented ranges", "[model][guitartone]")
{
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForGuitarTone(tone);
        INFO("tone " << engine::guitarToneName(tone));

        REQUIRE(preset.guitar.decaySeconds > 0.0f);
        for (float normalised : { preset.guitar.brightness, preset.guitar.pickPosition,
                                  preset.guitar.pickHardness, preset.guitar.muteOnNoteOff })
        {
            REQUIRE(normalised >= 0.0f);
            REQUIRE(normalised <= 1.0f);
        }
    }
}

TEST_CASE("Each guitar tone slot's active settings are enabled, matching its kind", "[model][guitartone]")
{
    // Same invariant GenrePresetsTests holds: slot.enabled and
    // slot.<kind>.enabled are set together, so a slot can't inherit a stale
    // "enabled" from whichever kind used to occupy it.
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForGuitarTone(tone);
        INFO("tone " << engine::guitarToneName(tone));
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

TEST_CASE("A gated tone always has something driving it", "[model][guitartone]")
{
    // A gate ahead of the distortion it exists to clean up is the one
    // ordering mistake in these chains that would be heard as "the gate
    // doesn't work" rather than as a wrong setting - it would be gating the
    // clean signal's much quieter noise floor instead of the noise the
    // drive adds. See the gate-placement note in docs/PLAN.md.
    for (auto tone : kAllTones)
    {
        const auto preset = model::presetForGuitarTone(tone);
        INFO("tone " << engine::guitarToneName(tone));

        int firstDrive = -1, firstGate = -1;
        for (size_t i = 0; i < preset.effectChain.size(); ++i)
        {
            const auto kind = preset.effectChain[i].kind;
            if (firstDrive < 0 && kind == model::EffectKind::Drive)
                firstDrive = (int) i;
            if (firstGate < 0 && kind == model::EffectKind::Gate)
                firstGate = (int) i;
        }

        if (firstGate >= 0)
        {
            REQUIRE(firstDrive >= 0);
            REQUIRE(firstDrive < firstGate);
        }
    }
}
