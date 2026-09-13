#pragma once

#include <cstdint>

namespace looper::engine
{
/** An immutable snapshot of the transport at the start of an audio block. */
struct TransportSnapshot
{
    bool    playing            = false;
    int64_t playheadSamples    = 0;
    double  bpm                = 120.0;
    int     timeSigNumerator   = 4;
    int     timeSigDenominator = 4;
    double  ppqPosition        = 0.0; // quarter-note position at block start
    double  ppqAtBlockEnd      = 0.0; // and at block end

    /** How much musical time this block covers. Not a constant once tempo can
        change, which is exactly why it is carried rather than derived from
        bpm - see engine::TempoMap. */
    double blockLengthBeats() const noexcept { return ppqAtBlockEnd - ppqPosition; }

    /** Sample offset within this block for a musical position.

        Tempo is treated as constant across one block: the map is evaluated at
        the block's edges and everything inside interpolates. A tempo change
        therefore lands on a block boundary - about 10ms at 512 samples, well
        under anything audible - and in exchange no inner loop has to evaluate
        a map. */
    double sampleOffsetForPpq(double ppq, int numSamples) const noexcept
    {
        const double span = blockLengthBeats();
        if (span <= 0.0 || numSamples <= 0)
            return 0.0;

        return (ppq - ppqPosition) / span * (double) numSamples;
    }
};

/** Everything a Node needs to render one block. Passed by const ref; POD, no JUCE. */
struct ProcessContext
{
    double            sampleRate = 0.0;
    int               numSamples = 0;
    TransportSnapshot transport;
};

} // namespace looper::engine
