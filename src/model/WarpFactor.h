#pragma once

#include <utility>
#include <vector>

#include "engine/TempoDetect.h"
#include "engine/TempoMap.h"
#include "model/Clip.h"

namespace soundsplice::model
{
/** The arithmetic of warp (see model/Warp.h), apart from it so the tempo
    edits (model/TempoChanges.h), which restretch warped clips, can use it. */
namespace warpedit
{
    /** How many times as long @p clip plays as its file, under the tempo map
        @p map: 1 for a clip that isn't warped or whose tempo isn't known. */
    inline double factorUnder(const std::vector<engine::TempoChange>& map, const Clip& clip)
    {
        if (! clip.warp || clip.sourceBpm <= 0.0)
            return 1.0;

        engine::TempoMap tempo;
        tempo.setTempoChanges(map);
        return engine::warpStretchFactor(clip.sourceBpm, tempo.tempoAtBeat(clip.startBeats), true);
    }

    /** Scales everything @p clip measures in its stretched time - its offset,
        its volume curve, its takes' shifts - by @p ratio: a new stretch over
        the old. */
    inline void rescale(Clip& clip, double ratio)
    {
        if (! (ratio > 0.0) || ratio == 1.0)
            return;

        clip.sourceOffsetSeconds *= ratio;
        engine::ClipEnvelope envelope;
        for (const auto& point : clip.envelope.points())
            envelope.addPoint(point.seconds * ratio, point.gain);
        clip.envelope = std::move(envelope);
        for (auto& take : clip.takes)
            take.shiftSeconds *= ratio;
    }

}

} // namespace soundsplice::model
