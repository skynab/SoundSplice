#pragma once

#include <algorithm>

#include "engine/DynamicsProcessor.h"
#include "model/Effects.h"

namespace soundsplice::model
{
/** The dynamics processor's settings, stored as flat fields (a parameter
    each), as the engine's transfer curve, and back. */
inline engine::TransferCurve transferCurve(const DynamicsSettings& d)
{
    engine::TransferCurve curve;
    curve.count = std::clamp(d.points, 2, engine::TransferCurve::kMaxPoints);
    curve.points[0] = { d.point1InDb, d.point1OutDb };
    curve.points[1] = { d.point2InDb, d.point2OutDb };
    curve.points[2] = { d.point3InDb, d.point3OutDb };
    curve.points[3] = { d.point4InDb, d.point4OutDb };
    curve.points[4] = { d.point5InDb, d.point5OutDb };
    curve.points[5] = { d.point6InDb, d.point6OutDb };
    return curve;
}

inline void setTransferCurve(DynamicsSettings& d, const engine::TransferCurve& curve)
{
    d.points = std::clamp(curve.count, 2, engine::TransferCurve::kMaxPoints);
    d.point1InDb = curve.points[0].inDb; d.point1OutDb = curve.points[0].outDb;
    d.point2InDb = curve.points[1].inDb; d.point2OutDb = curve.points[1].outDb;
    d.point3InDb = curve.points[2].inDb; d.point3OutDb = curve.points[2].outDb;
    d.point4InDb = curve.points[3].inDb; d.point4OutDb = curve.points[3].outDb;
    d.point5InDb = curve.points[4].inDb; d.point5OutDb = curve.points[4].outDb;
    d.point6InDb = curve.points[5].inDb; d.point6OutDb = curve.points[5].outDb;
}

} // namespace soundsplice::model
