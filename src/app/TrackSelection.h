#pragma once

#include <algorithm>

namespace looper
{
/**
    Where the selection lands after a track is removed.

    Deleting from the gear menu removes the track whose gear was clicked,
    which is not necessarily the selected one — so the selection has to be
    fixed up rather than just clamped. Removing a track above the selected one
    shifts it down by one; removing the selected one leaves the selection on
    whatever slid into its place; removing one below it changes nothing.

    Getting this wrong doesn't crash. It quietly selects a different track
    than the one that was highlighted a moment ago, and the next edit goes
    somewhere the user didn't mean — which is exactly the kind of fault that
    is noticed several actions later, if at all.

    @p tracksRemaining is the count *after* the removal. Zero means nothing is
    left to select, reported as -1.
*/
inline int selectionAfterTrackRemoved(int selectedIndex, int removedIndex, int tracksRemaining)
{
    if (tracksRemaining <= 0)
        return -1;

    int selected = selectedIndex;

    if (removedIndex < selectedIndex)
        --selected; // everything below the removal slid up by one

    // Removing the selected track itself leaves the index pointing at whatever
    // took its place, which is the track that was below it — or the new last
    // track, if it was the bottom one.
    return std::clamp(selected, 0, tracksRemaining - 1);
}

} // namespace looper
