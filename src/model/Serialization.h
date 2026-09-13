#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    The SoundSplice project format: a small, line-based text format for the
    project document. Deliberately JUCE-free so the round-trip can be
    unit-tested headless.

    Layout is flat and count-prefixed so it parses deterministically. Numbers use
    %.17g (exact IEEE double round-trip); string fields (track name, audio file,
    scene name, plugin fields) are the rest of their line, so they may contain
    spaces — which is why a value that belongs with such a record lives in a
    record of its own after it (CLIPGAIN after CLIP, for instance).

    **Versioning.** A file starts with "SOUNDSPLICE <version>", and `serialize`
    always writes kFormatVersion. `deserialize` reads optional records only if
    present, so a record added later leaves its fields at their struct defaults
    when an earlier file lacks it; positional fields are only ever appended to
    the end of their line, for the same reason. A file written by a *newer*
    build is refused outright rather than part-parsed: silently dropping records
    the user can't see would be worse than declining to open it.

    Looper-Audio's ".looper" files are not read.
*/
inline constexpr int kFormatVersion = 1;

namespace detail
{
    inline std::string num(double v)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return buffer;
    }

    /** A stored fade shape, or Linear for a value this build doesn't know —
        a shape added later should play as a plain fade here, not refuse the
        whole file. */
    inline engine::FadeShape fadeShapeFrom(int value)
    {
        switch (value)
        {
            case (int) engine::FadeShape::EqualPower: return engine::FadeShape::EqualPower;
            case (int) engine::FadeShape::SCurve:     return engine::FadeShape::SCurve;
            default:                                  return engine::FadeShape::Linear;
        }
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
        // follow it on that line.
        out << "CLIPGAIN " << num((double) clip.gainDb) << "\n";
        out << "CLIPSRC " << num(clip.sourceOffsetSeconds) << "\n";
        out << "CLIPFADE " << num(clip.fades.inSeconds) << " " << (int) clip.fades.inShape << " "
            << num(clip.fades.outSeconds) << " " << (int) clip.fades.outShape << "\n";
        out << "PEDALS " << clip.pattern.pedals.size() << "\n";
        for (const auto& pedal : clip.pattern.pedals)
            out << "PEDAL " << num(pedal.beat) << " " << (pedal.down ? 1 : 0) << "\n";

        out << "NOTES " << clip.pattern.notes.size() << "\n";

        for (const auto& note : clip.pattern.notes)
            out << "NOTE " << num(note.startBeats) << " " << num(note.lengthBeats)
                << " " << note.noteNumber << " " << num((double) note.velocity) << "\n";
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
    out << "SOUNDSPLICE " << kFormatVersion << "\n";
    out << "BPM " << detail::num(song.bpm) << "\n";
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
            << " " << (track.solo ? 1 : 0)
            << " " << detail::num((double) track.pan)
            << " " << track.colour
            << " " << track.name << "\n";

        // Only non-empty lanes are written, so an unautomated track costs one
        // "TAUTOS 0" line rather than one empty record per automatable
        // parameter.
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
                << detail::num((double) slot.drive.asymmetry) << " "
                << (slot.drive.oversample ? 1 : 0) << " "
                << slot.drive.stages << " "
                << (slot.drive.cabinetIr ? 1 : 0) << " "
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
                << detail::num((double) slot.eqPedal.lowShelfHz) << " "
                << detail::num((double) slot.eqPedal.lowShelfDb) << " "
                << detail::num((double) slot.eqPedal.midHz) << " "
                << detail::num((double) slot.eqPedal.midDb) << " "
                << detail::num((double) slot.eqPedal.midQ) << " "
                << detail::num((double) slot.eqPedal.highShelfHz) << " "
                << detail::num((double) slot.eqPedal.highShelfDb) << "\n";

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
    // optional record be absent (see readTagged below).
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
        Required records treat false as an error; optional ones simply let
        their defaults stand. */
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
            clip.type = typeInt == (int) ClipType::Audio ? ClipType::Audio : ClipType::Instrument;
            std::string audio;
            std::getline(cs, audio);
            clip.audioFile = detail::trimLeadingSpace(std::move(audio));
        }

        if (readTagged("CLIPGAIN", rest))
            clip.gainDb = (float) std::strtod(rest.c_str(), nullptr);

        if (readTagged("CLIPSRC", rest))
            clip.sourceOffsetSeconds = std::strtod(rest.c_str(), nullptr);

        if (readTagged("CLIPFADE", rest))
        {
            std::istringstream fs(rest);
            int inShape = 0, outShape = 0;
            fs >> clip.fades.inSeconds >> inShape >> clip.fades.outSeconds >> outShape;
            clip.fades.inShape  = detail::fadeShapeFrom(inShape);
            clip.fades.outShape = detail::fadeShapeFrom(outShape);
        }

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
            ns >> note.startBeats >> note.lengthBeats >> note.noteNumber >> velocity;
            note.velocity = (float) velocity;

            clip.pattern.notes.push_back(note);
        }
        return true;
    };

    if (! readTagged("SOUNDSPLICE", rest))
        return fail("not a SoundSplice project file");

    const int version = std::atoi(rest.c_str());
    if (version <= 0)
        return fail("unrecognised project format version");
    if (version > kFormatVersion)
        return fail("saved by a newer version of SoundSplice");

    Song song;

    if (! readTagged("BPM", rest))    return fail("missing tempo"); song.bpm = std::strtod(rest.c_str(), nullptr);
    if (! readTagged("TSNUM", rest))  return fail("missing time signature"); song.timeSigNumerator = std::atoi(rest.c_str());
    if (! readTagged("TSDEN", rest))  return fail("missing time signature"); song.timeSigDenominator = std::atoi(rest.c_str());
    if (! readTagged("NEXTID", rest)) return fail("missing id counter"); song.nextId = std::atoi(rest.c_str());

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

    if (readTagged("EQ", rest))
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

    if (readTagged("MASTERING", rest))
    {
        std::istringstream ms(rest);
        auto&              m = song.mastering;

        // Pre-set to the struct's own defaults before extraction, so a
        // truncated record leaves sane values rather than zeros — a zero
        // lowShelfHz or peakQ would be a broken filter, not a neutral one.
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
            // optional — a short list means the file is damaged.
            if (! readTagged("APT", rest)) return fail("truncated master automation");
            std::istringstream ps(rest);
            double beat = 0.0, value = 0.0;
            ps >> beat >> value;
            song.masterGainDb.addPoint(beat, (float) value);
        }
    }

    if (readTagged("SCENES", rest))
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
            int          typeInt = 0, muteInt = 0, soloInt = 0;
            double       gain = 0.0, pan = 0.0;
            unsigned int colour = 0;
            ts >> track.id >> typeInt >> gain >> muteInt >> soloInt >> pan >> colour;

            if (typeInt != (int) TrackType::Instrument && typeInt != (int) TrackType::Audio)
                return fail("unknown track type");

            track.type   = (TrackType) typeInt;
            track.gainDb = (float) gain;
            track.muted  = muteInt != 0;
            track.solo   = soloInt != 0;
            track.pan    = (float) pan;
            track.colour = colour;

            std::string name;
            std::getline(ts, name);
            track.name = detail::trimLeadingSpace(std::move(name));
        }

        if (readTagged("TAUTOS", rest))
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

        if (readTagged("SYNTH", rest))
        {
            std::istringstream ss(rest);
            const SynthSettings defaults;
            int    waveform = defaults.waveform, filterEnabled = defaults.filterEnabled ? 1 : 0;
            int    filterMode = defaults.filterMode;
            double attackMs = defaults.attackMs, decayMs = defaults.decayMs;
            double sustain = defaults.sustain, releaseMs = defaults.releaseMs;
            double filterCutoff = defaults.filterCutoff, filterResonance = defaults.filterResonance;
            double gainDb = defaults.gainDb;
            double filterEnvAmount = defaults.filterEnvAmount, filterEnvAttackMs = defaults.filterEnvAttackMs;
            double filterEnvDecayMs = defaults.filterEnvDecayMs, filterEnvSustain = defaults.filterEnvSustain;
            double filterEnvReleaseMs = defaults.filterEnvReleaseMs;
            int    subOscEnabled = defaults.subOscEnabled ? 1 : 0;
            double subOscLevel = defaults.subOscLevel;
            int    unisonVoices = defaults.unisonVoices;
            double unisonDetuneCents = defaults.unisonDetuneCents;
            ss >> waveform >> attackMs >> decayMs >> sustain >> releaseMs
               >> filterEnabled >> filterMode >> filterCutoff >> filterResonance >> gainDb
               >> filterEnvAmount >> filterEnvAttackMs >> filterEnvDecayMs >> filterEnvSustain >> filterEnvReleaseMs
               >> subOscEnabled >> subOscLevel >> unisonVoices >> unisonDetuneCents;
            track.synthSettings.waveform           = waveform;
            track.synthSettings.attackMs           = (float) attackMs;
            track.synthSettings.decayMs            = (float) decayMs;
            track.synthSettings.sustain            = (float) sustain;
            track.synthSettings.releaseMs          = (float) releaseMs;
            track.synthSettings.filterEnabled      = filterEnabled != 0;
            track.synthSettings.filterMode         = filterMode;
            track.synthSettings.filterCutoff       = (float) filterCutoff;
            track.synthSettings.filterResonance    = (float) filterResonance;
            track.synthSettings.gainDb             = (float) gainDb;
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

        if (readTagged("FXCHAIN", rest))
        {
            const int slotCount = std::atoi(rest.c_str());
            for (int s = 0; s < slotCount; ++s)
            {
                if (! readTagged("FXSLOT", rest)) return fail("truncated effect chain");

                std::istringstream ss(rest);
                const EffectSlot defaults;
                EffectSlot slot;

                int    kind = 0, enabled = 0;
                int    filterMode = defaults.filter.mode;
                double cutoff = defaults.filter.cutoff, resonance = defaults.filter.resonance;
                double delayTime = defaults.delay.timeMs, delayFeedback = defaults.delay.feedback;
                double delayMix = defaults.delay.mix;
                double room = defaults.reverb.roomSize, damping = defaults.reverb.damping;
                double reverbMix = defaults.reverb.mix;
                double driveAmount = defaults.drive.drive, driveTone = defaults.drive.tone;
                double driveLevel = defaults.drive.level;
                int    driveHard = defaults.drive.hardClip ? 1 : 0, driveCab = defaults.drive.cabinet ? 1 : 0;
                double driveAsymmetry = defaults.drive.asymmetry;
                int    driveOversample = defaults.drive.oversample ? 1 : 0;
                int    driveStages = defaults.drive.stages;
                int    driveCabinetIr = defaults.drive.cabinetIr ? 1 : 0;
                double compThreshold = defaults.compressor.thresholdDb, compRatio = defaults.compressor.ratio;
                double compAttack = defaults.compressor.attackMs, compRelease = defaults.compressor.releaseMs;
                double compMakeUp = defaults.compressor.makeUpDb;
                double tremRate = defaults.tremolo.rateHz, tremDepth = defaults.tremolo.depth;
                double chorusRate = defaults.chorus.rateHz, chorusDepth = defaults.chorus.depth;
                double chorusMix = defaults.chorus.mix;
                double wobbleRateBeats = defaults.wobble.rateBeats, wobbleDepth = defaults.wobble.depth;
                double wobbleBaseCutoffHz = defaults.wobble.baseCutoffHz;
                double wobbleResonance = defaults.wobble.resonance, wobbleMix = defaults.wobble.mix;
                double gateThreshold = defaults.gate.thresholdDb, gateRange = defaults.gate.rangeDb;
                double gateAttack = defaults.gate.attackMs, gateHold = defaults.gate.holdMs;
                double gateRelease = defaults.gate.releaseMs;
                double eqLowHz = defaults.eqPedal.lowShelfHz, eqLowDb = defaults.eqPedal.lowShelfDb;
                double eqMidHz = defaults.eqPedal.midHz, eqMidDb = defaults.eqPedal.midDb;
                double eqMidQ = defaults.eqPedal.midQ;
                double eqHighHz = defaults.eqPedal.highShelfHz, eqHighDb = defaults.eqPedal.highShelfDb;

                ss >> kind >> enabled >> filterMode >> cutoff >> resonance
                   >> delayTime >> delayFeedback >> delayMix >> room >> damping >> reverbMix
                   >> driveAmount >> driveTone >> driveLevel >> driveHard >> driveCab
                   >> driveAsymmetry >> driveOversample >> driveStages >> driveCabinetIr
                   >> compThreshold >> compRatio >> compAttack >> compRelease >> compMakeUp
                   >> tremRate >> tremDepth
                   >> chorusRate >> chorusDepth >> chorusMix
                   >> wobbleRateBeats >> wobbleDepth >> wobbleBaseCutoffHz >> wobbleResonance >> wobbleMix
                   >> gateThreshold >> gateRange >> gateAttack >> gateHold >> gateRelease
                   >> eqLowHz >> eqLowDb >> eqMidHz >> eqMidDb >> eqMidQ >> eqHighHz >> eqHighDb;

                slot.kind                   = (EffectKind) kind;
                slot.enabled                = enabled != 0;
                slot.filter.enabled         = slot.enabled && slot.kind == EffectKind::Filter;
                slot.filter.mode            = filterMode;
                slot.filter.cutoff          = (float) cutoff;
                slot.filter.resonance       = (float) resonance;
                slot.delay.enabled          = slot.enabled && slot.kind == EffectKind::Delay;
                slot.delay.timeMs           = (float) delayTime;
                slot.delay.feedback         = (float) delayFeedback;
                slot.delay.mix              = (float) delayMix;
                slot.reverb.enabled         = slot.enabled && slot.kind == EffectKind::Reverb;
                slot.reverb.roomSize        = (float) room;
                slot.reverb.damping         = (float) damping;
                slot.reverb.mix             = (float) reverbMix;
                slot.drive.enabled          = slot.enabled && slot.kind == EffectKind::Drive;
                slot.drive.drive            = (float) driveAmount;
                slot.drive.tone             = (float) driveTone;
                slot.drive.level            = (float) driveLevel;
                slot.drive.hardClip         = driveHard != 0;
                slot.drive.cabinet          = driveCab != 0;
                slot.drive.asymmetry        = (float) driveAsymmetry;
                slot.drive.oversample       = driveOversample != 0;
                slot.drive.stages           = driveStages > 0 ? driveStages : 1;
                slot.drive.cabinetIr        = driveCabinetIr != 0;
                slot.compressor.enabled     = slot.enabled && slot.kind == EffectKind::Compressor;
                slot.compressor.thresholdDb = (float) compThreshold;
                slot.compressor.ratio       = (float) compRatio;
                slot.compressor.attackMs    = (float) compAttack;
                slot.compressor.releaseMs   = (float) compRelease;
                slot.compressor.makeUpDb    = (float) compMakeUp;
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
                slot.gate.enabled           = slot.enabled && slot.kind == EffectKind::Gate;
                slot.gate.thresholdDb       = (float) gateThreshold;
                slot.gate.rangeDb           = (float) gateRange;
                slot.gate.attackMs          = (float) gateAttack;
                slot.gate.holdMs            = (float) gateHold;
                slot.gate.releaseMs         = (float) gateRelease;
                slot.eqPedal.enabled        = slot.enabled && slot.kind == EffectKind::Eq;
                slot.eqPedal.lowShelfHz     = (float) eqLowHz;
                slot.eqPedal.lowShelfDb     = (float) eqLowDb;
                slot.eqPedal.midHz          = (float) eqMidHz;
                slot.eqPedal.midDb          = (float) eqMidDb;
                slot.eqPedal.midQ           = (float) eqMidQ;
                slot.eqPedal.highShelfHz    = (float) eqHighHz;
                slot.eqPedal.highShelfDb    = (float) eqHighDb;

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

        if (readTagged("SESSION", rest))
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

} // namespace soundsplice::model
