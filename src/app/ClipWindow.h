#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/SequencerMath.h"
#include "model/Clip.h"

namespace soundsplice
{
/**
    The arithmetic of an audio clip's window onto its file.

    An audio clip plays [sourceOffsetSeconds, sourceOffsetSeconds + its length)
    of its file, so trimming, splitting and slipping a clip are changes to
    those two numbers and its start — the file itself is never rewritten.
    That makes these the edits every other one builds on, and each has an
    edge that is easy to get subtly wrong (a split whose halves don't abut, a
    left trim that reveals audio from before the file began), hence JUCE-free
    and tested on their own rather than worked out inline at each call site.

    Seconds into the file and beats on the timeline meet here: the offset is
    in seconds because the audio underneath is real time, while the clip's
    start and length stay in beats like every other clip's.
*/

/** The shortest a clip may be trimmed to, in beats. A floor rather than a
    real minimum — callers with a coarser idea of "too short" apply their own. */
inline constexpr double kMinTrimmedClipBeats = 0.25;

/** A range of sample indices into a file, [start, end). */
struct SampleWindow
{
    int start = 0;
    int end   = 0;

    int  length() const { return end - start; }
    bool isEmpty() const { return end <= start; }
};

/** The seconds of audio @p clip actually plays: its length, cut short where
    the file runs out after its offset. */
inline double clipAudibleSeconds(const model::Clip& clip, double fileSeconds, double bpm)
{
    if (bpm <= 0.0)
        return 0.0;

    const double available = fileSeconds - clip.sourceOffsetSeconds;
    return std::max(0.0, std::min(available, clip.lengthBeats * 60.0 / bpm));
}

/** Which samples of a @p fileLengthSamples-long file @p clip plays. Empty if
    the offset is past the end of the file. */
inline SampleWindow clipSampleWindow(const model::Clip& clip, int fileLengthSamples, double sampleRate,
                                     double bpm)
{
    if (sampleRate <= 0.0 || bpm <= 0.0 || fileLengthSamples <= 0)
        return {};

    const auto offset = (std::int64_t) std::llround(clip.sourceOffsetSeconds * sampleRate);
    const auto wanted = (std::int64_t) std::llround(clip.lengthBeats * 60.0 / bpm * sampleRate);

    const auto start = std::clamp<std::int64_t>(offset, 0, fileLengthSamples);
    const auto end   = std::clamp<std::int64_t>(start + std::max<std::int64_t>(0, wanted), start,
                                                fileLengthSamples);
    return { (int) start, (int) end };
}

/** A copy of the samples inside @p window. */
inline std::vector<float> windowSamples(const std::vector<float>& file, SampleWindow window)
{
    const int size  = (int) file.size();
    const int start = std::clamp(window.start, 0, size);
    const int end   = std::clamp(window.end, start, size);
    return std::vector<float>(file.begin() + start, file.begin() + end);
}

/** @p file with @p window replaced by @p replacement, which may be a
    different length. What's either side of the window is kept as it was, so
    a destructive edit to a trimmed clip leaves the audio it hides intact and
    its offset still pointing at the same place. */
inline std::vector<float> spliceWindow(const std::vector<float>& file, SampleWindow window,
                                       const std::vector<float>& replacement)
{
    const int size  = (int) file.size();
    const int start = std::clamp(window.start, 0, size);
    const int end   = std::clamp(window.end, start, size);

    std::vector<float> out;
    out.reserve((size_t) (size - (end - start)) + replacement.size());
    out.insert(out.end(), file.begin(), file.begin() + start);
    out.insert(out.end(), replacement.begin(), replacement.end());
    out.insert(out.end(), file.begin() + end, file.end());
    return out;
}

/** @p clip cut down to [fromSeconds, toSeconds), measured from the clip's
    start. The kept audio stays where it was on the timeline: the clip's start
    moves forward to meet it rather than the audio jumping back. */
inline model::Clip trimClipToRange(const model::Clip& clip, double fromSeconds, double toSeconds, double bpm)
{
    model::Clip out = clip;
    if (bpm <= 0.0)
        return out;

    const double from = std::max(0.0, fromSeconds);
    const double to   = std::max(from, toSeconds);

    out.startBeats          = clip.startBeats + engine::beatsForSeconds(from, bpm);
    out.lengthBeats         = engine::beatsForSeconds(to - from, bpm);
    out.sourceOffsetSeconds = clip.sourceOffsetSeconds + from;
    return out;
}

/** @p clip split at @p atSeconds from its start into two clips that play the
    same file back to back. The second keeps the first's id; the caller gives
    it a new one when adding it. Callers refuse a split at either edge. */
inline std::pair<model::Clip, model::Clip> splitClipAt(const model::Clip& clip, double atSeconds, double bpm)
{
    const double atBeats = bpm > 0.0 ? engine::beatsForSeconds(atSeconds, bpm) : 0.0;

    model::Clip first = clip;
    first.lengthBeats = atBeats;

    model::Clip second         = clip;
    second.startBeats          = clip.startBeats + atBeats;
    second.lengthBeats         = clip.lengthBeats - atBeats;
    second.sourceOffsetSeconds = clip.sourceOffsetSeconds + atSeconds;

    return { first, second };
}

/** @p clip with its left edge moved to @p newStartBeats and its audio left
    where it was on the timeline, as dragging the edge does.

    Clamped three ways: not before beat zero, not so far left that the clip
    would start before its file's first sample (there is no audio there to
    reveal), and not so far right that less than @p minLengthBeats remains. */
inline model::Clip trimClipStart(const model::Clip& clip, double newStartBeats, double bpm,
                                 double minLengthBeats)
{
    model::Clip out = clip;
    if (bpm <= 0.0)
        return out;

    const double secondsPerBeat = 60.0 / bpm;

    // Where the file's first sample sits on the timeline.
    const double earliest = std::max(0.0, clip.startBeats - clip.sourceOffsetSeconds / secondsPerBeat);
    const double latest   = clip.startBeats + clip.lengthBeats - std::max(0.0, minLengthBeats);

    // A clip already shorter than the minimum can't move its start at all.
    if (latest < earliest)
        return out;

    const double start = std::clamp(newStartBeats, earliest, latest);
    const double delta = start - clip.startBeats;

    out.startBeats          = start;
    out.lengthBeats         = clip.lengthBeats - delta;
    out.sourceOffsetSeconds = std::max(0.0, clip.sourceOffsetSeconds + delta * secondsPerBeat);
    return out;
}

} // namespace soundsplice
