#pragma once

#include <algorithm>

namespace looper
{
/**
    Pure geometry for the audio editor: converts between a position in an
    audio file (in seconds) and a pixel x-coordinate, at a given zoom and
    scroll offset.

    JUCE-free for the same reason TimelineGeometry is: the conversions and the
    clamping are where selection bugs actually live, and a Component can't be
    unit-tested headlessly. Seconds rather than sample indices is deliberate —
    a clip's file may be at any sample rate, and every caller that matters
    (the thumbnail, the UI readout, the offline processing range) already
    speaks in seconds or converts once at the edge.
*/
struct AudioSelection
{
    double visibleStartSeconds = 0.0;  // leftmost point currently drawn
    double secondsPerPixel     = 0.01; // zoom: smaller means further in
    float  contentLeft         = 0.0f; // x of the first drawn pixel
    double fileLengthSeconds   = 0.0;

    /** How far the view extends, which is deliberately *past* the end of the
        file. Without room beyond the last sample there is nowhere to put the
        cursor to paste after the recording ends — the click would clamp back
        onto the final sample. Selections still clamp to the file; only the
        cursor may go past it, because a selection of nothing is meaningless
        while a paste position past the end is not. */
    double viewLengthSeconds   = 0.0;

    /** The greater of the file and the view — everything that scrolls,
        zooms or hit-tests works against this. */
    double spanSeconds() const
    {
        return std::max(fileLengthSeconds, std::max(0.0, viewLengthSeconds));
    }

    /** x-coordinate for a position in the file. Not clamped — callers draw
        into a clipped region, and clamping here would silently pile
        off-screen content onto the edges. */
    float xForSeconds(double seconds) const
    {
        const double perPixel = secondsPerPixel > 0.0 ? secondsPerPixel : 1.0e-9;
        return contentLeft + (float) ((seconds - visibleStartSeconds) / perPixel);
    }

    /** Position for an x-coordinate, clamped into the *view* rather than the
        file — so the cursor can be placed past the last sample. A drag that
        leaves the component is the normal case, not an error, so this
        saturates rather than returning something out of range. */
    double secondsForX(float x) const
    {
        const double perPixel = secondsPerPixel > 0.0 ? secondsPerPixel : 1.0e-9;
        const double raw      = visibleStartSeconds + (double) (x - contentLeft) * perPixel;
        return std::clamp(raw, 0.0, spanSeconds());
    }

    double clampToFile(double seconds) const
    {
        return std::clamp(seconds, 0.0, fileLengthSeconds > 0.0 ? fileLengthSeconds : 0.0);
    }

    /** How many seconds fit across @p widthPixels at the current zoom. */
    double visibleSeconds(float widthPixels) const
    {
        return (double) widthPixels * (secondsPerPixel > 0.0 ? secondsPerPixel : 1.0e-9);
    }

    /** The scroll offset clamped so the view can't be scrolled past either
        end — and so content shorter than the view sits at 0 rather than
        floating. */
    double clampedStart(double desiredStart, float widthPixels) const
    {
        const double span = visibleSeconds(widthPixels);
        const double most = spanSeconds() - span;
        if (most <= 0.0)
            return 0.0;
        return std::clamp(desiredStart, 0.0, most);
    }

    /** The zoom that fits everything across @p widthPixels — including the
        room past the end of the file, so "Fit" shows the space you can paste
        into rather than hiding it. */
    double secondsPerPixelToFit(float widthPixels) const
    {
        if (widthPixels <= 0.0f || spanSeconds() <= 0.0)
            return 0.01;
        return spanSeconds() / (double) widthPixels;
    }
};

/** A selected span of a file, in seconds. Empty (start == end) means "no
    selection", which is distinct from "the whole file" — actions fall back
    to the whole clip when nothing is selected, so the two must not be
    conflated. */
struct AudioRange
{
    double startSeconds = 0.0;
    double endSeconds   = 0.0;

    bool   isEmpty() const { return endSeconds <= startSeconds; }
    double lengthSeconds() const { return isEmpty() ? 0.0 : endSeconds - startSeconds; }

    /** Normalised so start <= end. A drag runs in either direction, so the
        raw anchor/current pair routinely arrives backwards; every consumer
        wants it ordered. */
    static AudioRange fromDrag(double anchorSeconds, double currentSeconds)
    {
        AudioRange r;
        r.startSeconds = std::min(anchorSeconds, currentSeconds);
        r.endSeconds   = std::max(anchorSeconds, currentSeconds);
        return r;
    }

    /** Clipped into [0, fileLengthSeconds]. Returns an empty range if it
        falls entirely outside the file. */
    AudioRange clampedTo(double fileLengthSeconds) const
    {
        AudioRange r;
        r.startSeconds = std::clamp(startSeconds, 0.0, fileLengthSeconds);
        r.endSeconds   = std::clamp(endSeconds,   0.0, fileLengthSeconds);
        if (r.endSeconds < r.startSeconds)
            r.endSeconds = r.startSeconds;
        return r;
    }

    bool operator==(const AudioRange&) const = default;
};

} // namespace looper
