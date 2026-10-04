#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "engine/TempoMap.h"
#include "model/Song.h"
#include "model/TimeSelection.h"

namespace soundsplice::app::recording
{
/**
    The decisions recording makes, apart from the doing of it: which tracks a
    take lands on, how late it comes back, and when a timed take starts and
    stops. JUCE-free and tested; MainComponent_Recording.cpp gathers the inputs
    and acts on the answers.
*/

/** The tracks a take records onto, by index, the first being the main take:
    every armed audio track, in order, each from its own input; with none
    armed, the selected track if it can hold audio, or -1 for a new one. */
inline std::vector<int> takeTargets(const model::Song& song, const std::set<int>& armedTrackIds, int selectedTrack)
{
    std::vector<int> targets;
    for (int t = 0; t < (int) song.tracks.size(); ++t)
        if (song.tracks[(size_t) t].type == model::TrackType::Audio && armedTrackIds.count(song.tracks[(size_t) t].id) > 0)
            targets.push_back(t);
    if (targets.empty())
    {
        const bool canHoldAudio = selectedTrack >= 0 && selectedTrack < (int) song.tracks.size()
                               && song.tracks[(size_t) selectedTrack].type == model::TrackType::Audio;
        targets.push_back(canHoldAudio ? selectedTrack : -1);
    }
    return targets;
}

/** What a take does with the time selection: goes round it, each pass a
    take of one clip (Loop on); replaces only it, keeping the lead-up out
    (Punch Recording on); or neither. */
struct TakeRange
{
    bool   loop      = false;
    bool   punch     = false;
    double fromBeats = 0.0;
    double toBeats   = 0.0;
};

inline TakeRange takeRange(const model::TimeSelection& selection, bool loopOn, bool punchOn)
{
    TakeRange range;
    range.loop      = loopOn && ! selection.isEmpty();
    range.punch     = ! range.loop && ! selection.isEmpty() && punchOn;
    range.fromBeats = selection.startBeats;
    range.toBeats   = selection.endBeats;
    return range;
}

/** Where a loop recording has to start from, the playhead being at
    @p playheadBeat: the loop's start if it's outside the loop, so there is a
    loop to go round; nothing if it's inside, or this isn't a loop take. */
inline std::optional<double> loopTakeSeek(const TakeRange& range, double playheadBeat)
{
    if (! range.loop || (playheadBeat >= range.fromBeats && playheadBeat < range.toBeats))
        return std::nullopt;
    return range.fromBeats;
}

/** A track recording alongside the main take, on one of the engine's extra
    recorders. */
struct ExtraTake
{
    int trackIndex = -1;
    int slot       = -1;
};

/** An audio take in progress. Session state, not part of the song: reset in
    one go when the take ends. */
struct AudioTake
{
    bool                   running   = false;
    int                    mainTrack = -1; // -1 = a new track
    TakeRange              range;
    std::vector<ExtraTake> extras;
};

/** The first of @p slotCount extra recorders that @p extras isn't using, for
    a track armed mid-take; nothing if all of them are. */
inline std::optional<int> freeRecorderSlot(const std::vector<ExtraTake>& extras, int slotCount)
{
    for (int slot = 0; slot < slotCount; ++slot)
        if (std::none_of(extras.begin(), extras.end(), [slot](const ExtraTake& extra) { return extra.slot == slot; }))
            return slot;
    return std::nullopt;
}

/** The Recording Latency settings. */
struct Latency
{
    bool   compensate      = true;
    int    measuredSamples = -1;  // File > Measure Recording Latency's answer; -1 for none
    double measuredRate    = 0.0; // the rate it was measured at
    double adjustMs        = 0.0; // + moves recordings earlier
};

/** Whether the measured round trip applies at @p rate, rather than what the
    device reports - only at the rate it was measured at. */
inline bool usesMeasured(const Latency& latency, double rate)
{
    return latency.measuredSamples >= 0 && std::abs(latency.measuredRate - rate) < 0.5;
}

/** How many samples late a recording comes back at @p rate, which it's moved
    back by: the measured round trip or @p reportedSamples, adjusted; nothing
    with compensation off. */
inline int latencySamples(const Latency& latency, double rate, int reportedSamples)
{
    if (! latency.compensate)
        return 0;
    const int adjust = (int) std::lround(latency.adjustMs * 0.001 * rate);
    return std::max(0, (usesMeasured(latency, rate) ? latency.measuredSamples : reportedSamples) + adjust);
}

/** Sound-Activated Recording as the engine takes it: the level a take waits
    for (0 to start at once), and how long a silence stops it (0 never). */
struct SoundTrigger
{
    float   thresholdGain    = 0.0f;
    int64_t stopAfterSamples = 0;
};

inline SoundTrigger soundTrigger(bool on, double thresholdDb, double stopSeconds, double rate)
{
    if (! on)
        return {};
    rate = rate > 0.0 ? rate : 48000.0;
    const float gain = thresholdDb > -100.0 ? (float) std::pow(10.0, thresholdDb / 20.0) : 0.0f;
    return { gain, stopSeconds > 0.0 ? (int64_t) (stopSeconds * rate) : 0 };
}

/** Timer Record: a take that starts at a set time and, given a length, stops
    by itself. Times are in milliseconds, as juce::Time keeps them. */
class TimerRecord
{
public:
    enum class Action
    {
        None,
        Start, // press Record
        Stop   // press Stop
    };

    void schedule(int64_t nowMs, double startInMinutes, double lengthMinutes)
    {
        pending_ = true;
        startMs_ = nowMs + (int64_t) std::llround(std::max(0.0, startInMinutes) * 60000.0);
        stopMs_.reset();
        if (lengthMinutes > 0.0)
            stopMs_ = startMs_ + (int64_t) std::llround(lengthMinutes * 60000.0);
    }

    void cancel()
    {
        pending_ = false;
        stopMs_.reset();
    }

    bool                   isPending() const noexcept { return pending_; }
    int64_t                startMs() const noexcept { return startMs_; }
    std::optional<int64_t> stopMs() const noexcept { return stopMs_; }

    /** What to do at @p nowMs, with a take running or not: each of start and
        stop happens once, and only if it would change anything. */
    Action tick(int64_t nowMs, bool recording)
    {
        if (pending_ && nowMs >= startMs_)
        {
            pending_ = false;
            return recording ? Action::None : Action::Start;
        }
        if (stopMs_ && nowMs >= *stopMs_)
        {
            stopMs_.reset();
            return recording ? Action::Stop : Action::None;
        }
        return Action::None;
    }

private:
    bool                   pending_ = false;
    int64_t                startMs_ = 0;
    std::optional<int64_t> stopMs_;
};

} // namespace soundsplice::app::recording
