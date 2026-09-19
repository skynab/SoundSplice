#pragma once

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "model/EffectParams.h"

namespace soundsplice::model
{
/**
    Effect presets: named sets of parameter values for a built-in effect.

    Values are keyed by parameter id (see EffectParam::id), not by position.
    A preset saved today still applies after an effect gains a parameter
    (which then takes its default) or loses one (whose value is ignored), and
    a preset can never land a value on the wrong control because the order
    changed.

    Factory presets ship with the app. User presets live in the app's
    settings rather than in a project, because a sound someone has dialled in
    is something they reach for across projects; serializeUserPresets is the
    text form they are stored in.
*/
struct EffectPreset
{
    std::string                                 name;
    std::vector<std::pair<std::string, double>> values; // parameter id -> stored value

    bool operator==(const EffectPreset&) const = default;
};

/** A preset the user saved, and which effect it belongs to (EffectDescriptor::id). */
struct UserEffectPreset
{
    std::string  effectId;
    EffectPreset preset;

    bool operator==(const UserEffectPreset&) const = default;
};

/** @p slot's current settings as a preset called @p name. */
inline EffectPreset capturePreset(const EffectSlot& slot, const EffectDescriptor& descriptor, std::string name)
{
    EffectPreset preset;
    preset.name = std::move(name);

    for (const auto& param : descriptor.params)
        preset.values.emplace_back(param.id, paramValue(slot, param));

    return preset;
}

/** Applies @p preset to @p slot. Parameters the preset names are set
    (clamped to their range); parameters it leaves out go back to their
    defaults, so a preset always sounds the same whatever was dialled in
    before it; ids it names that the effect doesn't have are ignored.

    Returns false, changing nothing, for a slot with no descriptor (a plugin). */
inline bool applyPreset(EffectSlot& slot, const EffectPreset& preset)
{
    const auto* descriptor = descriptorFor(slot.kind);
    if (descriptor == nullptr)
        return false;

    const auto defaults = makeEffectSlot(slot.kind);

    for (const auto& param : descriptor->params)
    {
        const auto named = std::find_if(preset.values.begin(), preset.values.end(),
                                        [&param](const auto& value) { return value.first == param.id; });

        if (named != preset.values.end())
            setParamValue(slot, param, named->second);
        else
            param.access.set(slot, paramValue(defaults, param)); // exactly the default, not snapped
    }

    return true;
}

/** The presets that ship with @p kind. Starting points, each reachable by
    hand from the controls; empty for a plugin. */
inline const std::vector<EffectPreset>& factoryPresets(EffectKind kind)
{
    static const std::vector<std::pair<EffectKind, std::vector<EffectPreset>>> table {
        { EffectKind::Filter, {
            { "Rumble Cut",   { { "mode", 1 }, { "cutoff", 80 },   { "resonance", 0.71 } } },
            { "Warm Top",     { { "mode", 0 }, { "cutoff", 6000 }, { "resonance", 0.71 } } },
            { "Telephone",    { { "mode", 2 }, { "cutoff", 1500 }, { "resonance", 1.2 } } } } },

        { EffectKind::Delay, {
            { "Slapback",     { { "time", 110 }, { "feedback", 0.1 }, { "mix", 0.25 } } },
            { "Echo",         { { "time", 380 }, { "feedback", 0.4 }, { "mix", 0.3 } } },
            { "Long Ambient", { { "time", 750 }, { "feedback", 0.6 }, { "mix", 0.25 } } } } },

        { EffectKind::Reverb, {
            { "Small Room",   { { "room", 0.3 },  { "damping", 0.6 }, { "mix", 0.18 } } },
            { "Hall",         { { "room", 0.75 }, { "damping", 0.4 }, { "mix", 0.28 } } },
            { "Cathedral",    { { "room", 0.95 }, { "damping", 0.2 }, { "mix", 0.38 } } } } },

        { EffectKind::Drive, {
            { "Light Overdrive", { { "drive", 3 },  { "tone", 0.55 }, { "level", 0.8 }, { "cabinet", 1 } } },
            { "Crunch",          { { "drive", 8 },  { "tone", 0.5 },  { "level", 0.7 }, { "asymmetry", 0.15 },
                                   { "stages", 2 }, { "cabinet", 1 } } },
            { "Fuzz",            { { "drive", 25 }, { "tone", 0.45 }, { "level", 0.6 }, { "hardClip", 1 },
                                   { "cabinet", 1 }, { "oversample", 1 } } } } },

        { EffectKind::Compressor, {
            { "Gentle Glue",      { { "threshold", -18 }, { "ratio", 2 },  { "attack", 30 },  { "release", 200 },
                                    { "makeUp", 2 } } },
            { "Vocal",            { { "threshold", -22 }, { "ratio", 4 },  { "attack", 8 },   { "release", 120 },
                                    { "makeUp", 4 } } },
            { "Podcast Leveller", { { "threshold", -26 }, { "ratio", 6 },  { "attack", 5 },   { "release", 150 },
                                    { "makeUp", 6 } } },
            { "Peak Catcher",     { { "threshold", -8 },  { "ratio", 20 }, { "attack", 0.5 }, { "release", 60 },
                                    { "makeUp", 0 } } } } },

        { EffectKind::Tremolo, {
            { "Slow Pulse", { { "rate", 3 }, { "depth", 0.5 } } },
            { "Fast Chop",  { { "rate", 9 }, { "depth", 0.9 } } } } },

        { EffectKind::Chorus, {
            { "Subtle", { { "rate", 0.4 }, { "depth", 0.3 }, { "mix", 0.35 } } },
            { "Lush",   { { "rate", 0.8 }, { "depth", 0.7 }, { "mix", 0.5 } } } } },

        { EffectKind::Wobble, {
            { "Sixteenth Wub",   { { "rate", 0.25 }, { "depth", 0.8 }, { "cutoff", 150 }, { "resonance", 1.2 },
                                   { "mix", 1 } } },
            { "Half-Note Sweep", { { "rate", 2 },    { "depth", 0.7 }, { "cutoff", 250 }, { "resonance", 0.9 },
                                   { "mix", 1 } } } } },

        { EffectKind::Gate, {
            { "Noise Floor", { { "threshold", -50 }, { "range", 40 }, { "attack", 1 },   { "hold", 30 },
                               { "release", 200 } } },
            { "Tight Drums", { { "threshold", -30 }, { "range", 80 }, { "attack", 0.5 }, { "hold", 10 },
                               { "release", 60 } } } } },

        { EffectKind::Limiter, {
            { "Streaming Ceiling", { { "input", 0 }, { "ceiling", -1 },   { "release", 100 } } },
            { "Broadcast",         { { "input", 0 }, { "ceiling", -2 },   { "release", 150 } } },
            { "Loud",              { { "input", 6 }, { "ceiling", -0.3 }, { "release", 50 } } } } },

        { EffectKind::Phaser, {
            { "Slow Swirl", { { "rate", 0.2 }, { "depth", 0.8 }, { "feedback", 0.4 }, { "stages", 3 }, { "mix", 0.5 } } },
            { "Jet",        { { "rate", 1.5 }, { "depth", 0.9 }, { "feedback", 0.75 }, { "stages", 6 }, { "mix", 0.5 } } },
            { "Subtle",     { { "rate", 0.4 }, { "depth", 0.5 }, { "feedback", 0.1 }, { "stages", 2 }, { "mix", 0.35 } } } } },

        { EffectKind::Flanger, {
            { "Classic",  { { "rate", 0.25 }, { "depth", 0.7 }, { "delay", 1 },   { "feedback", 0.5 },  { "mix", 0.5 } } },
            { "Metallic", { { "rate", 0.1 },  { "depth", 0.5 }, { "delay", 0.5 }, { "feedback", 0.85 }, { "mix", 0.5 } } },
            { "Wide",     { { "rate", 0.5 },  { "depth", 1 },   { "delay", 2 },   { "feedback", -0.4 }, { "mix", 0.5 } } } } },

        { EffectKind::BassTreble, {
            { "Bass Boost",  { { "bass", 6 },  { "treble", 0 },  { "volume", -3 } } },
            { "Brighten",    { { "bass", 0 },  { "treble", 4 },  { "volume", -1 } } },
            { "Telephone",   { { "bass", -24 }, { "treble", -18 }, { "volume", 6 } } } } },

        { EffectKind::StereoTool, {
            { "Mono Check",   { { "width", 1 },   { "balance", 0 }, { "mono", 1 }, { "swap", 0 } } },
            { "Wider",        { { "width", 1.4 }, { "balance", 0 }, { "mono", 0 }, { "swap", 0 } } },
            { "Narrower",     { { "width", 0.6 }, { "balance", 0 }, { "mono", 0 }, { "swap", 0 } } } } },

        { EffectKind::Dynamics, {
            { "Unity", { { "points", 2 },
                                { "point1In", -80 }, { "point1Out", -80 },
                                { "point2In", 0 }, { "point2Out", 0 },
                                { "point3In", -60 }, { "point3Out", -60 },
                                { "point4In", -40 }, { "point4Out", -40 },
                                { "point5In", -20 }, { "point5Out", -20 },
                                { "point6In", -10 }, { "point6Out", -10 },
                                { "detector", 0 }, { "attack", 5 }, { "release", 150 }, { "makeUp", 0 } } },
            { "Gentle Compressor", { { "points", 3 },
                                { "point1In", -80 }, { "point1Out", -80 },
                                { "point2In", -20 }, { "point2Out", -20 },
                                { "point3In", 0 }, { "point3Out", -10 },
                                { "point4In", -40 }, { "point4Out", -40 },
                                { "point5In", -20 }, { "point5Out", -20 },
                                { "point6In", -10 }, { "point6Out", -10 },
                                { "detector", 1 }, { "attack", 10 }, { "release", 150 }, { "makeUp", 3 } } },
            { "Noise Gate", { { "points", 4 },
                                { "point1In", -100 }, { "point1Out", -100 },
                                { "point2In", -55 }, { "point2Out", -100 },
                                { "point3In", -50 }, { "point3Out", -50 },
                                { "point4In", 0 }, { "point4Out", 0 },
                                { "point5In", -20 }, { "point5Out", -20 },
                                { "point6In", -10 }, { "point6Out", -10 },
                                { "detector", 0 }, { "attack", 1 }, { "release", 80 }, { "makeUp", 0 } } },
            { "Limiter", { { "points", 3 },
                                { "point1In", -80 }, { "point1Out", -80 },
                                { "point2In", -6 }, { "point2Out", -6 },
                                { "point3In", 0 }, { "point3Out", -5 },
                                { "point4In", -40 }, { "point4Out", -40 },
                                { "point5In", -20 }, { "point5Out", -20 },
                                { "point6In", -10 }, { "point6Out", -10 },
                                { "detector", 0 }, { "attack", 0.5 }, { "release", 60 }, { "makeUp", 0 } } },
            { "Voice Leveler", { { "points", 4 },
                                { "point1In", -80 }, { "point1Out", -80 },
                                { "point2In", -55 }, { "point2Out", -55 },
                                { "point3In", -40 }, { "point3Out", -28 },
                                { "point4In", 0 }, { "point4Out", -10 },
                                { "point5In", -20 }, { "point5Out", -20 },
                                { "point6In", -10 }, { "point6Out", -10 },
                                { "detector", 1 }, { "attack", 20 }, { "release", 300 }, { "makeUp", 4 } } } } },

        { EffectKind::Multiband, {
            { "Podcast Glue",  { { "lowCrossover", 180 }, { "highCrossover", 3000 },
                                 { "lowThreshold", -24 }, { "lowRatio", 3 },   { "lowMakeUp", 2 },
                                 { "midThreshold", -20 }, { "midRatio", 2.5 }, { "midMakeUp", 2 },
                                 { "highThreshold", -22 }, { "highRatio", 3 },  { "highMakeUp", 1 },
                                 { "attack", 15 }, { "release", 200 } } },
            { "Tame the Bass", { { "lowCrossover", 120 }, { "highCrossover", 5000 },
                                 { "lowThreshold", -28 }, { "lowRatio", 6 },   { "lowMakeUp", 3 },
                                 { "midThreshold", 0 },   { "midRatio", 1 },   { "midMakeUp", 0 },
                                 { "highThreshold", 0 },  { "highRatio", 1 },  { "highMakeUp", 0 },
                                 { "attack", 5 }, { "release", 120 } } },
            { "Master Polish", { { "lowCrossover", 250 }, { "highCrossover", 4000 },
                                 { "lowThreshold", -16 }, { "lowRatio", 2 },   { "lowMakeUp", 1 },
                                 { "midThreshold", -14 }, { "midRatio", 1.8 }, { "midMakeUp", 1 },
                                 { "highThreshold", -16 }, { "highRatio", 2 },  { "highMakeUp", 1 },
                                 { "attack", 30 }, { "release", 250 } } } } },

        { EffectKind::Echo, {
            { "Slapback",   { { "time", 110 }, { "taps", 1 }, { "decay", 0.5 },  { "mix", 0.3 }, { "pingPong", 0 } } },
            { "Tape Echo",  { { "time", 320 }, { "taps", 4 }, { "decay", 0.55 }, { "mix", 0.35 }, { "pingPong", 0 } } },
            { "Ping-pong",  { { "time", 260 }, { "taps", 6 }, { "decay", 0.6 },  { "mix", 0.4 }, { "pingPong", 1 } } } } },

        { EffectKind::ParametricEq, {
            { "Flat", { { "band1Type", 0 }, { "band1Hz", 30 }, { "band1Gain", 0 }, { "band1Q", 0.71 },
                             { "band2Type", 2 }, { "band2Hz", 100 }, { "band2Gain", 0 }, { "band2Q", 0.71 },
                             { "band3Type", 1 }, { "band3Hz", 400 }, { "band3Gain", 0 }, { "band3Q", 1.0 },
                             { "band4Type", 1 }, { "band4Hz", 1500 }, { "band4Gain", 0 }, { "band4Q", 1.0 },
                             { "band5Type", 1 }, { "band5Hz", 5000 }, { "band5Gain", 0 }, { "band5Q", 1.0 },
                             { "band6Type", 3 }, { "band6Hz", 10000 }, { "band6Gain", 0 }, { "band6Q", 0.71 } } },
            { "Remove Rumble", { { "band1Type", 5 }, { "band1Hz", 80 }, { "band1Gain", 0 }, { "band1Q", 0.71 },
                             { "band2Type", 0 }, { "band2Hz", 100 }, { "band2Gain", 0 }, { "band2Q", 0.71 },
                             { "band3Type", 0 }, { "band3Hz", 400 }, { "band3Gain", 0 }, { "band3Q", 1.0 },
                             { "band4Type", 0 }, { "band4Hz", 1500 }, { "band4Gain", 0 }, { "band4Q", 1.0 },
                             { "band5Type", 0 }, { "band5Hz", 5000 }, { "band5Gain", 0 }, { "band5Q", 1.0 },
                             { "band6Type", 0 }, { "band6Hz", 10000 }, { "band6Gain", 0 }, { "band6Q", 0.71 } } },
            { "Vocal Presence", { { "band1Type", 5 }, { "band1Hz", 90 }, { "band1Gain", 0 }, { "band1Q", 0.71 },
                             { "band2Type", 0 }, { "band2Hz", 100 }, { "band2Gain", 0 }, { "band2Q", 0.71 },
                             { "band3Type", 1 }, { "band3Hz", 300 }, { "band3Gain", -3 }, { "band3Q", 1.2 },
                             { "band4Type", 0 }, { "band4Hz", 1500 }, { "band4Gain", 0 }, { "band4Q", 1.0 },
                             { "band5Type", 1 }, { "band5Hz", 3500 }, { "band5Gain", 3 }, { "band5Q", 0.9 },
                             { "band6Type", 3 }, { "band6Hz", 10000 }, { "band6Gain", 2 }, { "band6Q", 0.71 } } },
            { "Hum Notch 60 Hz", { { "band1Type", 4 }, { "band1Hz", 60 }, { "band1Gain", 0 }, { "band1Q", 20 },
                             { "band2Type", 4 }, { "band2Hz", 120 }, { "band2Gain", 0 }, { "band2Q", 20 },
                             { "band3Type", 4 }, { "band3Hz", 180 }, { "band3Gain", 0 }, { "band3Q", 20 },
                             { "band4Type", 4 }, { "band4Hz", 240 }, { "band4Gain", 0 }, { "band4Q", 20 },
                             { "band5Type", 0 }, { "band5Hz", 5000 }, { "band5Gain", 0 }, { "band5Q", 1.0 },
                             { "band6Type", 0 }, { "band6Hz", 10000 }, { "band6Gain", 0 }, { "band6Q", 0.71 } } },
            { "Telephone", { { "band1Type", 5 }, { "band1Hz", 400 }, { "band1Gain", 0 }, { "band1Q", 0.9 },
                             { "band2Type", 0 }, { "band2Hz", 100 }, { "band2Gain", 0 }, { "band2Q", 0.71 },
                             { "band3Type", 0 }, { "band3Hz", 400 }, { "band3Gain", 0 }, { "band3Q", 1.0 },
                             { "band4Type", 1 }, { "band4Hz", 1500 }, { "band4Gain", 4 }, { "band4Q", 1.0 },
                             { "band5Type", 0 }, { "band5Hz", 5000 }, { "band5Gain", 0 }, { "band5Q", 1.0 },
                             { "band6Type", 6 }, { "band6Hz", 3400 }, { "band6Gain", 0 }, { "band6Q", 0.9 } } } } },

        { EffectKind::ChannelMixer, {
            { "Unchanged", { { "midSide", 0 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 0 }, { "rightToRight", 1 } } },
            { "Swap Sides", { { "midSide", 0 }, { "leftToLeft", 0 }, { "rightToLeft", 1 }, { "leftToRight", 1 }, { "rightToRight", 0 } } },
            { "Mono", { { "midSide", 0 }, { "leftToLeft", 0.5 }, { "rightToLeft", 0.5 }, { "leftToRight", 0.5 }, { "rightToRight", 0.5 } } },
            { "Left Only", { { "midSide", 0 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 1 }, { "rightToRight", 0 } } },
            { "Right Only", { { "midSide", 0 }, { "leftToLeft", 0 }, { "rightToLeft", 1 }, { "leftToRight", 0 }, { "rightToRight", 1 } } },
            { "Invert Right", { { "midSide", 0 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 0 }, { "rightToRight", -1 } } },
            { "Wider (Side +50%)", { { "midSide", 3 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 0 }, { "rightToRight", 1.5 } } },
            { "Narrower (Side -50%)", { { "midSide", 3 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 0 }, { "rightToRight", 0.5 } } },
            { "Encode to Mid/Side", { { "midSide", 1 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 0 }, { "rightToRight", 1 } } },
            { "Decode from Mid/Side", { { "midSide", 2 }, { "leftToLeft", 1 }, { "rightToLeft", 0 }, { "leftToRight", 0 }, { "rightToRight", 1 } } } } },

        { EffectKind::Vocoder, {
            { "Robot",      { { "carrier", 1 }, { "pitch", 110 }, { "bands", 16 }, { "response", 30 }, { "mix", 1 }, { "gain", 0 } } },
            { "Deep Robot", { { "carrier", 1 }, { "pitch", 55 },  { "bands", 24 }, { "response", 40 }, { "mix", 1 }, { "gain", 0 } } },
            { "Whisper",    { { "carrier", 2 }, { "pitch", 110 }, { "bands", 24 }, { "response", 20 }, { "mix", 1 }, { "gain", 0 } } },
            { "Talk Box",   { { "carrier", 0 }, { "pitch", 110 }, { "bands", 16 }, { "response", 25 }, { "mix", 1 }, { "gain", 0 } } } } },

        { EffectKind::Convolution, {
            { "Subtle Space", { { "mix", 0.15 }, { "preDelay", 0 },  { "gain", 0 } } },
            { "Big Hall",     { { "mix", 0.4 },  { "preDelay", 25 }, { "gain", 0 } } },
            { "All Wet",      { { "mix", 1.0 },  { "preDelay", 0 },  { "gain", 0 } } } } },

        { EffectKind::GraphicEq31, {
            { "Flat", { { "band1", 0 }, { "band2", 0 }, { "band3", 0 }, { "band4", 0 }, { "band5", 0 }, { "band6", 0 }, { "band7", 0 }, { "band8", 0 }, { "band9", 0 }, { "band10", 0 }, { "band11", 0 }, { "band12", 0 }, { "band13", 0 }, { "band14", 0 }, { "band15", 0 }, { "band16", 0 }, { "band17", 0 }, { "band18", 0 }, { "band19", 0 }, { "band20", 0 }, { "band21", 0 }, { "band22", 0 }, { "band23", 0 }, { "band24", 0 }, { "band25", 0 }, { "band26", 0 }, { "band27", 0 }, { "band28", 0 }, { "band29", 0 }, { "band30", 0 }, { "band31", 0 } } },
            { "Bass Boost", { { "band1", 6 }, { "band2", 6 }, { "band3", 6 }, { "band4", 6 }, { "band5", 5.5 }, { "band6", 5.5 }, { "band7", 4.5 }, { "band8", 4 }, { "band9", 3 }, { "band10", 2 }, { "band11", 1 }, { "band12", 0.5 }, { "band13", 0.5 }, { "band14", 0 }, { "band15", 0 }, { "band16", 0 }, { "band17", 0 }, { "band18", 0 }, { "band19", 0 }, { "band20", 0 }, { "band21", 0 }, { "band22", 0 }, { "band23", 0 }, { "band24", 0 }, { "band25", 0 }, { "band26", 0 }, { "band27", 0 }, { "band28", 0 }, { "band29", 0 }, { "band30", 0 }, { "band31", 0 } } },
            { "Treble Boost", { { "band1", 0 }, { "band2", 0 }, { "band3", 0 }, { "band4", 0 }, { "band5", 0 }, { "band6", 0 }, { "band7", 0 }, { "band8", 0 }, { "band9", 0 }, { "band10", 0 }, { "band11", 0 }, { "band12", 0 }, { "band13", 0 }, { "band14", 0 }, { "band15", 0 }, { "band16", 0 }, { "band17", 0 }, { "band18", 0 }, { "band19", 0 }, { "band20", 0.5 }, { "band21", 0.5 }, { "band22", 1 }, { "band23", 2 }, { "band24", 3 }, { "band25", 4 }, { "band26", 4.5 }, { "band27", 5.5 }, { "band28", 5.5 }, { "band29", 6 }, { "band30", 6 }, { "band31", 6 } } },
            { "Loudness Smile", { { "band1", 12 }, { "band2", 11.5 }, { "band3", 10 }, { "band4", 8.5 }, { "band5", 7.5 }, { "band6", 6 }, { "band7", 5 }, { "band8", 4 }, { "band9", 3 }, { "band10", 2 }, { "band11", 1.5 }, { "band12", 1 }, { "band13", 0 }, { "band14", 0 }, { "band15", -0.5 }, { "band16", -1 }, { "band17", -1 }, { "band18", -1 }, { "band19", -1 }, { "band20", -1 }, { "band21", -0.5 }, { "band22", 0 }, { "band23", 0 }, { "band24", 1 }, { "band25", 1.5 }, { "band26", 2 }, { "band27", 3 }, { "band28", 4 }, { "band29", 5 }, { "band30", 6 }, { "band31", 7.5 } } },
            { "Radio", { { "band1", -12 }, { "band2", -12 }, { "band3", -12 }, { "band4", -12 }, { "band5", -12 }, { "band6", -12 }, { "band7", -12 }, { "band8", -12 }, { "band9", 0 }, { "band10", 0 }, { "band11", 0 }, { "band12", 0 }, { "band13", 0 }, { "band14", 0 }, { "band15", 0 }, { "band16", 0 }, { "band17", 3 }, { "band18", 3 }, { "band19", 3 }, { "band20", 3 }, { "band21", 0 }, { "band22", 0 }, { "band23", 0 }, { "band24", 0 }, { "band25", -12 }, { "band26", -12 }, { "band27", -12 }, { "band28", -12 }, { "band29", -12 }, { "band30", -12 }, { "band31", -12 } } } } },

        { EffectKind::GraphicEq, {
            { "Bass Boost",      { { "band31", 6 }, { "band62", 5 }, { "band125", 3 }, { "band250", 1 }, { "band500", 0 }, { "band1k", 0 }, { "band2k", 0 }, { "band4k", 0 }, { "band8k", 0 }, { "band16k", 0 } } },
            { "Treble Boost",    { { "band31", 0 }, { "band62", 0 }, { "band125", 0 }, { "band250", 0 }, { "band500", 0 }, { "band1k", 0 }, { "band2k", 1 }, { "band4k", 3 }, { "band8k", 5 }, { "band16k", 6 } } },
            { "Loudness Smile",  { { "band31", 5 }, { "band62", 4 }, { "band125", 2 }, { "band250", 0 }, { "band500", -1 }, { "band1k", -1 }, { "band2k", 0 }, { "band4k", 2 }, { "band8k", 4 }, { "band16k", 5 } } },
            { "Voice Presence",  { { "band31", -6 }, { "band62", -4 }, { "band125", -2 }, { "band250", 0 }, { "band500", 0 }, { "band1k", 1 }, { "band2k", 3 }, { "band4k", 3 }, { "band8k", 1 }, { "band16k", 0 } } } } },

        { EffectKind::DeEsser, {
            { "Gentle", { { "frequency", 6000 }, { "threshold", -24 }, { "reduction", 6 } } },
            { "Female Voice", { { "frequency", 7000 }, { "threshold", -30 }, { "reduction", 10 } } },
            { "Male Voice", { { "frequency", 5000 }, { "threshold", -30 }, { "reduction", 10 } } } } },

        { EffectKind::Expander, {
            { "Background Noise", { { "threshold", -50 }, { "ratio", 2 }, { "range", 20 }, { "attack", 2 }, { "release", 150 } } },
            { "Tighten Drums",    { { "threshold", -30 }, { "ratio", 4 }, { "range", 40 }, { "attack", 1 }, { "release", 60 } } } } },

        { EffectKind::RingMod, {
            { "Robot",  { { "frequency", 50 },  { "mix", 1 } } },
            { "Bell",   { { "frequency", 880 }, { "mix", 0.6 } } } } },

        { EffectKind::Wah, {
            { "Funky",     { { "rate", 2 },   { "depth", 0.9 }, { "resonance", 6 }, { "mix", 1 } } },
            { "Slow Sweep", { { "rate", 0.3 }, { "depth", 1 },   { "resonance", 3 }, { "mix", 0.8 } } } } },

        { EffectKind::Invert, {
            { "Both Channels", { { "left", 1 }, { "right", 1 } } },
            { "Left Only",     { { "left", 1 }, { "right", 0 } } },
            { "Right Only",    { { "left", 0 }, { "right", 1 } } } } },

        { EffectKind::DcOffset, {
            { "Gentle (2 Hz)",   { { "cutoff", 2 } } },
            { "Standard (5 Hz)", { { "cutoff", 5 } } },
            { "Firm (15 Hz)",    { { "cutoff", 15 } } } } },

        { EffectKind::Amplify, {
            { "+6 dB", { { "gain", 6 } } },
            { "-6 dB", { { "gain", -6 } } } } },

        { EffectKind::Eq, {
            { "Voice Clarity", { { "lowFreq", 100 }, { "low", -4 }, { "midFreq", 3000 }, { "mid", 3 },
                                 { "midQ", 1 }, { "highFreq", 8000 }, { "high", 2 } } },
            { "Mid Scoop",     { { "lowFreq", 120 }, { "low", 3 },  { "midFreq", 700 },  { "mid", -6 },
                                 { "midQ", 0.8 }, { "highFreq", 5000 }, { "high", 3 } } },
            { "Warmth",        { { "lowFreq", 150 }, { "low", 3 },  { "midFreq", 3000 }, { "mid", -2 },
                                 { "midQ", 1 }, { "highFreq", 9000 }, { "high", -3 } } } } },
    };

    for (const auto& [presetKind, presets] : table)
        if (presetKind == kind)
            return presets;

    static const std::vector<EffectPreset> none;
    return none;
}

/** @p presets with @p preset saved for @p effectId, replacing one of the same
    name for that effect if there is one. */
inline std::vector<UserEffectPreset> withUserPreset(std::vector<UserEffectPreset> presets,
                                                    const std::string& effectId, EffectPreset preset)
{
    for (auto& user : presets)
    {
        if (user.effectId == effectId && user.preset.name == preset.name)
        {
            user.preset = std::move(preset);
            return presets;
        }
    }

    presets.push_back({ effectId, std::move(preset) });
    return presets;
}

/** @p presets without @p effectId's preset called @p name. */
inline std::vector<UserEffectPreset> withoutUserPreset(std::vector<UserEffectPreset> presets,
                                                       const std::string& effectId, const std::string& name)
{
    presets.erase(std::remove_if(presets.begin(), presets.end(),
                                 [&](const UserEffectPreset& user)
                                 { return user.effectId == effectId && user.preset.name == name; }),
                  presets.end());
    return presets;
}

/** User presets as text: a PRESET line naming the effect and the preset,
    then a VALUE line per parameter. Line-based for the same reason the
    project format is, and so a hand-edited or truncated file still loads
    everything before the damage. */
inline std::string serializeUserPresets(const std::vector<UserEffectPreset>& presets)
{
    std::ostringstream out;

    for (const auto& user : presets)
    {
        // The name is the rest of its line, so it may hold spaces but not a
        // line break.
        auto name = user.preset.name;
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::replace(name.begin(), name.end(), '\r', ' ');

        out << "PRESET " << user.effectId << " " << name << "\n";

        for (const auto& [id, value] : user.preset.values)
        {
            char number[64];
            std::snprintf(number, sizeof(number), "%.17g", value);
            out << "VALUE " << id << " " << number << "\n";
        }
    }

    return out.str();
}

/** The inverse of serializeUserPresets. Lines it can't make sense of are
    skipped, as are the values of a PRESET line missing its effect or name:
    losing one damaged preset is better than losing the library. */
inline std::vector<UserEffectPreset> deserializeUserPresets(const std::string& text)
{
    std::vector<UserEffectPreset> presets;
    bool                          collecting = false; // whether VALUE lines belong to presets.back()

    std::istringstream in(text);
    std::string        line;

    while (std::getline(in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();

        std::istringstream fields(line);
        std::string        tag;
        fields >> tag;

        if (tag == "PRESET")
        {
            UserEffectPreset user;
            fields >> user.effectId;
            std::getline(fields, user.preset.name);

            const auto first = user.preset.name.find_first_not_of(' ');
            user.preset.name = first == std::string::npos ? std::string() : user.preset.name.substr(first);

            collecting = ! user.effectId.empty() && ! user.preset.name.empty();
            if (collecting)
                presets.push_back(std::move(user));
        }
        else if (tag == "VALUE" && collecting)
        {
            std::string id;
            double      value = 0.0;
            if (fields >> id >> value)
                presets.back().preset.values.emplace_back(id, value);
        }
    }

    return presets;
}

} // namespace soundsplice::model
