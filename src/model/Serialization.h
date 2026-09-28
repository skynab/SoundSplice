#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "model/EffectParams.h"
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

    Effect settings are the exception to positional fields: FXPARAMS names
    each one by its descriptor ids (model/EffectParams.h), so a new effect or
    parameter needs nothing here. Version 1 files held them positionally on
    the FXSLOT line, and are still read (detail::kPositionalEffectParams).

    Looper-Audio's ".looper" files are not read.
*/
inline constexpr int kFormatVersion = 2;

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
            case (int) engine::FadeShape::Exponential: return engine::FadeShape::Exponential;
            case (int) engine::FadeShape::Logarithmic: return engine::FadeShape::Logarithmic;
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
        out << "CLIPCHANS " << (int) clip.channels << "\n";
        if (clip.autoFadeIn || clip.autoFadeOut)
            out << "CLIPAUTOXF " << (clip.autoFadeIn ? 1 : 0) << " " << (clip.autoFadeOut ? 1 : 0) << "\n";

        // Only when there's a curve: a clip without one reads back as unity.
        if (! clip.envelope.isEmpty())
        {
            out << "CLIPENV " << clip.envelope.points().size();
            for (const auto& point : clip.envelope.points())
                out << " " << num(point.seconds) << " " << num((double) point.gain);
            out << "\n";
        }

        // Likewise only when there are some.
        if (! clip.spectralEdits.empty())
        {
            out << "CLIPSPEC " << clip.spectralEdits.size();
            for (const auto& region : clip.spectralEdits)
                out << " " << num(region.startSeconds) << " " << num(region.endSeconds) << " " << num(region.lowHz)
                    << " " << num(region.highHz) << " " << num((double) region.gainDb);
            out << "\n";
        }
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

    /** One automation point record: beat, value, and the shape of the
        segment it starts when that isn't a straight line - appended, so a
        file from before shapes existed reads every segment as linear. */
    inline void writePoint(std::ostringstream& out, const char* tag, const AutomationPoint& point)
    {
        out << tag << " " << num(point.beat) << " " << num((double) point.value);
        if (point.shape != CurveShape::Linear)
            out << " " << (int) point.shape;
        out << "\n";
    }

    /** The mirror of writePoint: adds the record's point to @p lane. */
    inline void readPoint(const std::string& fields, AutomationLane& lane)
    {
        std::istringstream ps(fields);
        double beat = 0.0, value = 0.0;
        int    shape = 0;
        ps >> beat >> value >> shape;
        lane.addPoint(beat, (float) value, engine::curveShapeFrom(shape));
    }

    /** Sets one effect parameter from a project, by descriptor. A choice or a
        switch is kept to its range, since it becomes an enum or a count; a
        slider value is taken as saved. */
    inline void loadEffectParam(EffectSlot& slot, const EffectParam& param, double value)
    {
        if (param.control == ParamControl::Slider)
            param.access.set(slot, value);
        else
            setParamValue(slot, param, value);
    }

    /** One slot's settings, as ` effect.param=value` pairs: every parameter of
        its own kind, and any other built-in's that has been moved off its
        default (a slot keeps every kind's settings, so switching kind loses
        nothing). Named rather than positional, so adding a parameter or an
        effect doesn't move anything already in a file. */
    inline void writeEffectParams(std::ostringstream& out, const EffectSlot& slot)
    {
        const EffectSlot defaults;
        for (const auto& effect : builtInEffects())
            for (const auto& param : effect.params)
            {
                const double value = paramValue(slot, param);
                if (effect.kind == slot.kind || value != paramValue(defaults, param))
                    out << " " << effect.id << "." << param.id << "=" << num(value);
            }
    }

    /** The mirror of writeEffectParams. A name this build doesn't know is
        skipped rather than refusing the file. */
    inline void readEffectParams(const std::string& text, EffectSlot& slot)
    {
        std::istringstream tokens(text);
        std::string        token;
        while (tokens >> token)
        {
            const auto dot    = token.find('.');
            const auto equals = token.find('=');
            if (dot == std::string::npos || equals == std::string::npos || equals < dot)
                continue;

            const auto* effect = descriptorFor(std::string_view(token).substr(0, dot));
            const auto* param  = effect != nullptr
                                   ? paramFor(*effect, std::string_view(token).substr(dot + 1, equals - dot - 1))
                                   : nullptr;
            if (param == nullptr)
                continue;

            std::istringstream number(token.substr(equals + 1));
            number.imbue(std::locale::classic());
            double value = 0.0;
            if (number >> value)
                loadEffectParam(slot, *param, value);
        }
    }

    /** The order the FXSLOT line held every built-in's settings in, before
        they were saved by name. Frozen: files written that way are read
        through it, and nothing is ever added to it. */
    inline constexpr const char* kPositionalEffectParams[] {
        "filter.mode", "filter.cutoff", "filter.resonance", "delay.time", "delay.feedback", "delay.mix",
        "reverb.room", "reverb.damping", "reverb.mix", "drive.drive", "drive.tone", "drive.level",
        "drive.hardClip", "drive.cabinet", "drive.asymmetry", "drive.oversample", "drive.stages",
        "drive.cabinetIr", "compressor.threshold", "compressor.ratio", "compressor.attack",
        "compressor.release", "compressor.makeUp", "tremolo.rate", "tremolo.depth", "chorus.rate",
        "chorus.depth", "chorus.mix", "wobble.rate", "wobble.depth", "wobble.cutoff", "wobble.resonance",
        "wobble.mix", "gate.threshold", "gate.range", "gate.attack", "gate.hold", "gate.release", "eq.lowFreq",
        "eq.low", "eq.midFreq", "eq.mid", "eq.midQ", "eq.highFreq", "eq.high", "amplify.gain", "invert.left",
        "invert.right", "dcOffset.cutoff", "limiter.input", "limiter.ceiling", "limiter.release",
        "phaser.rate", "phaser.depth", "phaser.feedback", "phaser.stages", "phaser.mix", "flanger.rate",
        "flanger.depth", "flanger.delay", "flanger.feedback", "flanger.mix", "bassTreble.bass",
        "bassTreble.treble", "bassTreble.volume", "stereoTool.width", "stereoTool.balance", "stereoTool.mono",
        "stereoTool.swap", "graphicEq.band31", "graphicEq.band62", "graphicEq.band125", "graphicEq.band250",
        "graphicEq.band500", "graphicEq.band1k", "graphicEq.band2k", "graphicEq.band4k", "graphicEq.band8k",
        "graphicEq.band16k", "deEsser.frequency", "deEsser.threshold", "deEsser.reduction",
        "expander.threshold", "expander.ratio", "expander.range", "expander.attack", "expander.release",
        "ringMod.frequency", "ringMod.mix", "wah.rate", "wah.depth", "wah.resonance", "wah.mix", "echo.time",
        "echo.taps", "echo.decay", "echo.mix", "echo.pingPong", "multiband.lowCrossover",
        "multiband.highCrossover", "multiband.lowThreshold", "multiband.lowRatio", "multiband.lowMakeUp",
        "multiband.midThreshold", "multiband.midRatio", "multiband.midMakeUp", "multiband.highThreshold",
        "multiband.highRatio", "multiband.highMakeUp", "multiband.attack", "multiband.release",
        "parametricEq.band1Type", "parametricEq.band1Hz", "parametricEq.band1Gain", "parametricEq.band1Q",
        "parametricEq.band2Type", "parametricEq.band2Hz", "parametricEq.band2Gain", "parametricEq.band2Q",
        "parametricEq.band3Type", "parametricEq.band3Hz", "parametricEq.band3Gain", "parametricEq.band3Q",
        "parametricEq.band4Type", "parametricEq.band4Hz", "parametricEq.band4Gain", "parametricEq.band4Q",
        "parametricEq.band5Type", "parametricEq.band5Hz", "parametricEq.band5Gain", "parametricEq.band5Q",
        "parametricEq.band6Type", "parametricEq.band6Hz", "parametricEq.band6Gain", "parametricEq.band6Q",
        "dynamics.points", "dynamics.point1In", "dynamics.point1Out", "dynamics.point2In",
        "dynamics.point2Out", "dynamics.point3In", "dynamics.point3Out", "dynamics.point4In",
        "dynamics.point4Out", "dynamics.point5In", "dynamics.point5Out", "dynamics.point6In",
        "dynamics.point6Out", "dynamics.detector", "dynamics.attack", "dynamics.release", "dynamics.makeUp",
        "graphicEq31.band1", "graphicEq31.band2", "graphicEq31.band3", "graphicEq31.band4",
        "graphicEq31.band5", "graphicEq31.band6", "graphicEq31.band7", "graphicEq31.band8",
        "graphicEq31.band9", "graphicEq31.band10", "graphicEq31.band11", "graphicEq31.band12",
        "graphicEq31.band13", "graphicEq31.band14", "graphicEq31.band15", "graphicEq31.band16",
        "graphicEq31.band17", "graphicEq31.band18", "graphicEq31.band19", "graphicEq31.band20",
        "graphicEq31.band21", "graphicEq31.band22", "graphicEq31.band23", "graphicEq31.band24",
        "graphicEq31.band25", "graphicEq31.band26", "graphicEq31.band27", "graphicEq31.band28",
        "graphicEq31.band29", "graphicEq31.band30", "graphicEq31.band31", "convolution.mix",
        "convolution.preDelay", "convolution.gain", "vocoder.carrier", "vocoder.pitch", "vocoder.bands",
        "vocoder.response", "vocoder.mix", "vocoder.gain", "channelMixer.leftToLeft",
        "channelMixer.rightToLeft", "channelMixer.leftToRight", "channelMixer.rightToRight",
        "channelMixer.midSide"
    };

    /** Reads the settings an older FXSLOT line carries after its kind and
        enabled flag, in kPositionalEffectParams order. A file from before a
        field existed stops short, and the rest keep their defaults. */
    inline void readPositionalEffectParams(std::istream& fields, EffectSlot& slot)
    {
        for (const std::string_view name : kPositionalEffectParams)
        {
            double value = 0.0;
            if (! (fields >> value))
                return;

            const auto  dot    = name.find('.');
            const auto* effect = descriptorFor(name.substr(0, dot));
            if (const auto* param = effect != nullptr ? paramFor(*effect, name.substr(dot + 1)) : nullptr)
                loadEffectParam(slot, *param, value);
        }
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
        detail::writePoint(out, "APT", p);
    out << "MARKERS " << song.markers.size() << "\n";
    for (const auto& marker : song.markers)
    {
        // The name is the rest of its line, so a line break in it becomes a space.
        auto name = marker.name;
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::replace(name.begin(), name.end(), '\r', ' ');

        out << "MARKER " << marker.id << " " << detail::num(marker.startBeats) << " "
            << detail::num(marker.lengthBeats) << " " << name << "\n";
    }

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
                detail::writePoint(out, "TAPT", pt);
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

        // The effect chain, in order.
        out << "FXCHAIN " << track.effectChain.size() << "\n";
        for (const auto& slot : track.effectChain)
        {
            out << "FXSLOT " << (int) slot.kind << " " << (slot.enabled ? 1 : 0) << "\n";
            out << "FXPARAMS";
            detail::writeEffectParams(out, slot);
            out << "\n";

            // A path takes the rest of its own line, as a plugin's name does.
            if (! slot.convolution.irFile.empty())
                out << "FXIR " << slot.convolution.irFile << "\n";

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

            // Only lanes with points, and only when there are some, as for
            // the track's own lanes.
            size_t effectLanes = 0;
            for (const auto& [paramId, lane] : slot.automation)
                if (! lane.empty())
                    ++effectLanes;

            if (effectLanes > 0)
            {
                out << "FXAUTOS " << effectLanes << "\n";
                for (const auto& [paramId, lane] : slot.automation)
                {
                    if (lane.empty())
                        continue;
                    out << "FXLANE " << paramId << " " << lane.points().size() << "\n";
                    for (const auto& pt : lane.points())
                        detail::writePoint(out, "TAPT", pt);
                }
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

        if (readTagged("CLIPCHANS", rest))
            clip.channels = engine::clipChannelsFrom(std::atoi(rest.c_str()));

        if (readTagged("CLIPAUTOXF", rest))
        {
            std::istringstream as(rest);
            int in = 0, out = 0;
            as >> in >> out;
            clip.autoFadeIn  = in != 0;
            clip.autoFadeOut = out != 0;
        }

        if (readTagged("CLIPENV", rest))
        {
            std::istringstream es(rest);
            es.imbue(std::locale::classic());

            int count = 0;
            es >> count;
            for (int i = 0; i < count; ++i)
            {
                double seconds = 0.0, gain = 1.0;
                if (! (es >> seconds >> gain))
                    break; // a truncated line keeps the points it has
                clip.envelope.addPoint(seconds, (float) gain);
            }
        }

        if (readTagged("CLIPSPEC", rest))
        {
            std::istringstream ss(rest);
            ss.imbue(std::locale::classic());

            int count = 0;
            ss >> count;
            for (int i = 0; i < count; ++i)
            {
                engine::SpectralRegion region;
                double gainDb = 0.0;
                if (! (ss >> region.startSeconds >> region.endSeconds >> region.lowHz >> region.highHz >> gainDb))
                    break;
                region.gainDb = (float) gainDb;
                clip.spectralEdits.push_back(region);
            }
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

    if (! readTagged("BPM", rest))
        return fail("missing tempo");
    song.bpm = std::strtod(rest.c_str(), nullptr);

    if (! readTagged("TSNUM", rest))
        return fail("missing time signature");
    song.timeSigNumerator = std::atoi(rest.c_str());

    if (! readTagged("TSDEN", rest))
        return fail("missing time signature");
    song.timeSigDenominator = std::atoi(rest.c_str());

    if (! readTagged("NEXTID", rest))
        return fail("missing id counter");
    song.nextId = std::atoi(rest.c_str());

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
            detail::readPoint(rest, song.masterGainDb);
        }
    }

    if (readTagged("MARKERS", rest))
    {
        const int markerCount = std::atoi(rest.c_str());
        for (int i = 0; i < markerCount; ++i)
        {
            if (! readTagged("MARKER", rest)) return fail("truncated marker list");

            std::istringstream fields(rest);
            Marker marker;
            fields >> marker.id >> marker.startBeats >> marker.lengthBeats;

            std::string name;
            std::getline(fields, name);
            marker.name = detail::trimLeadingSpace(std::move(name));

            song.markers.push_back(std::move(marker));
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
                    detail::readPoint(rest, lane);
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

                std::istringstream fields(rest);
                fields.imbue(std::locale::classic());
                EffectSlot slot;
                int        kind = 0, enabled = 0;
                fields >> kind >> enabled;
                slot.kind    = (EffectKind) kind;
                slot.enabled = enabled != 0;

                // Settings by name; a file from before that has them all on
                // this line instead, by position.
                detail::readPositionalEffectParams(fields, slot);
                if (readTagged("FXPARAMS", rest))
                    detail::readEffectParams(rest, slot);

                if (readTagged("FXIR", rest))
                    slot.convolution.irFile = rest;

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

                if (readTagged("FXAUTOS", rest))
                {
                    const int laneCount = std::atoi(rest.c_str());
                    for (int l = 0; l < laneCount; ++l)
                    {
                        if (! readTagged("FXLANE", rest)) return fail("truncated effect automation");
                        std::istringstream ls(rest);
                        std::string        paramId;
                        int                pointCount = 0;
                        ls >> paramId >> pointCount;

                        auto& lane = slot.automation[paramId];
                        for (int p = 0; p < pointCount; ++p)
                        {
                            if (! readTagged("TAPT", rest)) return fail("truncated effect automation");
                            detail::readPoint(rest, lane);
                        }
                    }
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
