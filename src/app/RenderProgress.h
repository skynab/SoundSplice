#pragma once

#include <algorithm>

namespace looper::app
{
/**
    Progress across a run of renders.

    An export can be one render or nine (a master mix plus eight stems), and a
    bar that restarted at zero for each of them would be actively misleading -
    it would say "nearly done" eight times. This maps a fraction *within* one
    task onto the fraction of the whole set.

    Free functions rather than a class because there is no state worth holding:
    the caller already knows which task it is on. Separated out and tested
    because this is where the bugs in a progress bar actually live - the
    threading either works or it obviously doesn't, but an off-by-one here
    gives a bar that reaches 100% early, never arrives, or goes backwards.
*/

/** Overall progress in 0..1 for @p taskIndex of @p taskCount, where the
    current task is @p fractionWithinTask complete. */
inline double overallProgress(int taskIndex, int taskCount, double fractionWithinTask)
{
    if (taskCount <= 0)
        return 1.0;

    const double index    = (double) std::clamp(taskIndex, 0, taskCount - 1);
    const double fraction = std::clamp(fractionWithinTask, 0.0, 1.0);

    return std::clamp((index + fraction) / (double) taskCount, 0.0, 1.0);
}

} // namespace looper::app
