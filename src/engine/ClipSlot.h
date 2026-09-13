#pragma once

#include "engine/Pattern.h"

namespace looper::engine
{
/**
    One scheduled clip within a track: a pattern that plays only within
    [startBeats, startBeats + lengthBeats) of the timeline, looping on the
    pattern's own length inside that window. A Sequencer is given a track's
    whole list of ClipSlots and picks whichever one's window covers the current
    transport position (they're expected not to overlap).
*/
struct ClipSlot
{
    Pattern pattern;
    double  startBeats  = 0.0;
    double  lengthBeats = 4.0;
};

} // namespace looper::engine
