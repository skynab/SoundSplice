#pragma once

namespace looper
{
/**
    Pure geometry for the arrangement timeline: converts between musical beats
    and pixel x-coordinates, and computes the content size for a given song span.
    JUCE-free so the conversion math is unit-tested headless.
*/
struct TimelineGeometry
{
    float gutterWidth       = 110.0f; // fixed left column for track names
    float rulerHeight       = 22.0f;
    float laneHeight        = 40.0f;
    float basePixelsPerBeat = 24.0f;  // pixels per quarter-note beat at zoom 1.0
    float zoom              = 1.0f;

    float pixelsPerBeat() const { return basePixelsPerBeat * zoom; }

    /** x-coordinate (in the content component) for a beat position. */
    float xForBeat(double beat) const
    {
        return gutterWidth + (float) (beat * (double) pixelsPerBeat());
    }

    /** Beat position for an x-coordinate; clamped to 0 at/left of the gutter. */
    double beatForX(float x) const
    {
        const float rel = x - gutterWidth;
        return rel > 0.0f ? (double) (rel / pixelsPerBeat()) : 0.0;
    }

    /** Total content width for a timeline spanning @p totalBeats of music. */
    float contentWidth(double totalBeats) const
    {
        return gutterWidth + (float) (totalBeats * (double) pixelsPerBeat());
    }

    /** Total content height for @p numTracks lanes (at least one, so it's never empty). */
    float contentHeight(int numTracks) const
    {
        return rulerHeight + laneHeight * (float) (numTracks > 0 ? numTracks : 1);
    }
};

} // namespace looper
