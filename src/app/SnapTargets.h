#pragma once

#include <cmath>
#include <limits>
#include <vector>

namespace soundsplice::app
{
/**
    Where a dragged clip edge lands: pulled onto a nearby "magnet" (a marker,
    the playhead, another clip's edge) when one is close enough, onto the grid
    otherwise, or left exactly where it was dropped.

    Magnets win over the grid because they are specific: lining a clip up with
    the end of the one before it, or with a marker, is a deliberate target,
    where the grid is a general tidiness. The tolerance is given in beats but
    chosen from a distance in pixels, so how close counts as "close" stays the
    same on screen at any zoom.

    JUCE-free so the rules are tested headless.
*/

/** @p beat snapped: to the nearest of @p magnets within @p toleranceBeats,
    else to multiples of @p gridUnitBeats if @p grid is on, else unchanged. */
inline double snapPosition(double beat, const std::vector<double>& magnets, double toleranceBeats, bool grid,
                           double gridUnitBeats)
{
    double best         = beat;
    double bestDistance = std::numeric_limits<double>::max();

    for (double magnet : magnets)
    {
        const double distance = std::abs(magnet - beat);
        if (distance <= toleranceBeats && distance < bestDistance)
        {
            best         = magnet;
            bestDistance = distance;
        }
    }

    if (bestDistance <= toleranceBeats)
        return best;

    if (grid && gridUnitBeats > 0.0)
        return std::round(beat / gridUnitBeats) * gridUnitBeats;

    return beat;
}

/** Where a span @p lengthBeats long, being moved so it would start at
    @p startBeats, should start: whichever of its start or its end is nearer a
    magnet within @p toleranceBeats is put on it, so a clip can be butted up
    against something from either side. With no magnet close, its start goes
    onto the grid if @p grid is on. */
inline double snapSpanStart(double startBeats, double lengthBeats, const std::vector<double>& magnets,
                            double toleranceBeats, bool grid, double gridUnitBeats)
{
    const double endBeats     = startBeats + lengthBeats;
    double       snapped      = startBeats;
    double       bestDistance = std::numeric_limits<double>::max();

    for (double magnet : magnets)
    {
        const double fromStart = std::abs(magnet - startBeats);
        if (fromStart <= toleranceBeats && fromStart < bestDistance)
        {
            snapped      = magnet;
            bestDistance = fromStart;
        }

        const double fromEnd = std::abs(magnet - endBeats);
        if (fromEnd <= toleranceBeats && fromEnd < bestDistance)
        {
            snapped      = magnet - lengthBeats;
            bestDistance = fromEnd;
        }
    }

    if (bestDistance <= toleranceBeats)
        return snapped;

    if (grid && gridUnitBeats > 0.0)
        return std::round(startBeats / gridUnitBeats) * gridUnitBeats;

    return startBeats;
}

} // namespace soundsplice::app
