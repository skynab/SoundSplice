#pragma once

#include <algorithm>

namespace looper
{
/**
    Where a horizontal viewport should scroll to keep a moving position in
    view.

    Paging rather than centring. Centring the playhead means the content
    slides continuously under a stationary line, which is exhausting to read
    and makes it impossible to judge where a note sits relative to the bar
    lines — they never stop moving. Paging leaves the view still while the
    playhead crosses it, then jumps once when it reaches the edge, which is
    what most sequencers do and what a reader can actually follow.

    Returns the offset unchanged when the position is already comfortably
    inside the view, so a caller can compare and skip the scroll entirely.

    JUCE-free so the paging arithmetic — which is entirely about edges and
    off-by-ones — is testable without a window.
*/
inline int scrollToFollow(int positionX, int currentOffset, int viewportWidth,
                          int contentWidth, int margin)
{
    // Nothing to scroll: the content fits, so the only valid offset is 0.
    if (viewportWidth <= 0 || contentWidth <= viewportWidth)
        return 0;

    const int maxOffset = contentWidth - viewportWidth;

    // A margin at or past half the view would make the "comfortable" band
    // empty or inverted, and every frame would trigger a scroll.
    const int safeMargin = std::clamp(margin, 0, std::max(0, viewportWidth / 2 - 1));

    const int leftEdge  = currentOffset + safeMargin;
    const int rightEdge = currentOffset + viewportWidth - safeMargin;

    if (positionX >= leftEdge && positionX < rightEdge)
        return currentOffset; // already in view, leave it alone

    // Off either edge, the position lands just inside the left of the new
    // view — so after a jump there is a full page of travel before the next
    // one, rather than another jump a few pixels later.
    return std::clamp(positionX - safeMargin, 0, maxOffset);
}

} // namespace looper
