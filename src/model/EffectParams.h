#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

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

    JUCE-free so those checks run headless. Storage is unchanged: the table
    reads and writes the existing settings structs, so project files are
    exactly as they were.
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
inline const std::vector<EffectDescriptor>& builtInEffects()
{
    using S = EffectSlot;

    static const std::vector<EffectDescriptor> effects {
        { .kind = EffectKind::Filter, .id = "filter", .name = "Filter", .group = "",
          .params = {
              { .id = "mode", .name = "Mode", .control = ParamControl::Choice, .min = 0, .max = 2, .step = 1,
                .choices = { "Low-pass", "High-pass", "Band-pass" },
                .access = fieldAccess<&S::filter, &FilterSettings::mode> },
              { .id = "cutoff", .name = "Cutoff", .min = 20, .max = 18000, .step = 1, .unit = " Hz",
                .skewMidpoint = 1000, .access = fieldAccess<&S::filter, &FilterSettings::cutoff> },
              { .id = "resonance", .name = "Resonance", .min = 0.1, .max = 5, .step = 0.01, .unit = " Q",
                .access = fieldAccess<&S::filter, &FilterSettings::resonance> } } },

        { .kind = EffectKind::Delay, .id = "delay", .name = "Delay", .group = "",
          .params = {
              { .id = "time", .name = "Time", .min = 20, .max = 1000, .step = 1, .unit = " ms",
                .access = fieldAccess<&S::delay, &DelaySettings::timeMs> },
              { .id = "feedback", .name = "Feedback", .min = 0, .max = 0.95, .step = 0.01, .displayScale = 100,
                .unit = " %", .access = fieldAccess<&S::delay, &DelaySettings::feedback> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::delay, &DelaySettings::mix> } } },

        { .kind = EffectKind::Reverb, .id = "reverb", .name = "Reverb", .group = "",
          .params = {
              { .id = "room", .name = "Room Size", .min = 0, .max = 1, .step = 0.01, .displayScale = 100,
                .unit = " %", .access = fieldAccess<&S::reverb, &ReverbSettings::roomSize> },
              { .id = "damping", .name = "Damping", .min = 0, .max = 1, .step = 0.01, .displayScale = 100,
                .unit = " %", .access = fieldAccess<&S::reverb, &ReverbSettings::damping> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::reverb, &ReverbSettings::mix> } } },

        { .kind = EffectKind::Drive, .id = "drive", .name = "Drive", .group = "Pedals",
          .params = {
              { .id = "drive", .name = "Drive", .min = 1, .max = 40, .step = 0.1, .unit = " x",
                .access = fieldAccess<&S::drive, &DriveSettings::drive> },
              { .id = "tone", .name = "Tone", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::drive, &DriveSettings::tone> },
              { .id = "level", .name = "Level", .min = 0, .max = 1.5, .step = 0.01, .displayScale = 100,
                .unit = " %", .access = fieldAccess<&S::drive, &DriveSettings::level> },
              { .id = "asymmetry", .name = "Asymmetry", .min = -1, .max = 1, .step = 0.01, .displayScale = 100,
                .unit = " %",
                .tooltip = "Bias into the clipper - adds the even harmonics a symmetric curve can't make",
                .access = fieldAccess<&S::drive, &DriveSettings::asymmetry> },
              { .id = "stages", .name = "Stages", .control = ParamControl::Choice, .min = 1, .max = 3, .step = 1,
                .choices = { "1 - pedal", "2", "3 - amp" },
                .tooltip = "Gain stages in series - more is a different kind of distortion, not just more of it",
                .access = fieldAccess<&S::drive, &DriveSettings::stages> },
              { .id = "hardClip", .name = "Fuzz (hard clip)", .control = ParamControl::Toggle, .min = 0, .max = 1,
                .step = 1, .access = fieldAccess<&S::drive, &DriveSettings::hardClip> },
              { .id = "cabinet", .name = "Cabinet", .control = ParamControl::Toggle, .min = 0, .max = 1, .step = 1,
                .tooltip = "Speaker simulation - without it, distortion is heard as fizz",
                .access = fieldAccess<&S::drive, &DriveSettings::cabinet> },
              { .id = "cabinetIr", .name = "Cabinet IR", .control = ParamControl::Toggle, .min = 0, .max = 1,
                .step = 1, .tooltip = "Convolve a cabinet impulse response instead of the cabinet filters",
                .access = fieldAccess<&S::drive, &DriveSettings::cabinetIr> },
              { .id = "oversample", .name = "Oversample (4x)", .control = ParamControl::Toggle, .min = 0, .max = 1,
                .step = 1, .tooltip = "Runs the clipper at 4x - costs CPU, removes the aliasing grit of high gain",
                .access = fieldAccess<&S::drive, &DriveSettings::oversample> } } },

        { .kind = EffectKind::Compressor, .id = "compressor", .name = "Compressor", .group = "Pedals",
          .params = {
              { .id = "threshold", .name = "Threshold", .min = -60, .max = 0, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::compressor, &CompressorSettings::thresholdDb> },
              { .id = "ratio", .name = "Ratio", .min = 1, .max = 20, .step = 0.1, .unit = " :1",
                .access = fieldAccess<&S::compressor, &CompressorSettings::ratio> },
              { .id = "attack", .name = "Attack", .min = 0.5, .max = 200, .step = 0.5, .unit = " ms",
                .access = fieldAccess<&S::compressor, &CompressorSettings::attackMs> },
              { .id = "release", .name = "Release", .min = 10, .max = 1000, .step = 1, .unit = " ms",
                .access = fieldAccess<&S::compressor, &CompressorSettings::releaseMs> },
              { .id = "makeUp", .name = "Make-up", .min = -12, .max = 24, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::compressor, &CompressorSettings::makeUpDb> } } },

        { .kind = EffectKind::Tremolo, .id = "tremolo", .name = "Tremolo", .group = "Pedals",
          .params = {
              { .id = "rate", .name = "Rate", .min = 0.1, .max = 20, .step = 0.1, .unit = " Hz",
                .access = fieldAccess<&S::tremolo, &TremoloSettings::rateHz> },
              { .id = "depth", .name = "Depth", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::tremolo, &TremoloSettings::depth> } } },

        { .kind = EffectKind::Chorus, .id = "chorus", .name = "Chorus", .group = "Pedals",
          .params = {
              { .id = "rate", .name = "Rate", .min = 0.05, .max = 8, .step = 0.05, .unit = " Hz",
                .access = fieldAccess<&S::chorus, &ChorusSettings::rateHz> },
              { .id = "depth", .name = "Depth", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::chorus, &ChorusSettings::depth> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::chorus, &ChorusSettings::mix> } } },

        // Rate is beats per cycle, not Hz: 0.25 is a sixteenth note, 1.0 a
        // quarter, which is how a wobble is dialled in, and it stays locked to
        // the bar as the tempo changes.
        { .kind = EffectKind::Wobble, .id = "wobble", .name = "Wobble", .group = "Pedals",
          .params = {
              { .id = "rate", .name = "Rate", .min = 0.0625, .max = 4, .step = 0.0625, .unit = " beats",
                .access = fieldAccess<&S::wobble, &WobbleSettings::rateBeats> },
              { .id = "depth", .name = "Depth", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::wobble, &WobbleSettings::depth> },
              { .id = "cutoff", .name = "Cutoff", .min = 40, .max = 4000, .step = 1, .unit = " Hz",
                .skewMidpoint = 400, .access = fieldAccess<&S::wobble, &WobbleSettings::baseCutoffHz> },
              { .id = "resonance", .name = "Resonance", .min = 0.1, .max = 5, .step = 0.01, .unit = " Q",
                .access = fieldAccess<&S::wobble, &WobbleSettings::resonance> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::wobble, &WobbleSettings::mix> } } },

        { .kind = EffectKind::Gate, .id = "gate", .name = "Gate", .group = "Pedals",
          .params = {
              { .id = "threshold", .name = "Threshold", .min = -80, .max = 0, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::gate, &GateSettings::thresholdDb> },
              { .id = "range", .name = "Range", .min = 0, .max = 90, .step = 1, .unit = " dB",
                .access = fieldAccess<&S::gate, &GateSettings::rangeDb> },
              { .id = "attack", .name = "Attack", .min = 0.1, .max = 50, .step = 0.1, .unit = " ms",
                .access = fieldAccess<&S::gate, &GateSettings::attackMs> },
              { .id = "hold", .name = "Hold", .min = 0, .max = 500, .step = 1, .unit = " ms",
                .access = fieldAccess<&S::gate, &GateSettings::holdMs> },
              { .id = "release", .name = "Release", .min = 1, .max = 2000, .step = 1, .unit = " ms",
                .access = fieldAccess<&S::gate, &GateSettings::releaseMs> } } },

        { .kind = EffectKind::Eq, .id = "eq", .name = "EQ", .group = "Pedals",
          .params = {
              { .id = "lowFreq", .name = "Low Freq", .min = 20, .max = 1000, .step = 1, .unit = " Hz",
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::lowShelfHz> },
              { .id = "low", .name = "Low", .min = -24, .max = 24, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::lowShelfDb> },
              { .id = "midFreq", .name = "Mid Freq", .min = 100, .max = 8000, .step = 1, .unit = " Hz",
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::midHz> },
              { .id = "mid", .name = "Mid", .min = -24, .max = 24, .step = 0.5, .unit = " dB",
                .tooltip = "The mid scoop or push - the EQ decision a rock or metal tone turns on",
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::midDb> },
              { .id = "midQ", .name = "Mid Q", .min = 0.2, .max = 8, .step = 0.05,
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::midQ> },
              { .id = "highFreq", .name = "High Freq", .min = 1000, .max = 16000, .step = 10, .unit = " Hz",
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::highShelfHz> },
              { .id = "high", .name = "High", .min = -24, .max = 24, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::eqPedal, &EqPedalSettings::highShelfDb> } } },

        { .kind = EffectKind::Limiter, .id = "limiter", .name = "Limiter", .group = "Pedals",
          .params = {
              { .id = "input", .name = "Input Gain", .min = 0, .max = 24, .step = 0.1, .unit = " dB",
                .tooltip = "Drive into the limiter - louder, with the peaks held at the ceiling",
                .access = fieldAccess<&S::limiter, &LimiterSettings::inputGainDb> },
              { .id = "ceiling", .name = "Ceiling", .min = -24, .max = 0, .step = 0.1, .unit = " dB",
                .tooltip = "Nothing gets past this",
                .access = fieldAccess<&S::limiter, &LimiterSettings::ceilingDb> },
              { .id = "release", .name = "Release", .min = 1, .max = 1000, .step = 1, .unit = " ms",
                .access = fieldAccess<&S::limiter, &LimiterSettings::releaseMs> } } },

        { .kind = EffectKind::Amplify, .id = "amplify", .name = "Amplify", .group = "Utility",
          .params = {
              { .id = "gain", .name = "Gain", .min = -48, .max = 48, .step = 0.1, .unit = " dB",
                .access = fieldAccess<&S::amplify, &AmplifySettings::gainDb> } } },

        { .kind = EffectKind::Invert, .id = "invert", .name = "Invert", .group = "Utility",
          .params = {
              { .id = "left", .name = "Invert Left", .control = ParamControl::Toggle, .min = 0, .max = 1, .step = 1,
                .tooltip = "Flip the polarity of the left channel (a mono track's only channel)",
                .access = fieldAccess<&S::invert, &InvertSettings::left> },
              { .id = "right", .name = "Invert Right", .control = ParamControl::Toggle, .min = 0, .max = 1,
                .step = 1, .access = fieldAccess<&S::invert, &InvertSettings::right> } } },

        { .kind = EffectKind::DcOffset, .id = "dcOffset", .name = "DC Offset Removal", .group = "Utility",
          .params = {
              { .id = "cutoff", .name = "Cutoff", .min = 1, .max = 20, .step = 0.5, .unit = " Hz",
                .tooltip = "The high-pass corner: low enough to leave every audible frequency alone",
                .access = fieldAccess<&S::dcOffset, &DcOffsetSettings::cutoffHz> } } },
    };

    return effects;
}

/** The descriptor for @p kind, or nullptr for a hosted plugin, whose
    parameters belong to the plugin itself. */
inline const EffectDescriptor* descriptorFor(EffectKind kind)
{
    for (const auto& effect : builtInEffects())
        if (effect.kind == kind)
            return &effect;
    return nullptr;
}

/** A new, enabled slot of @p kind with its default settings.

    Also sets the per-kind `enabled` flag the way loading a project does
    (true only for the slot's own kind), so a slot added in the app compares
    equal to the same slot after a save and reload. */
inline EffectSlot makeEffectSlot(EffectKind kind)
{
    EffectSlot slot;
    slot.kind    = kind;
    slot.enabled = true; // added because you want to hear it

    slot.filter.enabled     = kind == EffectKind::Filter;
    slot.delay.enabled      = kind == EffectKind::Delay;
    slot.reverb.enabled     = kind == EffectKind::Reverb;
    slot.drive.enabled      = kind == EffectKind::Drive;
    slot.compressor.enabled = kind == EffectKind::Compressor;
    slot.tremolo.enabled    = kind == EffectKind::Tremolo;
    slot.chorus.enabled     = kind == EffectKind::Chorus;
    slot.wobble.enabled     = kind == EffectKind::Wobble;
    slot.gate.enabled       = kind == EffectKind::Gate;
    slot.eqPedal.enabled    = kind == EffectKind::Eq;
    slot.amplify.enabled    = kind == EffectKind::Amplify;
    slot.invert.enabled     = kind == EffectKind::Invert;
    slot.dcOffset.enabled   = kind == EffectKind::DcOffset;
    slot.limiter.enabled    = kind == EffectKind::Limiter;
    return slot;
}

} // namespace soundsplice::model
