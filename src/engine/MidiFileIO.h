#pragma once

#include <algorithm>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "engine/Pattern.h"
#include "model/Song.h"

namespace looper::engine
{
/**
    Converts between model::Song and Standard MIDI Files (.mid), using JUCE's
    built-in juce::MidiFile — no new dependency. Lives in engine/, not model/,
    because model/ is deliberately JUCE-free (same reasoning that already puts
    OfflineRenderer.h here despite needing juce_audio_formats).

    The engine has one global song.bpm, not a tempo map over time, so a source
    file with tempo *changes* mid-song can't be represented exactly — see
    MidiImportResult::extraTempoEventsIgnored, which reports rather than
    silently drops them. SMPTE-based time formats (rare) aren't supported.
*/
struct MidiImportResult
{
    bool ok                        = false;
    int  tracksImported             = 0;
    int  extraTempoEventsIgnored    = 0;
};

/** Imports @p file, appending one new Instrument track per non-empty MIDI
    track (a tempo-only or otherwise note-less track imports nothing). Each
    imported track gets one clip spanning its whole content, starting at beat
    0 — the same "single clip, unbounded window" convention already used
    elsewhere, not multi-clip splitting a single imported track doesn't need.
    song.bpm is set from the file's first tempo event, if it has one. */
inline MidiImportResult importMidiFile(const juce::File& file, model::Song& song)
{
    MidiImportResult result;

    juce::FileInputStream stream(file);
    if (! stream.openedOk())
        return result;

    juce::MidiFile midiFile;
    if (! midiFile.readFrom(stream)) // default createMatchingNoteOffs = true
        return result;

    const short timeFormat = midiFile.getTimeFormat();
    if (timeFormat <= 0)
        return result; // SMPTE-based time format — not supported in v1

    const double ticksPerQuarterNote = (double) timeFormat;

    // First *usable* tempo event found (scanning every track — Type 0 files
    // keep it alongside notes, Type 1 files usually put it on its own track
    // 0) sets song.bpm; every tempo event after that is counted rather than
    // silently dropped.
    bool foundTempo = false;
    for (int t = 0; t < midiFile.getNumTracks(); ++t)
    {
        const auto* track = midiFile.getTrack(t);
        if (track == nullptr)
            continue;

        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& msg = track->getEventPointer(i)->message;
            if (! msg.isTempoMetaEvent())
                continue;

            if (foundTempo)
            {
                ++result.extraTempoEventsIgnored;
                continue;
            }

            const double secondsPerQuarterNote = msg.getTempoSecondsPerQuarterNote();
            if (secondsPerQuarterNote > 0.0)
            {
                song.bpm    = 60.0 / secondsPerQuarterNote;
                foundTempo = true;
            }
        }
    }

    for (int t = 0; t < midiFile.getNumTracks(); ++t)
    {
        const auto* track = midiFile.getTrack(t);
        if (track == nullptr)
            continue;

        Pattern pattern;
        double  maxEndBeat = 0.0;

        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto* holder = track->getEventPointer(i);
            const auto& msg    = holder->message;
            if (! msg.isNoteOn())
                continue;

            const double startBeat = msg.getTimeStamp() / ticksPerQuarterNote;
            double       endBeat    = startBeat + 0.25; // fallback if somehow unmatched
            if (holder->noteOffObject != nullptr)
                endBeat = holder->noteOffObject->message.getTimeStamp() / ticksPerQuarterNote;

            const double lengthBeat = std::max(1.0e-3, endBeat - startBeat);
            pattern.notes.push_back({ startBeat, lengthBeat, msg.getNoteNumber(), msg.getFloatVelocity() });
            maxEndBeat = std::max(maxEndBeat, startBeat + lengthBeat);
        }

        if (pattern.notes.empty())
            continue; // a tempo-only or otherwise note-less track

        pattern.lengthBeats = maxEndBeat;

        const auto name = "MIDI " + juce::String(result.tracksImported + 1);
        auto&      newTrack = model::addTrack(song, model::TrackType::Instrument, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(song);
        clip.type        = model::ClipType::Instrument;
        clip.startBeats  = 0.0;
        clip.lengthBeats = pattern.lengthBeats;
        clip.pattern     = pattern;
        newTrack.clips.push_back(clip);

        ++result.tracksImported;
    }

    result.ok = result.tracksImported > 0;
    return result;
}

/** Exports every Instrument track's notes (audio tracks have nothing to
    export and are skipped) to @p file as a Type-1 Standard MIDI File: one
    MidiMessageSequence per track, each clip's notes offset by its own
    startBeats so the whole arrangement round-trips, not just one clip. Plus
    one tempo meta-event from song.bpm. Returns false if there was nothing to
    write or the file couldn't be opened. */
inline bool exportMidiFile(const juce::File& file, const model::Song& song)
{
    constexpr int ticksPerQuarterNote = 960;

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(ticksPerQuarterNote);

    juce::MidiMessageSequence tempoTrack;
    const int microsecondsPerQuarterNote =
        (int) std::llround(60.0e6 / std::max(1.0, song.bpm));
    tempoTrack.addEvent(juce::MidiMessage::tempoMetaEvent(microsecondsPerQuarterNote));
    midiFile.addTrack(tempoTrack);

    int exportedTracks = 0;
    for (const auto& track : song.tracks)
    {
        if (track.type != model::TrackType::Instrument)
            continue;

        juce::MidiMessageSequence seq;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Instrument)
                continue;

            for (const auto& note : clip.pattern.notes)
            {
                const double startBeat = clip.startBeats + note.startBeats;
                const double endBeat   = startBeat + note.lengthBeats;
                const auto   velocity  = (juce::uint8) juce::jlimit(1, 127, (int) std::lround(note.velocity * 127.0));

                auto onMsg = juce::MidiMessage::noteOn(1, note.noteNumber, velocity);
                onMsg.setTimeStamp(startBeat * ticksPerQuarterNote);
                seq.addEvent(onMsg);

                auto offMsg = juce::MidiMessage::noteOff(1, note.noteNumber);
                offMsg.setTimeStamp(endBeat * ticksPerQuarterNote);
                seq.addEvent(offMsg);
            }
        }

        if (seq.getNumEvents() == 0)
            continue; // an instrument track with no notes at all

        seq.updateMatchedPairs();
        midiFile.addTrack(seq);
        ++exportedTracks;
    }

    if (exportedTracks == 0)
        return false;

    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    return midiFile.writeTo(*stream, 1);
}

} // namespace looper::engine
