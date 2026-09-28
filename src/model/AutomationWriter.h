#pragma once

#include <algorithm>
#include <vector>

#include "model/AutomationLane.h"

namespace soundsplice::model
{
/** How moving a control during playback records automation.

    Read   - it doesn't: the lanes play, and a control sets the value used
             where there is no lane.
    Touch  - while a control is held, its value replaces the lane under the
             playhead; let go and the lane takes over again.
    Latch  - as Touch, but after letting go the last value keeps being
             written until playback stops.
    Write  - as Latch, and every track's volume and pan is written from its
             fader for the whole pass, touched or not. */
enum class AutomationMode
{
    Read  = 0,
    Touch = 1,
    Latch = 2,
    Write = 3
};

/**
    Records one control into its lane as the playhead moves: whatever the
    lane had between where writing began and where it is now is replaced by
    what the control did.

    Driven from the UI's timer rather than by every value change, so a held
    control still writes (a latched value has to hold across the stretch it
    covers, not just leave a point where it was let go). A value is only put
    down when it changes; a held stretch is closed with a point at each end,
    so it stays flat instead of ramping into the next move.

    JUCE-free and unit-tested: this is where recording can quietly corrupt a
    lane, and the lane is the whole of what the user made.
*/
class LaneWriter
{
public:
    bool active() const noexcept { return active_; }

    /** Starts writing at @p beat with the control at @p value. The lane's
        shape up to here is kept: a point at @p beat carries its value, so a
        move from it starts where the curve was. */
    void begin(AutomationLane& lane, double beat, float value)
    {
        const float before = lane.valueAt(beat, value);
        eraseAfter(lane, beat - kEpsilon, beat);
        lane.addPoint(beat, before);

        active_        = true;
        lastBeat_      = beat;
        lastPointBeat_ = beat;
        lastValue_     = before;

        // A control that isn't where the curve is jumps there at once.
        if (value != before)
        {
            lastBeat_ = lastPointBeat_ = beat + kEpsilon;
            lastValue_                 = value;
            lane.addPoint(lastBeat_, value);
        }
    }

    /** The playhead has reached @p beat with the control at @p value. A jump
        backwards (a loop, or the user moving the playhead) closes the
        stretch written so far and starts a new one where it landed. */
    void advance(AutomationLane& lane, double beat, float value)
    {
        if (! active_)
            return;

        if (beat < lastBeat_)
        {
            end(lane, lastBeat_);
            begin(lane, beat, value);
            return;
        }

        eraseAfter(lane, lastBeat_, beat);

        if (value != lastValue_)
        {
            // Close the held stretch before this move, or it would ramp.
            if (lastPointBeat_ < lastBeat_)
                lane.addPoint(lastBeat_, lastValue_);
            lane.addPoint(beat, value);
            lastPointBeat_ = beat;
            lastValue_     = value;
        }
        lastBeat_ = beat;
    }

    /** Stops writing at @p beat, closing the written stretch there so the
        lane beyond it resumes from its own points. */
    void end(AutomationLane& lane, double beat)
    {
        if (! active_)
            return;

        beat = std::max(beat, lastBeat_);
        eraseAfter(lane, lastBeat_, beat);
        lane.addPoint(beat, lastValue_);
        active_ = false;
    }

private:
    static constexpr double kEpsilon = 1.0e-6;

    /** Removes the points in (@p from, @p to]. */
    static void eraseAfter(AutomationLane& lane, double from, double to)
    {
        for (int i = (int) lane.points().size() - 1; i >= 0; --i)
        {
            const double b = lane.points()[(size_t) i].beat;
            if (b > from && b <= to)
                lane.removePointAt(i);
        }
    }

    bool   active_        = false;
    double lastBeat_      = 0.0;
    float  lastValue_     = 0.0f;
    double lastPointBeat_ = 0.0;
};

} // namespace soundsplice::model
