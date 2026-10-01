#pragma once

#include <vector>

#include "engine/TempoMap.h"

namespace soundsplice::model
{
/**
    Seconds and beats along the song's timeline: what an edit to an audio
    clip needs to turn a stretch of beats into a stretch of its file, and
    back.

    With one tempo it is exactly the arithmetic the edits always did, beats
    times 60 over the tempo, so a single-tempo project edits bit for bit as
    before. With tempo changes (Song::tempoChanges) it measures through the
    map, so a split, trim or crossfade after a tempo change, or across one,
    lands on the audio that's there.

    Converts implicitly from a plain tempo, so code and tests written for one
    tempo pass a number; the song's own clock is model::clockFor
    (model/TempoChanges.h).
*/
class BeatClock
{
public:
    /** One tempo throughout. */
    BeatClock(double bpm) : bpm_(bpm) {} // NOLINT: implicit on purpose, see above

    /** The tempo map @p changes (start first, as engine::TempoMap takes it). */
    explicit BeatClock(const std::vector<engine::TempoChange>& changes)
    {
        if (changes.size() <= 1)
        {
            bpm_ = changes.empty() ? 120.0 : changes.front().bpm;
            return;
        }

        mapped_ = true;
        bpm_    = changes.front().bpm;
        map_.setSampleRate(1.0); // a "sample" a second: offsets are seconds
        map_.setTempoChanges(changes);
    }

    /** False for a tempo that isn't positive, where nothing can be converted. */
    bool valid() const noexcept { return mapped_ || bpm_ > 0.0; }

    /** How long, in seconds, the stretch from beat @p from to @p to lasts. */
    double secondsBetween(double from, double to) const noexcept
    {
        if (! mapped_)
            return (to - from) * 60.0 / bpm_;
        return map_.sampleOffsetForPpq(to) - map_.sampleOffsetForPpq(from);
    }

    /** The beat @p seconds after beat @p from. */
    double beatAfter(double from, double seconds) const noexcept
    {
        if (! mapped_)
            return from + seconds * bpm_ / 60.0;
        return map_.ppqFromSampleOffset(map_.sampleOffsetForPpq(from) + seconds);
    }

    /** How many beats the @p seconds after beat @p from span: with one
        tempo, exactly seconds times the tempo over 60, as the edits always
        worked it out. */
    double beatsAfter(double from, double seconds) const noexcept
    {
        if (! mapped_)
            return seconds * bpm_ / 60.0;
        return beatAfter(from, seconds) - from;
    }

    double secondsAt(double beat) const noexcept { return secondsBetween(0.0, beat); }
    double beatAt(double seconds) const noexcept { return beatAfter(0.0, seconds); }

    /** The tempo in force at @p beat. */
    double bpmAt(double beat) const noexcept { return mapped_ ? map_.tempoAtBeat(beat) : bpm_; }

private:
    double           bpm_    = 120.0;
    bool             mapped_ = false;
    engine::TempoMap map_;
};

} // namespace soundsplice::model
