#pragma once

#include <algorithm>

namespace soundsplice::engine
{
/** How an automation segment travels from one point to the next. Belongs to
    the point the segment starts at. The values are saved in projects:
    append, never renumber. */
enum class CurveShape
{
    Linear    = 0,
    Hold      = 1, // stays at the first value, then steps at the next point
    FastStart = 2, // most of the change early, easing into the next point
    SlowStart = 3, // easing out of the first point, most of the change late
    SCurve    = 4  // easing out and in: a fade that starts and ends gently
};

/** A stored shape, or Linear for a value this build doesn't know, so a shape
    added later plays as a straight line here rather than refusing the file. */
inline CurveShape curveShapeFrom(int value)
{
    return value >= 0 && value <= (int) CurveShape::SCurve ? (CurveShape) value : CurveShape::Linear;
}

/** How far along a segment of @p shape the value is at fraction @p t of the
    way through it, both 0..1. The one definition shared by the document's
    lanes, the engine's curves and the automation pane's drawing, so what is
    drawn is what plays. */
inline double shapedFraction(CurveShape shape, double t) noexcept
{
    t = std::clamp(t, 0.0, 1.0);
    switch (shape)
    {
        case CurveShape::Linear:    return t;
        case CurveShape::Hold:      return t < 1.0 ? 0.0 : 1.0;
        case CurveShape::FastStart: { const double u = 1.0 - t; return 1.0 - u * u * u; }
        case CurveShape::SlowStart: return t * t * t;
        case CurveShape::SCurve:    return t * t * (3.0 - 2.0 * t);
    }
    return t;
}

} // namespace soundsplice::engine
