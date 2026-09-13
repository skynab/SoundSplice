#pragma once

#include <string>
#include <vector>

#include "model/Effects.h"
#include "model/SynthSettings.h"

namespace looper::model
{
/**
    A named, saveable bundle of a synth's timbre and its effect chain —
    "synths and distortion mixes" as one loadable thing, rather than two
    separate settings a user has to recreate by hand every time.

    Deliberately bundles the whole effect chain (built-ins and hosted
    plugins alike) rather than just the built-in drive pedal: the point of
    a preset is "the sound," and a distortion plugin sitting in the chain is
    as much a part of that sound as the oscillator waveform is. A preset's
    effectChain entries carry PluginRef the same way EffectSlot does, so a
    preset can reference a hosted plugin exactly like a project can.
*/
struct SynthPreset
{
    std::string              name;
    SynthSettings            synth;
    std::vector<EffectSlot>  effectChain;

    bool operator==(const SynthPreset&) const = default;
};

} // namespace looper::model
