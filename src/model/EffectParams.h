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

        { .kind = EffectKind::Phaser, .id = "phaser", .name = "Phaser", .group = "Modulation",
          .params = {
              { .id = "rate", .name = "Rate", .min = 0.05, .max = 5, .step = 0.05, .unit = " Hz",
                .access = fieldAccess<&S::phaser, &PhaserSettings::rateHz> },
              { .id = "depth", .name = "Depth", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .tooltip = "How far the notches sweep",
                .access = fieldAccess<&S::phaser, &PhaserSettings::depth> },
              { .id = "feedback", .name = "Feedback", .min = -0.9, .max = 0.9, .step = 0.01, .displayScale = 100,
                .unit = " %", .tooltip = "Sharpens the notches; negative moves them",
                .access = fieldAccess<&S::phaser, &PhaserSettings::feedback> },
              { .id = "stages", .name = "Stages", .control = ParamControl::Choice, .min = 1, .max = 6, .step = 1,
                .choices = { "2", "4", "6", "8", "10", "12" },
                .tooltip = "More stages, more notches",
                .access = fieldAccess<&S::phaser, &PhaserSettings::stagePairs> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::phaser, &PhaserSettings::mix> } } },

        { .kind = EffectKind::Flanger, .id = "flanger", .name = "Flanger", .group = "Modulation",
          .params = {
              { .id = "rate", .name = "Rate", .min = 0.05, .max = 5, .step = 0.05, .unit = " Hz",
                .access = fieldAccess<&S::flanger, &FlangerSettings::rateHz> },
              { .id = "depth", .name = "Depth", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::flanger, &FlangerSettings::depth> },
              { .id = "delay", .name = "Delay", .min = 0.1, .max = 5, .step = 0.05, .unit = " ms",
                .tooltip = "The shortest the delay gets, where the sweep starts",
                .access = fieldAccess<&S::flanger, &FlangerSettings::delayMs> },
              { .id = "feedback", .name = "Feedback", .min = -0.95, .max = 0.95, .step = 0.01, .displayScale = 100,
                .unit = " %", .access = fieldAccess<&S::flanger, &FlangerSettings::feedback> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::flanger, &FlangerSettings::mix> } } },

        { .kind = EffectKind::BassTreble, .id = "bassTreble", .name = "Bass and Treble", .group = "",
          .params = {
              { .id = "bass", .name = "Bass", .min = -24, .max = 24, .step = 0.5, .unit = " dB",
                .tooltip = "A shelf below 100 Hz",
                .access = fieldAccess<&S::bassTreble, &BassTrebleSettings::bassDb> },
              { .id = "treble", .name = "Treble", .min = -24, .max = 24, .step = 0.5, .unit = " dB",
                .tooltip = "A shelf above 8 kHz",
                .access = fieldAccess<&S::bassTreble, &BassTrebleSettings::trebleDb> },
              { .id = "volume", .name = "Volume", .min = -24, .max = 24, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::bassTreble, &BassTrebleSettings::volumeDb> } } },

        { .kind = EffectKind::StereoTool, .id = "stereoTool", .name = "Stereo Tools", .group = "Utility",
          .params = {
              { .id = "width", .name = "Width", .min = 0, .max = 2, .step = 0.01, .displayScale = 100, .unit = " %",
                .tooltip = "0% is mono, 100% unchanged, 200% twice as wide",
                .access = fieldAccess<&S::stereoTool, &StereoToolSettings::width> },
              { .id = "balance", .name = "Balance", .min = -1, .max = 1, .step = 0.01, .displayScale = 100,
                .unit = " %", .tooltip = "Turns one side down: negative favours the left",
                .access = fieldAccess<&S::stereoTool, &StereoToolSettings::balance> },
              { .id = "mono", .name = "Mono", .control = ParamControl::Toggle, .min = 0, .max = 1, .step = 1,
                .tooltip = "Fold both channels into one, to hear what a mono speaker will",
                .access = fieldAccess<&S::stereoTool, &StereoToolSettings::mono> },
              { .id = "swap", .name = "Swap Left and Right", .control = ParamControl::Toggle, .min = 0, .max = 1,
                .step = 1, .access = fieldAccess<&S::stereoTool, &StereoToolSettings::swap> } } },

        { .kind = EffectKind::GraphicEq, .id = "graphicEq", .name = "Graphic EQ", .group = "",
          .params = {
              { .id = "band31", .name = "31 Hz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band31> },
              { .id = "band62", .name = "62 Hz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band62> },
              { .id = "band125", .name = "125 Hz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band125> },
              { .id = "band250", .name = "250 Hz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band250> },
              { .id = "band500", .name = "500 Hz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band500> },
              { .id = "band1k", .name = "1 kHz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band1k> },
              { .id = "band2k", .name = "2 kHz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band2k> },
              { .id = "band4k", .name = "4 kHz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band4k> },
              { .id = "band8k", .name = "8 kHz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band8k> },
              { .id = "band16k", .name = "16 kHz", .min = -12, .max = 12, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::graphicEq, &GraphicEqSettings::band16k> } } },

        { .kind = EffectKind::DeEsser, .id = "deEsser", .name = "De-esser", .group = "Dynamics",
          .params = {
              { .id = "frequency", .name = "Frequency", .min = 2000, .max = 12000, .step = 50, .unit = " Hz",
                .tooltip = "Everything above this is what's turned down",
                .access = fieldAccess<&S::deEsser, &DeEsserSettings::frequencyHz> },
              { .id = "threshold", .name = "Threshold", .min = -60, .max = 0, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::deEsser, &DeEsserSettings::thresholdDb> },
              { .id = "reduction", .name = "Max Reduction", .min = 0, .max = 24, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::deEsser, &DeEsserSettings::maxReductionDb> } } },

        { .kind = EffectKind::Expander, .id = "expander", .name = "Expander", .group = "Dynamics",
          .params = {
              { .id = "threshold", .name = "Threshold", .min = -80, .max = 0, .step = 0.5, .unit = " dB",
                .access = fieldAccess<&S::expander, &ExpanderSettings::thresholdDb> },
              { .id = "ratio", .name = "Ratio", .min = 1, .max = 10, .step = 0.1, .unit = " :1",
                .tooltip = "Below the threshold, each dB quieter becomes this many",
                .access = fieldAccess<&S::expander, &ExpanderSettings::ratio> },
              { .id = "range", .name = "Range", .min = 0, .max = 80, .step = 1, .unit = " dB",
                .access = fieldAccess<&S::expander, &ExpanderSettings::rangeDb> },
              { .id = "attack", .name = "Attack", .min = 0.1, .max = 100, .step = 0.1, .unit = " ms",
                .access = fieldAccess<&S::expander, &ExpanderSettings::attackMs> },
              { .id = "release", .name = "Release", .min = 5, .max = 2000, .step = 1, .unit = " ms",
                .access = fieldAccess<&S::expander, &ExpanderSettings::releaseMs> } } },

        { .kind = EffectKind::RingMod, .id = "ringMod", .name = "Ring Modulator", .group = "Modulation",
          .params = {
              { .id = "frequency", .name = "Frequency", .min = 1, .max = 5000, .step = 1, .unit = " Hz",
                .skewMidpoint = 200, .access = fieldAccess<&S::ringMod, &RingModSettings::frequencyHz> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::ringMod, &RingModSettings::mix> } } },

        { .kind = EffectKind::Wah, .id = "wah", .name = "Wah-wah", .group = "Modulation",
          .params = {
              { .id = "rate", .name = "Rate", .min = 0.1, .max = 10, .step = 0.05, .unit = " Hz",
                .access = fieldAccess<&S::wah, &WahSettings::rateHz> },
              { .id = "depth", .name = "Depth", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::wah, &WahSettings::depth> },
              { .id = "resonance", .name = "Resonance", .min = 0.5, .max = 20, .step = 0.1, .unit = " Q",
                .access = fieldAccess<&S::wah, &WahSettings::resonance> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::wah, &WahSettings::mix> } } },

        { .kind = EffectKind::Echo, .id = "echo", .name = "Echo", .group = "",
          .params = {
              { .id = "time", .name = "Time", .min = 1, .max = 2000, .step = 1, .unit = " ms",
                .skewMidpoint = 300, .access = fieldAccess<&S::echo, &EchoSettings::timeMs> },
              { .id = "taps", .name = "Repeats", .control = ParamControl::Choice, .min = 1, .max = 8, .step = 1,
                .choices = { "1", "2", "3", "4", "5", "6", "7", "8" },
                .access = fieldAccess<&S::echo, &EchoSettings::taps> },
              { .id = "decay", .name = "Decay", .min = 0, .max = 0.95, .step = 0.01, .displayScale = 100,
                .unit = " %", .tooltip = "How much of the last repeat's level each one keeps",
                .access = fieldAccess<&S::echo, &EchoSettings::decay> },
              { .id = "mix", .name = "Mix", .min = 0, .max = 1, .step = 0.01, .displayScale = 100, .unit = " %",
                .access = fieldAccess<&S::echo, &EchoSettings::mix> },
              { .id = "pingPong", .name = "Ping-pong", .control = ParamControl::Toggle, .min = 0, .max = 1,
                .step = 1, .tooltip = "Every other repeat on the other side",
                .access = fieldAccess<&S::echo, &EchoSettings::pingPong> } } },

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
    slot.phaser.enabled     = kind == EffectKind::Phaser;
    slot.flanger.enabled    = kind == EffectKind::Flanger;
    slot.bassTreble.enabled = kind == EffectKind::BassTreble;
    slot.stereoTool.enabled = kind == EffectKind::StereoTool;
    slot.graphicEq.enabled  = kind == EffectKind::GraphicEq;
    slot.deEsser.enabled    = kind == EffectKind::DeEsser;
    slot.expander.enabled   = kind == EffectKind::Expander;
    slot.ringMod.enabled    = kind == EffectKind::RingMod;
    slot.wah.enabled        = kind == EffectKind::Wah;
    slot.echo.enabled       = kind == EffectKind::Echo;
    return slot;
}

} // namespace soundsplice::model
