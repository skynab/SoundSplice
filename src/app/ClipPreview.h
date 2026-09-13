#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Pattern.h"

namespace looper
{
/** One note as a block inside a clip rectangle. All four values are
    fractions of the rectangle: x/width along it, y/height down it, with y
    measured from the top so higher pitches sit higher. */
struct ClipPreviewBlock
{
    double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
};

/**
    Turns a clip's notes into blocks to draw inside its rectangle.

    A clip in the arrangement is otherwise a plain coloured box, which says
    where a part sits but nothing about what it is — two clips containing
    completely different music look identical, and the only way to tell them
    apart is to open each one.

    Two things this has to get right, both of which are invisible in a
    screenshot and obvious once you're using it:

    - **A clip repeats its pattern.** The engine wraps playback within the
      pattern length (see PatternPlayback), so an 8-beat clip holding a
      4-beat pattern plays it twice. A preview that drew the notes once and
      left the rest empty would be showing something the clip doesn't do.

    - **Pitch is scaled to the notes present**, not to the full MIDI range.
      Against 0..127 a typical bassline occupies about a twelfth of the
      height and reads as a flat line. The span is padded so a clip whose
      notes are all one pitch still draws them in the middle rather than
      dividing by zero.

    JUCE-free, like PianoRollGeometry and TimelineGeometry, so the arithmetic
    is testable without a screen.
*/
inline std::vector<ClipPreviewBlock> clipPreviewBlocks(const std::vector<engine::Note>& notes,
                                                       double patternLengthBeats,
                                                       double clipLengthBeats,
                                                       int    maxBlocks = 256)
{
    std::vector<ClipPreviewBlock> blocks;

    if (notes.empty() || clipLengthBeats <= 0.0 || maxBlocks <= 0)
        return blocks;

    // A pattern length of zero would divide by zero below; treat it as one
    // pass over the clip, which is the least surprising reading.
    const double patternLength = patternLengthBeats > 0.0 ? patternLengthBeats : clipLengthBeats;

    int lowest = 127, highest = 0;
    for (const auto& note : notes)
    {
        lowest  = std::min(lowest, note.noteNumber);
        highest = std::max(highest, note.noteNumber);
    }

    // Pad the span so a single-pitch clip still has somewhere to draw, and so
    // notes at the extremes aren't flush against the edges.
    const double lowEdge  = (double) lowest - 1.0;
    const double highEdge = (double) highest + 1.0;
    const double span     = highEdge - lowEdge;

    const int repeats = (int) std::ceil(clipLengthBeats / patternLength);

    for (int repeat = 0; repeat < repeats && (int) blocks.size() < maxBlocks; ++repeat)
    {
        const double offset = (double) repeat * patternLength;

        for (const auto& note : notes)
        {
            if ((int) blocks.size() >= maxBlocks)
                break;

            const double start = offset + note.startBeats;
            if (start >= clipLengthBeats)
                continue; // the clip was cut short of this repeat

            // A note running past the clip's end is drawn up to the end, as
            // that is how much of it is actually heard.
            const double end = std::min(start + std::max(note.lengthBeats, 0.01), clipLengthBeats);

            ClipPreviewBlock block;
            block.x      = start / clipLengthBeats;
            block.width  = std::max((end - start) / clipLengthBeats, 0.0);
            block.height = 1.0 / span;
            block.y      = (highEdge - (double) note.noteNumber - 1.0) / span;

            blocks.push_back(block);
        }
    }

    return blocks;
}

/**
    How much of an audio clip's width its waveform actually covers, as a
    fraction of that width.

    An audio clip does not loop — AudioClipSlot plays the file once from the
    clip's start and goes silent when the file runs out or the clip's window
    ends, whichever comes first. So a waveform stretched to fill the clip
    would be a picture of something the clip doesn't do: a two-second file in
    an eight-beat clip at 120bpm is heard for the first half and silent for
    the rest, and the drawing has to say so.

    The reverse case is just as real. A file longer than its clip is audible
    only up to the clip's end, so the waveform is drawn up to there and the
    rest of the file isn't shown at all.

    Returns 0 when there is nothing to draw, which callers use to skip the
    work entirely.
*/
inline double audioClipDrawnFraction(double fileSeconds, double clipLengthBeats, double secondsPerBeat)
{
    if (fileSeconds <= 0.0 || clipLengthBeats <= 0.0 || secondsPerBeat <= 0.0)
        return 0.0;

    const double clipSeconds = clipLengthBeats * secondsPerBeat;
    if (clipSeconds <= 0.0)
        return 0.0;

    return std::min(1.0, fileSeconds / clipSeconds);
}

/** The seconds of a file an audio clip actually plays — the same rule as
    audioClipDrawnFraction, expressed as a duration so a thumbnail can be
    asked for exactly that range. */
inline double audioClipAudibleSeconds(double fileSeconds, double clipLengthBeats, double secondsPerBeat)
{
    if (fileSeconds <= 0.0 || clipLengthBeats <= 0.0 || secondsPerBeat <= 0.0)
        return 0.0;

    return std::min(fileSeconds, clipLengthBeats * secondsPerBeat);
}

} // namespace looper
