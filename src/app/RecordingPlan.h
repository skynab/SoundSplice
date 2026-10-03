#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "engine/MidiCapture.h"
#include "engine/TempoMap.h"
#include "model/Song.h"

namespace soundsplice::app::recording
{
/**
    The decisions recording makes, apart from the doing of it: which tracks a
    take lands on, how late it comes back, when a timed take starts and stops,
    and what a MIDI take becomes. JUCE-free and tested, as RecordSourceChoice
    is; MainComponent_Recording.cpp gathers the inputs and acts on the answers.
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

/** A MIDI take as the clip it becomes. */
struct MidiTake
{
    double                    startBeats  = 0.0;
    double                    lengthBeats = 0.0; // the take's, rounded up to a whole bar
    std::vector<engine::Note> notes;             // from the clip's start
};

/** The take captured from @p startSample to @p endSample as a clip, through
    @p tempo: a take spanning a tempo change can't be converted by one scalar,
    which is why engine::MidiCapture works in beats and this does the
    converting. Nothing if no note was played - every event an unmatched
    note-off, from keys already down when capture began. */
inline std::optional<MidiTake> clipFromMidiTake(const std::vector<engine::RecordedMidiEvent>& events,
                                                int64_t startSample, int64_t endSample, const engine::TempoMap& tempo)
{
    MidiTake take;
    take.startBeats = std::max(0.0, tempo.ppqFromSamples(startSample));
    const double takeEndBeats = endSample > startSample ? std::max(take.startBeats, tempo.ppqFromSamples(endSample))
                                                        : take.startBeats;

    std::vector<engine::TimedMidiEvent> timed;
    timed.reserve(events.size());
    for (const auto& event : events)
    {
        engine::TimedMidiEvent converted;
        // From the take's own start: a clip's notes are positioned from the
        // clip start, and the clip is placed at startBeats.
        converted.beats      = tempo.ppqFromSamples(event.timeSamples) - take.startBeats;
        converted.noteNumber = event.noteNumber;
        converted.velocity   = event.velocity;
        converted.noteOn     = event.noteOn;
        timed.push_back(converted);
    }

    take.notes = engine::MidiCapture::notesFromEvents(std::move(timed), takeEndBeats - take.startBeats);
    if (take.notes.empty())
        return std::nullopt;

    // As long as the take, rounded up to a whole bar: a take is a musical
    // phrase, and ending the clip on the last note's release would make a
    // loop of it jarringly short.
    double contentEnd = takeEndBeats - take.startBeats;
    for (const auto& note : take.notes)
        contentEnd = std::max(contentEnd, note.startBeats + note.lengthBeats);
    take.lengthBeats = engine::MidiCapture::clipLengthForTake(contentEnd, std::max(1.0, tempo.quartersPerBar()));
    return take;
}

} // namespace soundsplice::app::recording
