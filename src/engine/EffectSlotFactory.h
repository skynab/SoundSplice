#pragma once

#include <memory>

#include "engine/EffectChain.h"
#include "model/EffectParams.h"

namespace soundsplice::engine
{
/**
    The engine node for a document effect slot, already configured - which is
    what every caller actually wants. Returns null for a Plugin slot, which
    needs a host.

    Shared rather than duplicated because it had already been duplicated three
    times - MainComponent, AudioEngine's rebuild switch, and the bounce tool -
    and the bounce tool's copy drifted. A verification tool that builds the
    chain differently from the app verifies the wrong thing, and it does so
    silently: everything still runs, the numbers just describe a signal path
    nobody hears.

    Free rather than a member of EffectChain because it maps a *model* slot,
    and EffectChain is deliberately reachable from code that has no model.
*/
inline std::unique_ptr<EffectProcessor> makeConfiguredNode(const model::EffectSlot& slot)
{
    auto node = makeBuiltInNode(slot.kind);
    if (node != nullptr)
        node->applyParams(model::effectParamValues(slot));
    return node;
}

} // namespace soundsplice::engine
