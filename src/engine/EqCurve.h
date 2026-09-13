#pragma once

#include "engine/ShelfPeakFilter.h"
#include "model/Effects.h"

namespace looper::engine
{
/**
    The master EQ's combined frequency response at @p hz, in dB — the same
    three bands EqEffect runs (bass low shelf, mid peak, treble high shelf),
    evaluated analytically rather than by processing audio. Exists so a UI
    can draw the curve a settings struct implies without an EqEffect, an
    AudioEngine, or any audio actually running.

    The three bands are in series, so their dB gains simply add — this is
    the same shape EqEffect::process() runs, just summed via
    ShelfPeakFilter::magnitudeDbAt() instead of processSample().

    @p sampleRate only shapes exactly how the curve bends very close to
    Nyquist; a fixed representative rate is fine for a visual guide that
    isn't tied to whatever project happens to be open.
*/
inline float eqMagnitudeDb(const model::EqSettings& eq, float hz, double sampleRate = 48000.0)
{
    ShelfPeakFilter bass;
    bass.prepare(sampleRate);
    bass.setShape(ShelfPeakFilter::Shape::LowShelf);
    bass.setFrequency(model::EqSettings::bassHz);
    bass.setGainDb(eq.bassDb);

    ShelfPeakFilter mid;
    mid.prepare(sampleRate);
    mid.setShape(ShelfPeakFilter::Shape::Peaking);
    mid.setFrequency(model::EqSettings::midHz());
    mid.setQ(0.7f);
    mid.setGainDb(eq.midDb);

    ShelfPeakFilter treble;
    treble.prepare(sampleRate);
    treble.setShape(ShelfPeakFilter::Shape::HighShelf);
    treble.setFrequency(model::EqSettings::trebleHz);
    treble.setGainDb(eq.trebleDb);

    return bass.magnitudeDbAt(hz) + mid.magnitudeDbAt(hz) + treble.magnitudeDbAt(hz);
}

/** The mastering rack's EQ response at @p hz, in dB.

    Same three-filters-summed approach as eqMagnitudeDb above, and built from
    the same ShelfPeakFilter the rack itself uses — so the curve on screen
    can't drift from the filters in the signal path. Unlike the master EQ,
    every frequency here is adjustable, which is why they come from the
    settings rather than from fixed constants. */
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

} // namespace looper::engine
