#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "model/Song.h"
#include "model/GraphicEq31Bands.h"

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
                << detail::num((double) slot.eqPedal.highShelfDb) << " "
                << detail::num((double) slot.amplify.gainDb) << " "
                << (slot.invert.left ? 1 : 0) << " "
                << (slot.invert.right ? 1 : 0) << " "
                << detail::num((double) slot.dcOffset.cutoffHz) << " "
                << detail::num((double) slot.limiter.inputGainDb) << " "
                << detail::num((double) slot.limiter.ceilingDb) << " "
                << detail::num((double) slot.limiter.releaseMs) << " "
                << detail::num((double) slot.phaser.rateHz) << " "
                << detail::num((double) slot.phaser.depth) << " "
                << detail::num((double) slot.phaser.feedback) << " "
                << slot.phaser.stagePairs << " "
                << detail::num((double) slot.phaser.mix) << " "
                << detail::num((double) slot.flanger.rateHz) << " "
                << detail::num((double) slot.flanger.depth) << " "
                << detail::num((double) slot.flanger.delayMs) << " "
                << detail::num((double) slot.flanger.feedback) << " "
                << detail::num((double) slot.flanger.mix) << " "
                << detail::num((double) slot.bassTreble.bassDb) << " "
                << detail::num((double) slot.bassTreble.trebleDb) << " "
                << detail::num((double) slot.bassTreble.volumeDb) << " "
                << detail::num((double) slot.stereoTool.width) << " "
                << detail::num((double) slot.stereoTool.balance) << " "
                << (slot.stereoTool.mono ? 1 : 0) << " "
                << (slot.stereoTool.swap ? 1 : 0) << " "
                << detail::num((double) slot.graphicEq.band31) << " "
                << detail::num((double) slot.graphicEq.band62) << " "
                << detail::num((double) slot.graphicEq.band125) << " "
                << detail::num((double) slot.graphicEq.band250) << " "
                << detail::num((double) slot.graphicEq.band500) << " "
                << detail::num((double) slot.graphicEq.band1k) << " "
                << detail::num((double) slot.graphicEq.band2k) << " "
                << detail::num((double) slot.graphicEq.band4k) << " "
                << detail::num((double) slot.graphicEq.band8k) << " "
                << detail::num((double) slot.graphicEq.band16k) << " "
                << detail::num((double) slot.deEsser.frequencyHz) << " "
                << detail::num((double) slot.deEsser.thresholdDb) << " "
                << detail::num((double) slot.deEsser.maxReductionDb) << " "
                << detail::num((double) slot.expander.thresholdDb) << " "
                << detail::num((double) slot.expander.ratio) << " "
                << detail::num((double) slot.expander.rangeDb) << " "
                << detail::num((double) slot.expander.attackMs) << " "
                << detail::num((double) slot.expander.releaseMs) << " "
                << detail::num((double) slot.ringMod.frequencyHz) << " "
                << detail::num((double) slot.ringMod.mix) << " "
                << detail::num((double) slot.wah.rateHz) << " "
                << detail::num((double) slot.wah.depth) << " "
                << detail::num((double) slot.wah.resonance) << " "
                << detail::num((double) slot.wah.mix) << " "
                << detail::num((double) slot.echo.timeMs) << " "
                << slot.echo.taps << " "
                << detail::num((double) slot.echo.decay) << " "
                << detail::num((double) slot.echo.mix) << " "
                << (slot.echo.pingPong ? 1 : 0) << " "
                << detail::num((double) slot.multiband.lowHz) << " "
                << detail::num((double) slot.multiband.highHz) << " "
                << detail::num((double) slot.multiband.lowThresholdDb) << " "
                << detail::num((double) slot.multiband.lowRatio) << " "
                << detail::num((double) slot.multiband.lowMakeUpDb) << " "
                << detail::num((double) slot.multiband.midThresholdDb) << " "
                << detail::num((double) slot.multiband.midRatio) << " "
                << detail::num((double) slot.multiband.midMakeUpDb) << " "
                << detail::num((double) slot.multiband.highThresholdDb) << " "
                << detail::num((double) slot.multiband.highRatio) << " "
                << detail::num((double) slot.multiband.highMakeUpDb) << " "
                << detail::num((double) slot.multiband.attackMs) << " "
                << detail::num((double) slot.multiband.releaseMs) << " "
                << slot.parametricEq.band1Type << " "
                << detail::num((double) slot.parametricEq.band1Hz) << " "
                << detail::num((double) slot.parametricEq.band1GainDb) << " "
                << detail::num((double) slot.parametricEq.band1Q) << " "
                << slot.parametricEq.band2Type << " "
                << detail::num((double) slot.parametricEq.band2Hz) << " "
                << detail::num((double) slot.parametricEq.band2GainDb) << " "
                << detail::num((double) slot.parametricEq.band2Q) << " "
                << slot.parametricEq.band3Type << " "
                << detail::num((double) slot.parametricEq.band3Hz) << " "
                << detail::num((double) slot.parametricEq.band3GainDb) << " "
                << detail::num((double) slot.parametricEq.band3Q) << " "
                << slot.parametricEq.band4Type << " "
                << detail::num((double) slot.parametricEq.band4Hz) << " "
                << detail::num((double) slot.parametricEq.band4GainDb) << " "
                << detail::num((double) slot.parametricEq.band4Q) << " "
                << slot.parametricEq.band5Type << " "
                << detail::num((double) slot.parametricEq.band5Hz) << " "
                << detail::num((double) slot.parametricEq.band5GainDb) << " "
                << detail::num((double) slot.parametricEq.band5Q) << " "
                << slot.parametricEq.band6Type << " "
                << detail::num((double) slot.parametricEq.band6Hz) << " "
                << detail::num((double) slot.parametricEq.band6GainDb) << " "
                << detail::num((double) slot.parametricEq.band6Q) << " "
                << slot.dynamics.points << " "
                << detail::num((double) slot.dynamics.point1InDb) << " "
                << detail::num((double) slot.dynamics.point1OutDb) << " "
                << detail::num((double) slot.dynamics.point2InDb) << " "
                << detail::num((double) slot.dynamics.point2OutDb) << " "
                << detail::num((double) slot.dynamics.point3InDb) << " "
                << detail::num((double) slot.dynamics.point3OutDb) << " "
                << detail::num((double) slot.dynamics.point4InDb) << " "
                << detail::num((double) slot.dynamics.point4OutDb) << " "
                << detail::num((double) slot.dynamics.point5InDb) << " "
                << detail::num((double) slot.dynamics.point5OutDb) << " "
                << detail::num((double) slot.dynamics.point6InDb) << " "
                << detail::num((double) slot.dynamics.point6OutDb) << " "
                << slot.dynamics.detector << " "
                << detail::num((double) slot.dynamics.attackMs) << " "
                << detail::num((double) slot.dynamics.releaseMs) << " "
                << detail::num((double) slot.dynamics.makeUpDb);
            for (float gain : model::graphicEq31Gains(slot.graphicEq31))
                out << " " << detail::num((double) gain);
            out << " " << detail::num((double) slot.convolution.mix) << " "
                << detail::num((double) slot.convolution.preDelayMs) << " "
                << detail::num((double) slot.convolution.gainDb) << " "
                << slot.vocoder.carrier << " "
                << detail::num((double) slot.vocoder.pitchHz) << " "
                << slot.vocoder.bands << " "
                << detail::num((double) slot.vocoder.responseMs) << " "
                << detail::num((double) slot.vocoder.mix) << " "
                << detail::num((double) slot.vocoder.gainDb) << "\n";

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
            std::istringstream ps(rest);
            double beat = 0.0, value = 0.0;
            ps >> beat >> value;
            song.masterGainDb.addPoint(beat, (float) value);
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
                // Appended after the EQ; a file from before they existed stops
                // short, and the stream leaves these at their defaults.
                double amplifyGain = defaults.amplify.gainDb;
                int    invertLeft = defaults.invert.left ? 1 : 0, invertRight = defaults.invert.right ? 1 : 0;
                double dcCutoff = defaults.dcOffset.cutoffHz;
                double limiterInput = defaults.limiter.inputGainDb, limiterCeiling = defaults.limiter.ceilingDb;
                double limiterRelease = defaults.limiter.releaseMs;
                double phaserRate = defaults.phaser.rateHz, phaserDepth = defaults.phaser.depth;
                double phaserFeedback = defaults.phaser.feedback, phaserMix = defaults.phaser.mix;
                int    phaserPairs = defaults.phaser.stagePairs;
                double flangerRate = defaults.flanger.rateHz, flangerDepth = defaults.flanger.depth;
                double flangerDelay = defaults.flanger.delayMs, flangerFeedback = defaults.flanger.feedback;
                double flangerMix = defaults.flanger.mix;
                double bassDb = defaults.bassTreble.bassDb, trebleDb = defaults.bassTreble.trebleDb;
                double toneVolume = defaults.bassTreble.volumeDb;
                double stereoWidth = defaults.stereoTool.width, stereoBalance = defaults.stereoTool.balance;
                int    stereoMono = defaults.stereoTool.mono ? 1 : 0, stereoSwap = defaults.stereoTool.swap ? 1 : 0;
                double eqBand31 = defaults.graphicEq.band31;
                double eqBand62 = defaults.graphicEq.band62;
                double eqBand125 = defaults.graphicEq.band125;
                double eqBand250 = defaults.graphicEq.band250;
                double eqBand500 = defaults.graphicEq.band500;
                double eqBand1k = defaults.graphicEq.band1k;
                double eqBand2k = defaults.graphicEq.band2k;
                double eqBand4k = defaults.graphicEq.band4k;
                double eqBand8k = defaults.graphicEq.band8k;
                double eqBand16k = defaults.graphicEq.band16k;
                double deEssFrequency = defaults.deEsser.frequencyHz, deEssThreshold = defaults.deEsser.thresholdDb;
                double deEssReduction = defaults.deEsser.maxReductionDb;
                double expThreshold = defaults.expander.thresholdDb, expRatio = defaults.expander.ratio;
                double expRange = defaults.expander.rangeDb, expAttack = defaults.expander.attackMs;
                double expRelease = defaults.expander.releaseMs;
                double ringFrequency = defaults.ringMod.frequencyHz, ringMix = defaults.ringMod.mix;
                double wahRate = defaults.wah.rateHz, wahDepth = defaults.wah.depth;
                double wahResonance = defaults.wah.resonance, wahMix = defaults.wah.mix;
                double echoTime = defaults.echo.timeMs, echoDecay = defaults.echo.decay, echoMix = defaults.echo.mix;
                int    echoTaps = defaults.echo.taps, echoPingPong = defaults.echo.pingPong ? 1 : 0;
                double mbLowHz = defaults.multiband.lowHz, mbHighHz = defaults.multiband.highHz;
                double mbLowThreshold = defaults.multiband.lowThresholdDb;
                double mbLowRatio = defaults.multiband.lowRatio;
                double mbLowMakeUp = defaults.multiband.lowMakeUpDb;
                double mbMidThreshold = defaults.multiband.midThresholdDb;
                double mbMidRatio = defaults.multiband.midRatio;
                double mbMidMakeUp = defaults.multiband.midMakeUpDb;
                double mbHighThreshold = defaults.multiband.highThresholdDb;
                double mbHighRatio = defaults.multiband.highRatio;
                double mbHighMakeUp = defaults.multiband.highMakeUpDb;
                double mbAttack = defaults.multiband.attackMs, mbRelease = defaults.multiband.releaseMs;
                int    peqType1 = defaults.parametricEq.band1Type;
                double peqHz1 = defaults.parametricEq.band1Hz, peqGain1 = defaults.parametricEq.band1GainDb, peqQ1 = defaults.parametricEq.band1Q;
                int    peqType2 = defaults.parametricEq.band2Type;
                double peqHz2 = defaults.parametricEq.band2Hz, peqGain2 = defaults.parametricEq.band2GainDb, peqQ2 = defaults.parametricEq.band2Q;
                int    peqType3 = defaults.parametricEq.band3Type;
                double peqHz3 = defaults.parametricEq.band3Hz, peqGain3 = defaults.parametricEq.band3GainDb, peqQ3 = defaults.parametricEq.band3Q;
                int    peqType4 = defaults.parametricEq.band4Type;
                double peqHz4 = defaults.parametricEq.band4Hz, peqGain4 = defaults.parametricEq.band4GainDb, peqQ4 = defaults.parametricEq.band4Q;
                int    peqType5 = defaults.parametricEq.band5Type;
                double peqHz5 = defaults.parametricEq.band5Hz, peqGain5 = defaults.parametricEq.band5GainDb, peqQ5 = defaults.parametricEq.band5Q;
                int    peqType6 = defaults.parametricEq.band6Type;
                int    dyn_points = defaults.dynamics.points;
                double dyn_point1InDb = defaults.dynamics.point1InDb;
                double dyn_point1OutDb = defaults.dynamics.point1OutDb;
                double dyn_point2InDb = defaults.dynamics.point2InDb;
                double dyn_point2OutDb = defaults.dynamics.point2OutDb;
                double dyn_point3InDb = defaults.dynamics.point3InDb;
                double dyn_point3OutDb = defaults.dynamics.point3OutDb;
                double dyn_point4InDb = defaults.dynamics.point4InDb;
                double dyn_point4OutDb = defaults.dynamics.point4OutDb;
                double dyn_point5InDb = defaults.dynamics.point5InDb;
                double dyn_point5OutDb = defaults.dynamics.point5OutDb;
                double dyn_point6InDb = defaults.dynamics.point6InDb;
                double dyn_point6OutDb = defaults.dynamics.point6OutDb;
                int    dyn_detector = defaults.dynamics.detector;
                double dyn_attackMs = defaults.dynamics.attackMs;
                double dyn_releaseMs = defaults.dynamics.releaseMs;
                double dyn_makeUpDb = defaults.dynamics.makeUpDb;
                auto   geq31 = model::graphicEq31Gains(defaults.graphicEq31);
                double convMix = defaults.convolution.mix, convPreDelay = defaults.convolution.preDelayMs;
                double convGain = defaults.convolution.gainDb;
                int    vocCarrier = defaults.vocoder.carrier, vocBands = defaults.vocoder.bands;
                double vocPitch = defaults.vocoder.pitchHz, vocResponse = defaults.vocoder.responseMs;
                double vocMix = defaults.vocoder.mix, vocGain = defaults.vocoder.gainDb;
                double peqHz6 = defaults.parametricEq.band6Hz, peqGain6 = defaults.parametricEq.band6GainDb, peqQ6 = defaults.parametricEq.band6Q;

                ss >> kind >> enabled >> filterMode >> cutoff >> resonance
                   >> delayTime >> delayFeedback >> delayMix >> room >> damping >> reverbMix
                   >> driveAmount >> driveTone >> driveLevel >> driveHard >> driveCab
                   >> driveAsymmetry >> driveOversample >> driveStages >> driveCabinetIr
                   >> compThreshold >> compRatio >> compAttack >> compRelease >> compMakeUp
                   >> tremRate >> tremDepth
                   >> chorusRate >> chorusDepth >> chorusMix
                   >> wobbleRateBeats >> wobbleDepth >> wobbleBaseCutoffHz >> wobbleResonance >> wobbleMix
                   >> gateThreshold >> gateRange >> gateAttack >> gateHold >> gateRelease
                   >> eqLowHz >> eqLowDb >> eqMidHz >> eqMidDb >> eqMidQ >> eqHighHz >> eqHighDb
                   >> amplifyGain >> invertLeft >> invertRight >> dcCutoff
                   >> limiterInput >> limiterCeiling >> limiterRelease
                   >> phaserRate >> phaserDepth >> phaserFeedback >> phaserPairs >> phaserMix
                   >> flangerRate >> flangerDepth >> flangerDelay >> flangerFeedback >> flangerMix
                   >> bassDb >> trebleDb >> toneVolume
                   >> stereoWidth >> stereoBalance >> stereoMono >> stereoSwap
                   >> eqBand31 >> eqBand62 >> eqBand125 >> eqBand250 >> eqBand500 >> eqBand1k >> eqBand2k >> eqBand4k >> eqBand8k >> eqBand16k
                   >> deEssFrequency >> deEssThreshold >> deEssReduction
                   >> expThreshold >> expRatio >> expRange >> expAttack >> expRelease
                   >> ringFrequency >> ringMix
                   >> wahRate >> wahDepth >> wahResonance >> wahMix
                   >> echoTime >> echoTaps >> echoDecay >> echoMix >> echoPingPong
                   >> mbLowHz >> mbHighHz
                   >> mbLowThreshold >> mbLowRatio >> mbLowMakeUp >> mbMidThreshold >> mbMidRatio >> mbMidMakeUp >> mbHighThreshold >> mbHighRatio >> mbHighMakeUp
                   >> mbAttack >> mbRelease
                   >> peqType1 >> peqHz1 >> peqGain1 >> peqQ1
                   >> peqType2 >> peqHz2 >> peqGain2 >> peqQ2
                   >> peqType3 >> peqHz3 >> peqGain3 >> peqQ3
                   >> peqType4 >> peqHz4 >> peqGain4 >> peqQ4
                   >> peqType5 >> peqHz5 >> peqGain5 >> peqQ5
                   >> peqType6 >> peqHz6 >> peqGain6 >> peqQ6
                   >> dyn_points >> dyn_point1InDb >> dyn_point1OutDb >> dyn_point2InDb >> dyn_point2OutDb >> dyn_point3InDb >> dyn_point3OutDb >> dyn_point4InDb >> dyn_point4OutDb >> dyn_point5InDb >> dyn_point5OutDb >> dyn_point6InDb >> dyn_point6OutDb >> dyn_detector >> dyn_attackMs >> dyn_releaseMs >> dyn_makeUpDb;
                for (auto& gain : geq31)
                {
                    double value = gain;
                    if (! (ss >> value))
                        break; // an older file stops here: the rest keep their defaults
                    gain = (float) value;
                }
                ss >> convMix >> convPreDelay >> convGain
                   >> vocCarrier >> vocPitch >> vocBands >> vocResponse >> vocMix >> vocGain;

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
                slot.amplify.enabled        = slot.enabled && slot.kind == EffectKind::Amplify;
                slot.amplify.gainDb         = (float) amplifyGain;
                slot.invert.enabled         = slot.enabled && slot.kind == EffectKind::Invert;
                slot.invert.left            = invertLeft != 0;
                slot.invert.right           = invertRight != 0;
                slot.dcOffset.enabled       = slot.enabled && slot.kind == EffectKind::DcOffset;
                slot.dcOffset.cutoffHz      = (float) dcCutoff;
                slot.limiter.enabled        = slot.enabled && slot.kind == EffectKind::Limiter;
                slot.limiter.inputGainDb    = (float) limiterInput;
                slot.limiter.ceilingDb      = (float) limiterCeiling;
                slot.limiter.releaseMs      = (float) limiterRelease;
                slot.phaser.enabled         = slot.enabled && slot.kind == EffectKind::Phaser;
                slot.phaser.rateHz          = (float) phaserRate;
                slot.phaser.depth           = (float) phaserDepth;
                slot.phaser.feedback        = (float) phaserFeedback;
                slot.phaser.stagePairs      = std::clamp(phaserPairs, 1, 6);
                slot.phaser.mix             = (float) phaserMix;
                slot.flanger.enabled        = slot.enabled && slot.kind == EffectKind::Flanger;
                slot.flanger.rateHz         = (float) flangerRate;
                slot.flanger.depth          = (float) flangerDepth;
                slot.flanger.delayMs        = (float) flangerDelay;
                slot.flanger.feedback       = (float) flangerFeedback;
                slot.flanger.mix            = (float) flangerMix;
                slot.bassTreble.enabled     = slot.enabled && slot.kind == EffectKind::BassTreble;
                slot.bassTreble.bassDb      = (float) bassDb;
                slot.bassTreble.trebleDb    = (float) trebleDb;
                slot.bassTreble.volumeDb    = (float) toneVolume;
                slot.stereoTool.enabled     = slot.enabled && slot.kind == EffectKind::StereoTool;
                slot.stereoTool.width       = (float) stereoWidth;
                slot.stereoTool.balance     = (float) stereoBalance;
                slot.stereoTool.mono        = stereoMono != 0;
                slot.stereoTool.swap        = stereoSwap != 0;
                slot.graphicEq.enabled      = slot.enabled && slot.kind == EffectKind::GraphicEq;
                slot.graphicEq.band31 = (float) eqBand31;
                slot.graphicEq.band62 = (float) eqBand62;
                slot.graphicEq.band125 = (float) eqBand125;
                slot.graphicEq.band250 = (float) eqBand250;
                slot.graphicEq.band500 = (float) eqBand500;
                slot.graphicEq.band1k = (float) eqBand1k;
                slot.graphicEq.band2k = (float) eqBand2k;
                slot.graphicEq.band4k = (float) eqBand4k;
                slot.graphicEq.band8k = (float) eqBand8k;
                slot.graphicEq.band16k = (float) eqBand16k;
                slot.deEsser.enabled        = slot.enabled && slot.kind == EffectKind::DeEsser;
                slot.deEsser.frequencyHz    = (float) deEssFrequency;
                slot.deEsser.thresholdDb    = (float) deEssThreshold;
                slot.deEsser.maxReductionDb = (float) deEssReduction;
                slot.expander.enabled       = slot.enabled && slot.kind == EffectKind::Expander;
                slot.expander.thresholdDb   = (float) expThreshold;
                slot.expander.ratio         = (float) expRatio;
                slot.expander.rangeDb       = (float) expRange;
                slot.expander.attackMs      = (float) expAttack;
                slot.expander.releaseMs     = (float) expRelease;
                slot.ringMod.enabled        = slot.enabled && slot.kind == EffectKind::RingMod;
                slot.ringMod.frequencyHz    = (float) ringFrequency;
                slot.ringMod.mix            = (float) ringMix;
                slot.wah.enabled            = slot.enabled && slot.kind == EffectKind::Wah;
                slot.wah.rateHz             = (float) wahRate;
                slot.wah.depth              = (float) wahDepth;
                slot.wah.resonance          = (float) wahResonance;
                slot.wah.mix                = (float) wahMix;
                slot.echo.enabled           = slot.enabled && slot.kind == EffectKind::Echo;
                slot.echo.timeMs            = (float) echoTime;
                slot.echo.taps              = std::clamp(echoTaps, 1, 8);
                slot.echo.decay             = (float) echoDecay;
                slot.echo.mix               = (float) echoMix;
                slot.echo.pingPong          = echoPingPong != 0;
                slot.multiband.enabled      = slot.enabled && slot.kind == EffectKind::Multiband;
                slot.multiband.lowHz        = (float) mbLowHz;
                slot.multiband.highHz       = (float) mbHighHz;
                slot.multiband.lowThresholdDb = (float) mbLowThreshold;
                slot.multiband.lowRatio       = (float) mbLowRatio;
                slot.multiband.lowMakeUpDb    = (float) mbLowMakeUp;
                slot.multiband.midThresholdDb = (float) mbMidThreshold;
                slot.multiband.midRatio       = (float) mbMidRatio;
                slot.multiband.midMakeUpDb    = (float) mbMidMakeUp;
                slot.multiband.highThresholdDb = (float) mbHighThreshold;
                slot.multiband.highRatio       = (float) mbHighRatio;
                slot.multiband.highMakeUpDb    = (float) mbHighMakeUp;
                slot.multiband.attackMs     = (float) mbAttack;
                slot.multiband.releaseMs    = (float) mbRelease;
                slot.parametricEq.enabled   = slot.enabled && slot.kind == EffectKind::ParametricEq;
                slot.dynamics.enabled       = slot.enabled && slot.kind == EffectKind::Dynamics;
                slot.graphicEq31.enabled    = slot.enabled && slot.kind == EffectKind::GraphicEq31;
                model::setGraphicEq31Gains(slot.graphicEq31, geq31);
                slot.convolution.enabled    = slot.enabled && slot.kind == EffectKind::Convolution;
                slot.convolution.mix        = (float) convMix;
                slot.convolution.preDelayMs = (float) convPreDelay;
                slot.convolution.gainDb     = (float) convGain;
                slot.vocoder.enabled        = slot.enabled && slot.kind == EffectKind::Vocoder;
                slot.vocoder.carrier        = std::clamp(vocCarrier, 0, 2);
                slot.vocoder.pitchHz        = (float) vocPitch;
                slot.vocoder.bands          = std::clamp(vocBands, 4, 32);
                slot.vocoder.responseMs     = (float) vocResponse;
                slot.vocoder.mix            = (float) vocMix;
                slot.vocoder.gainDb         = (float) vocGain;
                slot.dynamics.points = std::clamp(dyn_points, 2, 6);
                slot.dynamics.point1InDb = (float) dyn_point1InDb;
                slot.dynamics.point1OutDb = (float) dyn_point1OutDb;
                slot.dynamics.point2InDb = (float) dyn_point2InDb;
                slot.dynamics.point2OutDb = (float) dyn_point2OutDb;
                slot.dynamics.point3InDb = (float) dyn_point3InDb;
                slot.dynamics.point3OutDb = (float) dyn_point3OutDb;
                slot.dynamics.point4InDb = (float) dyn_point4InDb;
                slot.dynamics.point4OutDb = (float) dyn_point4OutDb;
                slot.dynamics.point5InDb = (float) dyn_point5InDb;
                slot.dynamics.point5OutDb = (float) dyn_point5OutDb;
                slot.dynamics.point6InDb = (float) dyn_point6InDb;
                slot.dynamics.point6OutDb = (float) dyn_point6OutDb;
                slot.dynamics.detector = std::clamp(dyn_detector, 0, 1);
                slot.dynamics.attackMs = (float) dyn_attackMs;
                slot.dynamics.releaseMs = (float) dyn_releaseMs;
                slot.dynamics.makeUpDb = (float) dyn_makeUpDb;
                slot.parametricEq.band1Type   = std::clamp(peqType1, 0, 6);
                slot.parametricEq.band1Hz     = (float) peqHz1;
                slot.parametricEq.band1GainDb = (float) peqGain1;
                slot.parametricEq.band1Q      = (float) peqQ1;
                slot.parametricEq.band2Type   = std::clamp(peqType2, 0, 6);
                slot.parametricEq.band2Hz     = (float) peqHz2;
                slot.parametricEq.band2GainDb = (float) peqGain2;
                slot.parametricEq.band2Q      = (float) peqQ2;
                slot.parametricEq.band3Type   = std::clamp(peqType3, 0, 6);
                slot.parametricEq.band3Hz     = (float) peqHz3;
                slot.parametricEq.band3GainDb = (float) peqGain3;
                slot.parametricEq.band3Q      = (float) peqQ3;
                slot.parametricEq.band4Type   = std::clamp(peqType4, 0, 6);
                slot.parametricEq.band4Hz     = (float) peqHz4;
                slot.parametricEq.band4GainDb = (float) peqGain4;
                slot.parametricEq.band4Q      = (float) peqQ4;
                slot.parametricEq.band5Type   = std::clamp(peqType5, 0, 6);
                slot.parametricEq.band5Hz     = (float) peqHz5;
                slot.parametricEq.band5GainDb = (float) peqGain5;
                slot.parametricEq.band5Q      = (float) peqQ5;
                slot.parametricEq.band6Type   = std::clamp(peqType6, 0, 6);
                slot.parametricEq.band6Hz     = (float) peqHz6;
                slot.parametricEq.band6GainDb = (float) peqGain6;
                slot.parametricEq.band6Q      = (float) peqQ6;

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
