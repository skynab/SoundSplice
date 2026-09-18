#pragma once

#include <algorithm>

#include "engine/ParametricEq.h"
#include "model/Effects.h"

namespace soundsplice::model
{
/** The parametric EQ's settings, stored as flat fields (so each is a
    parameter with its own control, preset entry and automation id), as the
    engine's six bands, and back. */
inline engine::ParametricEq::Bands parametricBands(const ParametricEqSettings& eq)
{
    engine::ParametricEq::Bands bands;
    bands[0] = { (engine::ParametricBand::Type) std::clamp(eq.band1Type, 0, 6), eq.band1Hz, eq.band1GainDb, eq.band1Q };
    bands[1] = { (engine::ParametricBand::Type) std::clamp(eq.band2Type, 0, 6), eq.band2Hz, eq.band2GainDb, eq.band2Q };
    bands[2] = { (engine::ParametricBand::Type) std::clamp(eq.band3Type, 0, 6), eq.band3Hz, eq.band3GainDb, eq.band3Q };
    bands[3] = { (engine::ParametricBand::Type) std::clamp(eq.band4Type, 0, 6), eq.band4Hz, eq.band4GainDb, eq.band4Q };
    bands[4] = { (engine::ParametricBand::Type) std::clamp(eq.band5Type, 0, 6), eq.band5Hz, eq.band5GainDb, eq.band5Q };
    bands[5] = { (engine::ParametricBand::Type) std::clamp(eq.band6Type, 0, 6), eq.band6Hz, eq.band6GainDb, eq.band6Q };
    return bands;
}

inline void setParametricBands(ParametricEqSettings& eq, const engine::ParametricEq::Bands& bands)
{
    eq.band1Type = (int) bands[0].type; eq.band1Hz = bands[0].hz; eq.band1GainDb = bands[0].gainDb; eq.band1Q = bands[0].q;
    eq.band2Type = (int) bands[1].type; eq.band2Hz = bands[1].hz; eq.band2GainDb = bands[1].gainDb; eq.band2Q = bands[1].q;
    eq.band3Type = (int) bands[2].type; eq.band3Hz = bands[2].hz; eq.band3GainDb = bands[2].gainDb; eq.band3Q = bands[2].q;
    eq.band4Type = (int) bands[3].type; eq.band4Hz = bands[3].hz; eq.band4GainDb = bands[3].gainDb; eq.band4Q = bands[3].q;
    eq.band5Type = (int) bands[4].type; eq.band5Hz = bands[4].hz; eq.band5GainDb = bands[4].gainDb; eq.band5Q = bands[4].q;
    eq.band6Type = (int) bands[5].type; eq.band6Hz = bands[5].hz; eq.band6GainDb = bands[5].gainDb; eq.band6Q = bands[5].q;
}

} // namespace soundsplice::model
