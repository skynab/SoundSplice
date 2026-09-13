#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "model/Song.h"

namespace looper::model
{
/**
    A small, line-based text format for the project document. Deliberately
    JUCE-free so the round-trip can be unit-tested headless.

    Layout is flat and count-prefixed so it parses deterministically. Numbers use
    %.17g (exact IEEE double round-trip); string fields (track name, audio file)
    are the rest of their line, so they may contain spaces.

    **Versioning.** `serialize` always writes kFormatVersion; there is no
    "save as an older version", so there's one write path to reason about and
    test. `deserialize` is the tolerant side: records introduced after a given
    version are read only *if present*, so an older file simply leaves those
    fields at their struct defaults (which are chosen to be behaviour-
    preserving). Where a record's own shape changed rather than a new record
    being added — only DPAD so far — the version decides how to read it.

    A file written by a *newer* build is refused outright rather than
    part-parsed: silently dropping records the user can't see would be worse
    than declining to open it.
*/

/** Bumped whenever the format changes. History worth knowing:
      11  the format before per-track synths
      12  + SYNTH (per-track model::SynthSettings)
      13  DPAD carries per-pad gain/pan/pitch/mute/solo before its sample path
      14  + TFX (per-track insert filter/delay/reverb)
      15  TRACK carries pan before its (rest-of-line) name
      16  TAUTO (one gain lane) -> TAUTOS/TLANE (a lane per parameter)
      17  + SCENES/SCENE and per-track SESSION/SSLOT (the session grid)
      18  TFX (a fixed filter/delay/reverb trio) -> FXCHAIN/FXSLOT (an
          ordered chain whose slots may be built-ins or hosted plugins)
      19  + GUITAR (per-track model::GuitarSettings)
      20  FXSLOT gains five drive fields (the guitar pedal). A file written
          before this simply stops short of them, and the reader keeps the
          defaults it started with.
      21  + seven more on the same line: compressor and tremolo pedals,
          read the same tolerant way.
      22  TRACK carries its colour before the rest-of-line name, the same
          way pan joined in v15.
      23  + three more FXSLOT fields: the chorus pedal, read the same
          tolerant way as 20's and 21's.
      24  + five more FXSLOT fields: the wobble pedal, read the same
          tolerant way as 20's, 21's, and 23's.
      26  SYNTH gains nine more fields: the filter envelope (amount +
          its own ADSR), the sub-oscillator, and unison — read the same
          tolerant way as every prior SYNTH/FXSLOT extension.
      27  + five more FXSLOT fields: the gate pedal, read the same
          tolerant way as 20's, 21's, 23's, and 24's.
      28  + CLIPGAIN, a per-clip trim. Its own record rather than another
          CLIP field, because CLIP ends in the rest-of-line audioFile and
          nothing can follow that; absent in older files, where 0dB is the
          right answer anyway.
      29  + MASTERING, the master-bus mastering rack. Read the same tolerant
          way as EQ in v25: absent in older files, where every field's
          default is a no-op, so an old project sounds identical.
      30  GUITAR gains the pickup resonance (frequency + Q), and FXSLOT gains
          nine more fields appended at the end of its line: two for the drive
          (asymmetry, oversampling) and seven for the new EQ pedal. All read
          the tolerant way, and every default is the behaviour that existed
          before them. Appended rather than grouped with the other drive
          fields because the line is positional.
      34  + CLIPWARP, a clip's source tempo and whether it follows the
          project's. Its own optional record for exactly the reason CLIPGAIN
          is one - CLIP ends in a rest-of-line audioFile, so nothing can
          follow it there. Absent in older files, where "not warped, tempo
          unknown" is the behaviour those files already had.
      35  FXSLOT gains one more field at the end of its line: a compressor's
          sidechain source track id. Appended, like v30's, because the line is
          positional. -1 in older files, which is "no sidechain" - exactly how
          every compressor written before this behaved.
      36  + TRACKBUS, a track's output bus id (-1 = master), and TrackType
          gains Bus. Its own optional record rather than another TRACK field,
          because TRACK ends in the rest-of-line name and nothing can follow
          it there - the same shape as CLIPGAIN and CLIPWARP. Absent in older
          files, where "straight to the master" is what they already did.
      37  GUITAR gains three more fields at the end of its line: velocity
          sensitivity, string coupling and stereo width. Appended, like v30's,
          because the line is positional - and seeded from the defaults on the
          way in, so an older file gets the improved instrument rather than a
          silent, mono, uncoupled one.
      38  GUITAR gains string stiffness, appended for the same reason and
          seeded from the default the same way.
      39  FXSLOT gains the drive's gain-stage count, appended at the end of
          its positional line like v30's and v35's. 1 in older files, which is
          the single-clipper behaviour they already had.
      40  + the drive's cabinet-IR flag, appended the same way; 0 in older
          files, which is the filter-chain cabinet they already had.
      41  + PEDALS/PEDAL after a clip's notes: sustain-pedal movements, the
          first performance data here that is not a note. Absent in older
          files, which had no way to express one. */
inline constexpr int kFormatVersion = 41;
namespace detail
{
    inline std::string num(double v)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return buffer;
    }

    /** One clip record: its header plus its note list. Shared by the
        arrangement's clips and the session grid's, so the two can't drift. */
    inline void writeClip(std::ostringstream& out, const Clip& clip)
    {
        out << "CLIP " << clip.id << " " << (int) clip.type << " "
            << num(clip.startBeats) << " " << num(clip.lengthBeats) << " "
            << num(clip.pattern.lengthBeats) << " " << clip.audioFile << "\n";

        // Its own record rather than another field on CLIP: audioFile is a
        // rest-of-line field (a path may contain spaces), so nothing can
        // follow it on that line. A separate optional record is also what
        // makes an older file readable unchanged — readTagged leaves the
        // cursor alone when the tag isn't there, and the default stands.
        out << "CLIPGAIN " << num((double) clip.gainDb) << "\n";
        out << "CLIPWARP " << (clip.warpEnabled ? 1 : 0) << " " << num(clip.sourceBpm) << "\n";
        out << "PEDALS " << clip.pattern.pedals.size() << "\n";
        for (const auto& pedal : clip.pattern.pedals)
            out << "PEDAL " << num(pedal.beat) << " " << (pedal.down ? 1 : 0) << "\n";

        out << "NOTES " << clip.pattern.notes.size() << "\n";

        for (const auto& note : clip.pattern.notes)
            out << "NOTE " << num(note.startBeats) << " " << num(note.lengthBeats)
                << " " << note.noteNumber << " " << num((double) note.velocity)
                << " " << (int) note.articulation << "\n";
    }

    inline std::string trimLeadingSpace(std::string s)
    {
        if (! s.empty() && s.front() == ' ')
            s.erase(0, 1);
        return s;
    }
}

inline std::string serialize(const Song& song)
{
    std::ostringstream out;
    out << "LOOPER " << kFormatVersion << "\n";
    out << "BPM " << detail::num(song.bpm) << "\n";

    // Only the changes *after* the start: BPM already carries beat 0, and
    // writing it twice would give two sources of truth for the same number.
    out << "TEMPOS " << song.tempoChanges.size() << "\n";
    for (const auto& change : song.tempoChanges)
        out << "TEMPOAT " << detail::num(change.beat) << " " << detail::num(change.bpm)
            << " " << (change.ramp ? 1 : 0) << "\n";
    out << "TSNUM " << song.timeSigNumerator << "\n";
    out << "TSDEN " << song.timeSigDenominator << "\n";
    out << "NEXTID " << song.nextId << "\n";
    out << "FILTER " << (song.filter.enabled ? 1 : 0) << " " << song.filter.mode << " "
        << detail::num((double) song.filter.cutoff) << " "
        << detail::num((double) song.filter.resonance) << "\n";
    out << "DELAY " << (song.delay.enabled ? 1 : 0) << " "
        << detail::num((double) song.delay.timeMs) << " "
        << detail::num((double) song.delay.feedback) << " "
        << detail::num((double) song.delay.mix) << "\n";
    out << "REVERB " << (song.reverb.enabled ? 1 : 0) << " "
        << detail::num((double) song.reverb.roomSize) << " "
        << detail::num((double) song.reverb.damping) << " "
        << detail::num((double) song.reverb.mix) << "\n";
    out << "SENDBUS " << (song.sendBus.enabled ? 1 : 0) << " "
        << (int) song.sendBus.effectType << " "
        << detail::num((double) song.sendBus.roomSize) << " "
        << detail::num((double) song.sendBus.damping) << " "
        << detail::num((double) song.sendBus.delayTimeMs) << " "
        << detail::num((double) song.sendBus.delayFeedback) << " "
        << detail::num((double) song.sendBus.returnLevel) << "\n";
    out << "EQ " << (song.eq.enabled ? 1 : 0) << " "
        << detail::num((double) song.eq.bassDb) << " "
        << detail::num((double) song.eq.midDb) << " "
        << detail::num((double) song.eq.trebleDb) << "\n";
    {
        const auto& m = song.mastering;
        out << "MASTERING " << (m.enabled ? 1 : 0) << " "
            << detail::num((double) m.lowShelfHz) << " " << detail::num((double) m.lowShelfDb) << " "
            << detail::num((double) m.peakHz) << " " << detail::num((double) m.peakDb) << " "
            << detail::num((double) m.peakQ) << " "
            << detail::num((double) m.highShelfHz) << " " << detail::num((double) m.highShelfDb) << " "
            << detail::num((double) m.exciterAmount) << " " << detail::num((double) m.exciterCrossoverHz) << " "
            << detail::num((double) m.width) << " "
            << detail::num((double) m.reverbAmount) << " " << detail::num((double) m.reverbRoomSize) << " "
            << detail::num((double) m.maximizerInputDb) << " " << detail::num((double) m.maximizerCeilingDb) << " "
            << detail::num((double) m.maximizerReleaseMs) << " "
            << detail::num((double) m.outputGainDb) << "\n";
    }
    out << "PROJECTROOT " << song.projectRootFolder << "\n";
    out << "AUTO " << song.masterGainDb.points().size() << "\n";
    for (const auto& p : song.masterGainDb.points())
        out << "APT " << detail::num(p.beat) << " " << detail::num((double) p.value) << "\n";
    out << "SCENES " << song.scenes.size() << "\n";
    for (const auto& scene : song.scenes)
        out << "SCENE " << scene.name << "\n"; // name is rest-of-line, so it may contain spaces

    out << "TRACKS " << song.tracks.size() << "\n";

    for (const auto& track : song.tracks)
    {
        out << "TRACK " << track.id << " " << (int) track.type << " "
            << detail::num((double) track.gainDb) << " " << (track.muted ? 1 : 0)
            << " " << (track.solo ? 1 : 0) << " " << detail::num((double) track.sendLevel)
            << " " << detail::num((double) track.pan)
            << " " << track.colour
            << " " << track.name << "\n";

        // Its own record for the reason CLIPGAIN has one: TRACK's name takes
        // the rest of its line, so nothing can follow it there.
        out << "TRACKBUS " << track.outputBusId << "\n";
        // Only non-empty lanes are written, so an unautomated track costs one
        // "TAUTOS 0" line rather than one empty record per automatable
        // parameter (a list that will only grow).
        size_t laneCount = 0;
        for (const auto& [param, lane] : track.automation)
            if (! lane.empty())
                ++laneCount;

        out << "TAUTOS " << laneCount << "\n";
        for (const auto& [param, lane] : track.automation)
        {
            if (lane.empty())
                continue;
            out << "TLANE " << param << " " << lane.points().size() << "\n";
            for (const auto& pt : lane.points())
                out << "TAPT " << detail::num(pt.beat) << " " << detail::num((double) pt.value) << "\n";
        }
        out << "DRUMKIT " << track.drumKit.pads.size() << "\n";
        for (const auto& pad : track.drumKit.pads)
            // label is a space-free token (no pad-rename UI exists yet, so
            // this always holds); samplePath is the rest of the line, like
            // clip.audioFile/track.name, since a real file path can have
            // spaces — so every fixed-width field has to precede it.
            out << "DPAD " << pad.noteNumber << " " << pad.label << " "
                << detail::num((double) pad.gainDb) << " "
                << detail::num((double) pad.pan) << " "
                << detail::num((double) pad.pitchSemitones) << " "
                << (pad.muted ? 1 : 0) << " " << (pad.solo ? 1 : 0) << " "
                << pad.samplePath << "\n";

        const auto& synth = track.synthSettings;
        out << "SYNTH " << synth.waveform << " "
            << detail::num((double) synth.attackMs) << " " << detail::num((double) synth.decayMs) << " "
            << detail::num((double) synth.sustain) << " " << detail::num((double) synth.releaseMs) << " "
            << (synth.filterEnabled ? 1 : 0) << " " << synth.filterMode << " "
            << detail::num((double) synth.filterCutoff) << " " << detail::num((double) synth.filterResonance) << " "
            << detail::num((double) synth.gainDb) << " "
            << detail::num((double) synth.filterEnvAmount) << " "
            << detail::num((double) synth.filterEnvAttackMs) << " "
            << detail::num((double) synth.filterEnvDecayMs) << " "
            << detail::num((double) synth.filterEnvSustain) << " "
            << detail::num((double) synth.filterEnvReleaseMs) << " "
            << (synth.subOscEnabled ? 1 : 0) << " "
            << detail::num((double) synth.subOscLevel) << " "
            << synth.unisonVoices << " "
            << detail::num((double) synth.unisonDetuneCents) << "\n";

        const auto& guitar = track.guitarSettings;
        out << "GUITAR";
        for (int note : guitar.tuning)
            out << " " << note;
        out << " " << detail::num((double) guitar.decaySeconds)
            << " " << detail::num((double) guitar.brightness)
            << " " << detail::num((double) guitar.pickPosition)
            << " " << detail::num((double) guitar.pickHardness)
            << " " << detail::num((double) guitar.muteOnNoteOff)
            << " " << detail::num((double) guitar.pickupResonanceHz)
            << " " << detail::num((double) guitar.pickupQ)
            << " " << detail::num((double) guitar.palmMuteDecaySeconds)
            << " " << detail::num((double) guitar.palmMuteBrightness)
            // Appended for the reason v30's fields were: the line is
            // positional, so anything inserted mid-line would make every older
            // file read its values into the wrong slots.
            << " " << detail::num((double) guitar.velocitySensitivity)
            << " " << detail::num((double) guitar.stringCoupling)
            << " " << detail::num((double) guitar.stereoWidth)
            << " " << detail::num((double) guitar.stiffness) << "\n";

        // The effect chain, in order. A slot carries every built-in's settings
        // regardless of its kind, so switching kind doesn't lose the others.
        out << "FXCHAIN " << track.effectChain.size() << "\n";
        for (const auto& slot : track.effectChain)
        {
            out << "FXSLOT " << (int) slot.kind << " " << (slot.enabled ? 1 : 0) << " "
                << slot.filter.mode << " "
                << detail::num((double) slot.filter.cutoff) << " "
                << detail::num((double) slot.filter.resonance) << " "
                << detail::num((double) slot.delay.timeMs) << " "
                << detail::num((double) slot.delay.feedback) << " "
                << detail::num((double) slot.delay.mix) << " "
                << detail::num((double) slot.reverb.roomSize) << " "
                << detail::num((double) slot.reverb.damping) << " "
                << detail::num((double) slot.reverb.mix) << " "
                << detail::num((double) slot.drive.drive) << " "
                << detail::num((double) slot.drive.tone) << " "
                << detail::num((double) slot.drive.level) << " "
                << (slot.drive.hardClip ? 1 : 0) << " "
                << (slot.drive.cabinet ? 1 : 0) << " "
                << detail::num((double) slot.compressor.thresholdDb) << " "
                << detail::num((double) slot.compressor.ratio) << " "
                << detail::num((double) slot.compressor.attackMs) << " "
                << detail::num((double) slot.compressor.releaseMs) << " "
                << detail::num((double) slot.compressor.makeUpDb) << " "
                << detail::num((double) slot.tremolo.rateHz) << " "
                << detail::num((double) slot.tremolo.depth) << " "
                << detail::num((double) slot.chorus.rateHz) << " "
                << detail::num((double) slot.chorus.depth) << " "
                << detail::num((double) slot.chorus.mix) << " "
                << detail::num((double) slot.wobble.rateBeats) << " "
                << detail::num((double) slot.wobble.depth) << " "
                << detail::num((double) slot.wobble.baseCutoffHz) << " "
                << detail::num((double) slot.wobble.resonance) << " "
                << detail::num((double) slot.wobble.mix) << " "
                << detail::num((double) slot.gate.thresholdDb) << " "
                << detail::num((double) slot.gate.rangeDb) << " "
                << detail::num((double) slot.gate.attackMs) << " "
                << detail::num((double) slot.gate.holdMs) << " "
                << detail::num((double) slot.gate.releaseMs) << " "
                // Appended rather than written next to the other drive fields:
                // the line is positional, so inserting mid-line would make
                // every older file read its own values into the wrong slots.
                << detail::num((double) slot.drive.asymmetry) << " "
                << (slot.drive.oversample ? 1 : 0) << " "
                << detail::num((double) slot.eqPedal.lowShelfHz) << " "
                << detail::num((double) slot.eqPedal.lowShelfDb) << " "
                << detail::num((double) slot.eqPedal.midHz) << " "
                << detail::num((double) slot.eqPedal.midDb) << " "
                << detail::num((double) slot.eqPedal.midQ) << " "
                << detail::num((double) slot.eqPedal.highShelfHz) << " "
                << detail::num((double) slot.eqPedal.highShelfDb) << " "
                // Appended for the same reason v30's fields were: the line is
                // positional, so anything inserted mid-line would make every
                // older file read its values into the wrong slots.
                << slot.compressor.sidechainTrackId
                << " " << slot.drive.stages
                << " " << (slot.drive.cabinetIr ? 1 : 0) << "\n";

            if (slot.kind == EffectKind::Plugin)
            {
                // Split across lines because identifier, name and state are all
                // free-form: each takes the rest of its own line rather than
                // needing escaping.
                out << "FXPLUGFMT " << (int) slot.plugin.format << "\n";
                out << "FXPLUGID " << slot.plugin.identifier << "\n";
                out << "FXPLUGNAME " << slot.plugin.name << "\n";
                out << "FXPLUGSTATE " << slot.plugin.state << "\n";
            }
        }

        // The session grid's column for this track. Slots are written by index
        // including the empty ones, since the index is the scene.
        out << "SESSION " << track.sessionSlots.size() << "\n";
        for (const auto& slot : track.sessionSlots)
        {
            out << "SSLOT " << (slot.hasClip ? 1 : 0) << "\n";
            if (slot.hasClip)
                detail::writeClip(out, slot.clip);
        }

        out << "CLIPS " << track.clips.size() << "\n";

        for (const auto& clip : track.clips)
            detail::writeClip(out, clip);
    }

    return out.str();
}

/** Reads a project. @p errorOut, if given, receives a short human-readable
    reason on failure (the caller shows it — see MainComponent::openProject);
    @p out is left untouched unless the whole parse succeeds. */
inline bool deserialize(const std::string& text, Song& out, std::string* errorOut = nullptr)
{
    auto fail = [&](const char* why)
    {
        if (errorOut != nullptr)
            *errorOut = why;
        return false;
    };

    // Buffered into lines with a cursor, rather than streamed, so a record can
    // be *offered* and declined without being consumed — which is what lets an
    // older file skip records added in later versions (see readTagged below).
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string        line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    size_t cursor = 0;

    /** Consumes the next line and returns its remainder *only* if it carries
        @p expectedTag; otherwise leaves the cursor alone and returns false.
        Required records treat false as an error; records added in a later
        format version simply let their defaults stand. */
    auto readTagged = [&](const char* expectedTag, std::string& rest) -> bool
    {
        if (cursor >= lines.size())
            return false;

        std::istringstream ls(lines[cursor]);
        std::string        tag;
        ls >> tag;
        if (tag != expectedTag)
            return false;

        std::getline(ls, rest);
        rest = detail::trimLeadingSpace(std::move(rest));
        ++cursor;
        return true;
    };

    std::string rest;

    /** One clip record, the mirror of detail::writeClip — used for both the
        arrangement's clips and the session grid's, so the two can't drift
        apart. Returns false if the record is missing or truncated. */
    auto readClip = [&](Clip& clip) -> bool
    {
        if (! readTagged("CLIP", rest))
            return false;
        {
            std::istringstream cs(rest);
            int typeInt = 0;
            cs >> clip.id >> typeInt >> clip.startBeats >> clip.lengthBeats >> clip.pattern.lengthBeats;
            clip.type = (ClipType) typeInt;
            std::string audio;
            std::getline(cs, audio);
            clip.audioFile = detail::trimLeadingSpace(std::move(audio));
        }

        // Optional: absent in files written before v28, where 0 dB is right.
        if (readTagged("CLIPGAIN", rest))
            clip.gainDb = (float) std::strtod(rest.c_str(), nullptr);

        // Optional: absent before v34, where "not warped, tempo unknown" is
        // exactly how those files already played.
        if (readTagged("CLIPWARP", rest))
        {
            std::istringstream ws(rest);
            int warp = 0;
            ws >> warp >> clip.sourceBpm;
            clip.warpEnabled = warp != 0;
        }

        // Optional: absent before v41, where a clip had no way to hold one.
        if (readTagged("PEDALS", rest))
        {
            const int pedalCount = std::atoi(rest.c_str());
            for (int i = 0; i < pedalCount; ++i)
            {
                if (! readTagged("PEDAL", rest))
                    return false;

                std::istringstream ps(rest);
                engine::PedalEvent pedal;
                int down = 0;
                ps >> pedal.beat >> down;
                pedal.down = down != 0;
                clip.pattern.pedals.push_back(pedal);
            }
        }

        if (! readTagged("NOTES", rest))
            return false;
        const int noteCount = std::atoi(rest.c_str());

        for (int k = 0; k < noteCount; ++k)
        {
            if (! readTagged("NOTE", rest))
                return false;
            std::istringstream ns(rest);
            engine::Note note;
            double velocity = 0.0;

            // Seeded with the default: a file written before v31 stops after
            // the velocity, the extraction fails, and every note keeps the
            // articulation it always had.
            int articulation = (int) engine::Articulation::Normal;

            ns >> note.startBeats >> note.lengthBeats >> note.noteNumber >> velocity
               >> articulation;

            note.velocity = (float) velocity;

            // Clamped rather than cast blindly: a newer file could carry an
            // articulation this build has never heard of, and playing such a
            // note normally is better than playing it as whatever that integer
            // happens to alias to.
            note.articulation = articulation == (int) engine::Articulation::PalmMute
                                    ? engine::Articulation::PalmMute
                                    : engine::Articulation::Normal;

            clip.pattern.notes.push_back(note);
        }
        return true;
    };

    if (! readTagged("LOOPER", rest))
        return fail("not a Looper project file");

    const int version = std::atoi(rest.c_str());
    if (version <= 0)
        return fail("unrecognised project format version");
    if (version > kFormatVersion)
        return fail("saved by a newer version of Looper-Audio");

    Song song;

    if (! readTagged("BPM", rest))    return fail("missing tempo"); song.bpm = std::strtod(rest.c_str(), nullptr);

    // Read only if present: a file written before v32 has no tempo changes,
    // which is exactly what one tempo for the whole song means.
    if (readTagged("TEMPOS", rest))
    {
        const int count = std::atoi(rest.c_str());
        for (int i = 0; i < count; ++i)
        {
            if (! readTagged("TEMPOAT", rest))
                return fail("truncated tempo map");

            std::istringstream ts(rest);
            engine::TempoChange change;

            // Seeded false: a v32 line stops after the tempo, and every change
            // written before ramps existed was a step.
            int ramp = 0;
            ts >> change.beat >> change.bpm >> ramp;
            change.ramp = ramp != 0;

            // Dropped rather than trusted: a zero or negative tempo divides by
            // zero deep inside playback, and beat 0 is BPM's job.
            if (change.bpm > 0.0 && change.beat > 0.0)
                song.tempoChanges.push_back(change);
        }
    }
    if (! readTagged("TSNUM", rest))  return fail("missing time signature"); song.timeSigNumerator = std::atoi(rest.c_str());
    if (! readTagged("TSDEN", rest))  return fail("missing time signature"); song.timeSigDenominator = std::atoi(rest.c_str());
    if (! readTagged("NEXTID", rest)) return fail("missing id counter"); song.nextId = std::atoi(rest.c_str());

    // Everything from here to TRACKS is read only if present: each of these
    // records joined the format at some point, so an older file just leaves
    // the corresponding defaults in place.
    if (readTagged("FILTER", rest))
    {
        std::istringstream fs(rest);
        int    enabled = 0, mode = 0;
        double cutoff = 0.0, resonance = 0.0;
        fs >> enabled >> mode >> cutoff >> resonance;
        song.filter.enabled   = enabled != 0;
        song.filter.mode      = mode;
        song.filter.cutoff    = (float) cutoff;
        song.filter.resonance = (float) resonance;
    }

    if (readTagged("DELAY", rest))
    {
        std::istringstream ds(rest);
        int    enabled = 0;
        double timeMs = 0.0, feedback = 0.0, mix = 0.0;
        ds >> enabled >> timeMs >> feedback >> mix;
        song.delay.enabled  = enabled != 0;
        song.delay.timeMs   = (float) timeMs;
        song.delay.feedback = (float) feedback;
        song.delay.mix      = (float) mix;
    }

    if (readTagged("REVERB", rest))
    {
        std::istringstream rs(rest);
        int    enabled = 0;
        double roomSize = 0.0, damping = 0.0, mix = 0.0;
        rs >> enabled >> roomSize >> damping >> mix;
        song.reverb.enabled  = enabled != 0;
        song.reverb.roomSize = (float) roomSize;
        song.reverb.damping  = (float) damping;
        song.reverb.mix      = (float) mix;
    }

    if (readTagged("SENDBUS", rest))
    {
        std::istringstream sb(rest);
        int    enabled = 0, effectType = 0;
        double roomSize = 0.0, damping = 0.0, delayTimeMs = 0.0, delayFeedback = 0.0, returnLevel = 0.0;
        sb >> enabled >> effectType >> roomSize >> damping >> delayTimeMs >> delayFeedback >> returnLevel;
        song.sendBus.enabled       = enabled != 0;
        song.sendBus.effectType    = (SendBusEffectType) effectType;
        song.sendBus.roomSize      = (float) roomSize;
        song.sendBus.damping       = (float) damping;
        song.sendBus.delayTimeMs   = (float) delayTimeMs;
        song.sendBus.delayFeedback = (float) delayFeedback;
        song.sendBus.returnLevel   = (float) returnLevel;
    }

    if (readTagged("EQ", rest)) // added in v25; older files keep the defaults (flat)
    {
        std::istringstream eq(rest);
        int    enabled = 0;
        double bassDb = 0.0, midDb = 0.0, trebleDb = 0.0;
        eq >> enabled >> bassDb >> midDb >> trebleDb;
        song.eq.enabled  = enabled != 0;
        song.eq.bassDb   = (float) bassDb;
        song.eq.midDb    = (float) midDb;
        song.eq.trebleDb = (float) trebleDb;
    }

    if (readTagged("MASTERING", rest)) // added in v29; older files keep the defaults (every stage a no-op)
    {
        std::istringstream ms(rest);
        auto&              m = song.mastering;

        // Pre-set to the struct's own defaults before extraction, so a
        // record truncated by a future/older writer leaves sane values
        // rather than zeros — a zero lowShelfHz or peakQ would be a broken
        // filter, not a neutral one.
        int    enabled = 0;
        double lowHz = m.lowShelfHz, lowDb = m.lowShelfDb;
        double peakHz = m.peakHz, peakDb = m.peakDb, peakQ = m.peakQ;
        double highHz = m.highShelfHz, highDb = m.highShelfDb;
        double excAmount = m.exciterAmount, excHz = m.exciterCrossoverHz;
        double width = m.width;
        double revAmount = m.reverbAmount, revRoom = m.reverbRoomSize;
        double maxIn = m.maximizerInputDb, maxCeil = m.maximizerCeilingDb, maxRel = m.maximizerReleaseMs;
        double outDb = m.outputGainDb;

        ms >> enabled >> lowHz >> lowDb >> peakHz >> peakDb >> peakQ >> highHz >> highDb
           >> excAmount >> excHz >> width >> revAmount >> revRoom
           >> maxIn >> maxCeil >> maxRel >> outDb;

        m.enabled            = enabled != 0;
        m.lowShelfHz         = (float) lowHz;
        m.lowShelfDb         = (float) lowDb;
        m.peakHz             = (float) peakHz;
        m.peakDb             = (float) peakDb;
        m.peakQ              = (float) peakQ;
        m.highShelfHz        = (float) highHz;
        m.highShelfDb        = (float) highDb;
        m.exciterAmount      = (float) excAmount;
        m.exciterCrossoverHz = (float) excHz;
        m.width              = (float) width;
        m.reverbAmount       = (float) revAmount;
        m.reverbRoomSize     = (float) revRoom;
        m.maximizerInputDb   = (float) maxIn;
        m.maximizerCeilingDb = (float) maxCeil;
        m.maximizerReleaseMs = (float) maxRel;
        m.outputGainDb       = (float) outDb;
    }

    if (readTagged("PROJECTROOT", rest))
        song.projectRootFolder = rest;

    if (readTagged("AUTO", rest))
    {
        const int pointCount = std::atoi(rest.c_str());
        song.masterGainDb.clear();
        for (int i = 0; i < pointCount; ++i)
        {
            // Once a count-prefixed record is present its points are not
            // optional — a short list means the file is damaged, not old.
            if (! readTagged("APT", rest)) return fail("truncated master automation");
            std::istringstream ps(rest);
            double beat = 0.0, value = 0.0;
            ps >> beat >> value;
            song.masterGainDb.addPoint(beat, (float) value);
        }
    }

    if (readTagged("SCENES", rest)) // added in v17; older files have no session grid
    {
        const int sceneCount = std::atoi(rest.c_str());
        for (int s = 0; s < sceneCount; ++s)
        {
            if (! readTagged("SCENE", rest)) return fail("truncated scene list");
            song.scenes.push_back(Scene { rest });
        }
    }

    if (! readTagged("TRACKS", rest)) return fail("missing track list");
    const int trackCount = std::atoi(rest.c_str());

    for (int i = 0; i < trackCount; ++i)
    {
        if (! readTagged("TRACK", rest))
            return fail("truncated track list");

        Track track;
        {
            std::istringstream ts(rest);
            int typeInt = 0, muteInt = 0, soloInt = 0;
            double gain = 0.0, sendLevel = 0.0;
            ts >> track.id >> typeInt >> gain >> muteInt >> soloInt >> sendLevel;
            track.type      = (TrackType) typeInt;
            track.gainDb    = (float) gain;
            track.muted     = muteInt != 0;
            track.solo      = soloInt != 0;
            track.sendLevel = (float) sendLevel;

            // Pan joined this record in v15, ahead of the rest-of-line name.
            // Like DPAD, the field count can't be used to detect it, so the
            // version decides.
            if (version >= 15)
            {
                double pan = 0.0;
                ts >> pan;
                track.pan = (float) pan;
            }

            // Colour joined in v22, also ahead of the name. As with pan, the
            // field count can't tell — a name beginning with digits would be
            // read as one — so the version decides.
            if (version >= 22)
            {
                unsigned int colour = 0;
                ts >> colour;
                track.colour = colour;
            }

            std::string name;
            std::getline(ts, name);
            track.name = detail::trimLeadingSpace(std::move(name));
        }

        // Optional: absent before v36, where every track fed the master.
        if (readTagged("TRACKBUS", rest))
            track.outputBusId = std::atoi(rest.c_str());

        // Before v16 a track had exactly one lane, always gain, written as a
        // bare TAUTO point list. Read it straight into the Gain lane so an
        // older project keeps its automation rather than silently losing it.
        if (readTagged("TAUTO", rest))
        {
            const int pointCount = std::atoi(rest.c_str());
            auto&     gainLane   = track.laneFor(TrackParam::Gain);
            for (int p = 0; p < pointCount; ++p)
            {
                if (! readTagged("TAPT", rest)) return fail("truncated track automation");
                std::istringstream ps(rest);
                double beat = 0.0, value = 0.0;
                ps >> beat >> value;
                gainLane.addPoint(beat, (float) value);
            }
        }
        else if (readTagged("TAUTOS", rest))
        {
            const int laneCount = std::atoi(rest.c_str());
            for (int l = 0; l < laneCount; ++l)
            {
                if (! readTagged("TLANE", rest)) return fail("truncated automation lane list");
                std::istringstream ls(rest);
                int paramId = 0, pointCount = 0;
                ls >> paramId >> pointCount;

                auto& lane = track.automation[paramId];
                for (int p = 0; p < pointCount; ++p)
                {
                    if (! readTagged("TAPT", rest)) return fail("truncated track automation");
                    std::istringstream ps(rest);
                    double beat = 0.0, value = 0.0;
                    ps >> beat >> value;
                    lane.addPoint(beat, (float) value);
                }
            }
        }

        if (readTagged("DRUMKIT", rest))
        {
            const int padCount = std::atoi(rest.c_str());
            for (int p = 0; p < padCount; ++p)
            {
                if (! readTagged("DPAD", rest)) return fail("truncated drum kit");
                std::istringstream ps(rest);
                DrumPad pad;
                ps >> pad.noteNumber >> pad.label;

                // The one record whose *shape* changed rather than being
                // added wholesale: before v13 a pad was just note/label/path,
                // and the mix fields didn't exist. They can't be detected by
                // token count because the path is rest-of-line and may
                // contain spaces, so the version decides.
                if (version >= 13)
                {
                    double gainDb = 0.0, pan = 0.0, pitchSemitones = 0.0;
                    int    muted = 0, solo = 0;
                    ps >> gainDb >> pan >> pitchSemitones >> muted >> solo;
                    pad.gainDb         = (float) gainDb;
                    pad.pan            = (float) pan;
                    pad.pitchSemitones = (float) pitchSemitones;
                    pad.muted          = muted != 0;
                    pad.solo           = solo != 0;
                }

                std::string samplePath;
                std::getline(ps, samplePath);
                pad.samplePath = detail::trimLeadingSpace(std::move(samplePath));
                track.drumKit.pads.push_back(pad);
            }
        }

        if (readTagged("SYNTH", rest)) // added in v12; older files keep the defaults
        {
            std::istringstream ss(rest);
            int    waveform = 0, filterEnabled = 0, filterMode = 0;
            double attackMs = 0.0, decayMs = 0.0, sustain = 0.0, releaseMs = 0.0;
            double filterCutoff = 0.0, filterResonance = 0.0, gainDb = 0.0;
            // Pre-set to model::SynthSettings' real defaults (not 0), since a
            // file written before v26 has no tokens for these at all - the
            // stream simply stops filling them in, same tolerant-read
            // mechanism the whole SYNTH line already relies on.
            double filterEnvAmount = 0.0, filterEnvAttackMs = 0.0, filterEnvDecayMs = 0.0;
            double filterEnvSustain = 1.0, filterEnvReleaseMs = 0.0;
            int    subOscEnabled = 0;
            double subOscLevel = 0.3;
            int    unisonVoices = 1;
            double unisonDetuneCents = 12.0;
            ss >> waveform >> attackMs >> decayMs >> sustain >> releaseMs
               >> filterEnabled >> filterMode >> filterCutoff >> filterResonance >> gainDb
               >> filterEnvAmount >> filterEnvAttackMs >> filterEnvDecayMs >> filterEnvSustain >> filterEnvReleaseMs
               >> subOscEnabled >> subOscLevel >> unisonVoices >> unisonDetuneCents;
            track.synthSettings.waveform        = waveform;
            track.synthSettings.attackMs        = (float) attackMs;
            track.synthSettings.decayMs         = (float) decayMs;
            track.synthSettings.sustain         = (float) sustain;
            track.synthSettings.releaseMs       = (float) releaseMs;
            track.synthSettings.filterEnabled   = filterEnabled != 0;
            track.synthSettings.filterMode      = filterMode;
            track.synthSettings.filterCutoff    = (float) filterCutoff;
            track.synthSettings.filterResonance = (float) filterResonance;
            track.synthSettings.gainDb          = (float) gainDb;
            track.synthSettings.filterEnvAmount    = (float) filterEnvAmount;
            track.synthSettings.filterEnvAttackMs  = (float) filterEnvAttackMs;
            track.synthSettings.filterEnvDecayMs   = (float) filterEnvDecayMs;
            track.synthSettings.filterEnvSustain   = (float) filterEnvSustain;
            track.synthSettings.filterEnvReleaseMs = (float) filterEnvReleaseMs;
            track.synthSettings.subOscEnabled      = subOscEnabled != 0;
            track.synthSettings.subOscLevel        = (float) subOscLevel;
            track.synthSettings.unisonVoices       = unisonVoices;
            track.synthSettings.unisonDetuneCents  = (float) unisonDetuneCents;
        }

        if (readTagged("GUITAR", rest)) // added in v19; older files keep the defaults
        {
            std::istringstream gs(rest);
            for (int s = 0; s < kNumGuitarStrings; ++s)
                gs >> track.guitarSettings.tuning[(size_t) s];

            double decay = 0.0, brightness = 0.0, position = 0.0, hardness = 0.0, mute = 0.0;

            // Seeded with the defaults rather than 0, so a v29-or-older GUITAR
            // line - which ends after `mute` - leaves a sensible pickup rather
            // than a 0Hz one. Added in v30.
            const GuitarSettings fallback;
            double resonanceHz = (double) fallback.pickupResonanceHz;
            double pickupQ     = (double) fallback.pickupQ;
            double palmDecay   = (double) fallback.palmMuteDecaySeconds;
            double palmBright  = (double) fallback.palmMuteBrightness;

            // Seeded from the defaults for the same reason, and this time it
            // decides something audible: a v36-or-older GUITAR line ends after
            // palmBright, and reading 0 into these would give an older project
            // the *old* instrument - identical notes, no sympathetic ringing,
            // dead centre - rather than the improved one. Added in v37.
            double velocitySense = (double) fallback.velocitySensitivity;
            double coupling      = (double) fallback.stringCoupling;
            double width         = (double) fallback.stereoWidth;
            double stiffness     = (double) fallback.stiffness;

            gs >> decay >> brightness >> position >> hardness >> mute
               >> resonanceHz >> pickupQ >> palmDecay >> palmBright
               >> velocitySense >> coupling >> width >> stiffness;

            track.guitarSettings.stiffness = (float) stiffness;

            track.guitarSettings.velocitySensitivity = (float) velocitySense;
            track.guitarSettings.stringCoupling      = (float) coupling;
            track.guitarSettings.stereoWidth         = (float) width;
            track.guitarSettings.decaySeconds      = (float) decay;
            track.guitarSettings.brightness        = (float) brightness;
            track.guitarSettings.pickPosition      = (float) position;
            track.guitarSettings.pickHardness      = (float) hardness;
            track.guitarSettings.muteOnNoteOff     = (float) mute;
            track.guitarSettings.pickupResonanceHz = (float) resonanceHz;
            track.guitarSettings.pickupQ           = (float) pickupQ;
            track.guitarSettings.palmMuteDecaySeconds = (float) palmDecay;
            track.guitarSettings.palmMuteBrightness   = (float) palmBright;
        }

        // v14..v17 stored a fixed filter/delay/reverb trio. Migrate it into
        // three chain slots in that same order, so an old project comes back
        // with its effects in the order it had them and sounding the same.
        if (readTagged("TFX", rest))
        {
            std::istringstream fs(rest);
            int    filterOn = 0, filterMode = 0, delayOn = 0, reverbOn = 0;
            double cutoff = 0.0, resonance = 0.0;
            double delayTime = 0.0, delayFeedback = 0.0, delayMix = 0.0;
            double room = 0.0, damping = 0.0, reverbMix = 0.0;
            fs >> filterOn >> filterMode >> cutoff >> resonance
               >> delayOn >> delayTime >> delayFeedback >> delayMix
               >> reverbOn >> room >> damping >> reverbMix;

            EffectSlot filterSlot;
            filterSlot.kind             = EffectKind::Filter;
            filterSlot.enabled          = filterOn != 0;
            filterSlot.filter.enabled   = filterOn != 0;
            filterSlot.filter.mode      = filterMode;
            filterSlot.filter.cutoff    = (float) cutoff;
            filterSlot.filter.resonance = (float) resonance;

            EffectSlot delaySlot;
            delaySlot.kind           = EffectKind::Delay;
            delaySlot.enabled        = delayOn != 0;
            delaySlot.delay.enabled  = delayOn != 0;
            delaySlot.delay.timeMs   = (float) delayTime;
            delaySlot.delay.feedback = (float) delayFeedback;
            delaySlot.delay.mix      = (float) delayMix;

            EffectSlot reverbSlot;
            reverbSlot.kind            = EffectKind::Reverb;
            reverbSlot.enabled         = reverbOn != 0;
            reverbSlot.reverb.enabled  = reverbOn != 0;
            reverbSlot.reverb.roomSize = (float) room;
            reverbSlot.reverb.damping  = (float) damping;
            reverbSlot.reverb.mix      = (float) reverbMix;

            track.effectChain.push_back(filterSlot);
            track.effectChain.push_back(delaySlot);
            track.effectChain.push_back(reverbSlot);
        }
        else if (readTagged("FXCHAIN", rest)) // v18 onward
        {
            const int slotCount = std::atoi(rest.c_str());
            for (int s = 0; s < slotCount; ++s)
            {
                if (! readTagged("FXSLOT", rest)) return fail("truncated effect chain");

                std::istringstream ss(rest);
                int    kind = 0, enabled = 0, filterMode = 0;
                double cutoff = 0.0, resonance = 0.0;
                double delayTime = 0.0, delayFeedback = 0.0, delayMix = 0.0;
                double room = 0.0, damping = 0.0, reverbMix = 0.0;

                // Defaults matter: a file written before version 20 has no
                // drive fields, the extractions below simply fail, and these
                // values are what the slot keeps.
                double driveAmount = 4.0, driveTone = 0.5, driveLevel = 0.7;
                int    driveHard = 0, driveCab = 1;
                double compThreshold = -18.0, compRatio = 4.0, compAttack = 10.0;
                double compRelease = 120.0, compMakeUp = 0.0;
                double tremRate = 5.0, tremDepth = 0.5;
                double chorusRate = 0.6, chorusDepth = 0.5, chorusMix = 0.5;
                double wobbleRateBeats = 0.25, wobbleDepth = 0.7, wobbleBaseCutoffHz = 200.0;
                double wobbleResonance = 0.9, wobbleMix = 1.0;
                double gateThreshold = -40.0, gateRange = 60.0, gateAttack = 2.0;
                double gateHold = 20.0, gateRelease = 150.0;
                double driveAsymmetry = 0.0;
                int    driveOversample = 0;
                double eqLowHz = 100.0, eqLowDb = 0.0;
                double eqMidHz = 800.0, eqMidDb = 0.0, eqMidQ = 1.0;
                double eqHighHz = 4000.0, eqHighDb = 0.0;
                int    compSidechainTrackId = -1; // absent before v35: no sidechain
                int    driveStages          = 1;  // absent before v39: one clipper
                int    driveCabinetIr       = 0;  // absent before v40: filtered cabinet

                ss >> kind >> enabled >> filterMode >> cutoff >> resonance
                   >> delayTime >> delayFeedback >> delayMix >> room >> damping >> reverbMix
                   >> driveAmount >> driveTone >> driveLevel >> driveHard >> driveCab
                   >> compThreshold >> compRatio >> compAttack >> compRelease >> compMakeUp
                   >> tremRate >> tremDepth
                   >> chorusRate >> chorusDepth >> chorusMix
                   >> wobbleRateBeats >> wobbleDepth >> wobbleBaseCutoffHz >> wobbleResonance >> wobbleMix
                   >> gateThreshold >> gateRange >> gateAttack >> gateHold >> gateRelease
                   >> driveAsymmetry >> driveOversample
                   >> eqLowHz >> eqLowDb >> eqMidHz >> eqMidDb >> eqMidQ >> eqHighHz >> eqHighDb
                   >> compSidechainTrackId >> driveStages >> driveCabinetIr;

                EffectSlot slot;
                slot.kind              = (EffectKind) kind;
                slot.enabled           = enabled != 0;
                slot.filter.enabled    = slot.enabled && slot.kind == EffectKind::Filter;
                slot.filter.mode       = filterMode;
                slot.filter.cutoff     = (float) cutoff;
                slot.filter.resonance  = (float) resonance;
                slot.delay.enabled     = slot.enabled && slot.kind == EffectKind::Delay;
                slot.delay.timeMs      = (float) delayTime;
                slot.delay.feedback    = (float) delayFeedback;
                slot.delay.mix         = (float) delayMix;
                slot.reverb.enabled    = slot.enabled && slot.kind == EffectKind::Reverb;
                slot.reverb.roomSize   = (float) room;
                slot.reverb.damping    = (float) damping;
                slot.reverb.mix        = (float) reverbMix;
                slot.drive.enabled     = slot.enabled && slot.kind == EffectKind::Drive;
                slot.drive.drive       = (float) driveAmount;
                slot.drive.tone        = (float) driveTone;
                slot.drive.level       = (float) driveLevel;
                slot.drive.hardClip    = driveHard != 0;
                slot.drive.cabinet     = driveCab != 0;
                slot.drive.asymmetry   = (float) driveAsymmetry;
                slot.drive.oversample  = driveOversample != 0;
                slot.drive.stages      = driveStages > 0 ? driveStages : 1;
                slot.drive.cabinetIr   = driveCabinetIr != 0;
                slot.compressor.enabled     = slot.enabled && slot.kind == EffectKind::Compressor;
                slot.compressor.thresholdDb = (float) compThreshold;
                slot.compressor.ratio       = (float) compRatio;
                slot.compressor.attackMs    = (float) compAttack;
                slot.compressor.releaseMs   = (float) compRelease;
                slot.compressor.makeUpDb    = (float) compMakeUp;
                slot.compressor.sidechainTrackId = compSidechainTrackId;
                slot.tremolo.enabled        = slot.enabled && slot.kind == EffectKind::Tremolo;
                slot.tremolo.rateHz         = (float) tremRate;
                slot.tremolo.depth          = (float) tremDepth;
                slot.chorus.enabled         = slot.enabled && slot.kind == EffectKind::Chorus;
                slot.chorus.rateHz          = (float) chorusRate;
                slot.chorus.depth           = (float) chorusDepth;
                slot.chorus.mix             = (float) chorusMix;
                slot.wobble.enabled         = slot.enabled && slot.kind == EffectKind::Wobble;
                slot.wobble.rateBeats       = (float) wobbleRateBeats;
                slot.wobble.depth           = (float) wobbleDepth;
                slot.wobble.baseCutoffHz    = (float) wobbleBaseCutoffHz;
                slot.wobble.resonance       = (float) wobbleResonance;
                slot.wobble.mix             = (float) wobbleMix;
                slot.eqPedal.enabled     = slot.enabled && slot.kind == EffectKind::Eq;
                slot.eqPedal.lowShelfHz  = (float) eqLowHz;
                slot.eqPedal.lowShelfDb  = (float) eqLowDb;
                slot.eqPedal.midHz       = (float) eqMidHz;
                slot.eqPedal.midDb       = (float) eqMidDb;
                slot.eqPedal.midQ        = (float) eqMidQ;
                slot.eqPedal.highShelfHz = (float) eqHighHz;
                slot.eqPedal.highShelfDb = (float) eqHighDb;
                slot.gate.enabled           = slot.enabled && slot.kind == EffectKind::Gate;
                slot.gate.thresholdDb       = (float) gateThreshold;
                slot.gate.rangeDb           = (float) gateRange;
                slot.gate.attackMs          = (float) gateAttack;
                slot.gate.holdMs            = (float) gateHold;
                slot.gate.releaseMs         = (float) gateRelease;

                if (slot.kind == EffectKind::Plugin)
                {
                    if (! readTagged("FXPLUGFMT", rest))   return fail("truncated plugin slot");
                    slot.plugin.format = (PluginFormat) std::atoi(rest.c_str());
                    if (! readTagged("FXPLUGID", rest))    return fail("truncated plugin slot");
                    slot.plugin.identifier = rest;
                    if (! readTagged("FXPLUGNAME", rest))  return fail("truncated plugin slot");
                    slot.plugin.name = rest;
                    if (! readTagged("FXPLUGSTATE", rest)) return fail("truncated plugin slot");
                    slot.plugin.state = rest;
                }

                track.effectChain.push_back(std::move(slot));
            }
        }

        if (readTagged("SESSION", rest)) // added in v17
        {
            const int slotCount = std::atoi(rest.c_str());
            for (int s = 0; s < slotCount; ++s)
            {
                if (! readTagged("SSLOT", rest)) return fail("truncated session grid");

                SessionSlot slot;
                slot.hasClip = std::atoi(rest.c_str()) != 0;
                if (slot.hasClip && ! readClip(slot.clip))
                    return fail("truncated session clip");
                track.sessionSlots.push_back(std::move(slot));
            }
        }

        if (! readTagged("CLIPS", rest))
            return fail("missing clip list");
        const int clipCount = std::atoi(rest.c_str());

        for (int j = 0; j < clipCount; ++j)
        {
            Clip clip;
            if (! readClip(clip))
                return fail("truncated clip list");
            track.clips.push_back(std::move(clip));
        }

        song.tracks.push_back(std::move(track));
    }

    out = std::move(song);
    return true;
}

} // namespace looper::model
