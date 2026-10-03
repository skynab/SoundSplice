#pragma once

#include "engine/ShelfPeakFilter.h"
#include "model/Effects.h"

namespace soundsplice::engine
{
/** The mastering rack's EQ response at @p hz, in dB.

    Evaluated analytically rather than by processing audio, and built from
    the same ShelfPeakFilter the rack itself uses - so the curve on screen
    can't drift from the filters in the signal path. The three bands are in
    series, so their dB gains simply add.

    @p sampleRate only shapes exactly how the curve bends very close to
    Nyquist; a fixed representative rate is fine for a visual guide. */
inline float masteringEqMagnitudeDb(const model::MasteringSettings& m, float hz,
                                    double sampleRate = 48000.0)
{
    ShelfPeakFilter low;
    low.prepare(sampleRate);
    low.setShape(ShelfPeakFilter::Shape::LowShelf);
    low.setFrequency(m.lowShelfHz);
    low.setGainDb(m.lowShelfDb);

    ShelfPeakFilter peak;
    peak.prepare(sampleRate);
    peak.setShape(ShelfPeakFilter::Shape::Peaking);
    peak.setFrequency(m.peakHz);
    peak.setQ(m.peakQ);
    peak.setGainDb(m.peakDb);

    ShelfPeakFilter high;
    high.prepare(sampleRate);
    high.setShape(ShelfPeakFilter::Shape::HighShelf);
    high.setFrequency(m.highShelfHz);
    high.setGainDb(m.highShelfDb);

    return low.magnitudeDbAt(hz) + peak.magnitudeDbAt(hz) + high.magnitudeDbAt(hz);
}

} // namespace soundsplice::engine
