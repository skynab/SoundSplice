#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/AudioClipSlot.h"
#include "engine/AudioExport.h"
#include "engine/ClipData.h"
#include "engine/ClipSlot.h"
#include "engine/DelayEffect.h"
#include "engine/InstrumentTrack.h"
#include "engine/Pattern.h"
#include "engine/ProcessContext.h"
#include "engine/ReverbEffect.h"

namespace looper::engine
{
/**
    Renders patterns to audio offline (no audio device), reusing the exact same
    InstrumentTrack render path the live engine uses. This is what "Bounce" and
    the headless bounce tool are built on — and the first way the audio output can
    be inspected without hardware.
*/
class OfflineRenderer
{
public:
    /**
        Fills a block's transport snapshot at a constant tempo.

        Shared by every render loop in this class. They each filled it by hand
        before, and they did not agree — only one of the five set ppqPosition
        at all, which went unnoticed while nodes derived their own timing from
        `bpm`. Now that scheduling reads the block's musical span, a snapshot
        missing it renders silence, so there is one place that fills it.

        Constant tempo is correct here: this is the offline harness the tests
        and the bounce tool drive, and it renders at a single BPM by
        construction. AudioEngine::renderOffline is what renders a project's
        real tempo map.
    */
    static void fillTransport(ProcessContext& ctx, int64_t playhead, int numSamples,
                              double bpm, double sampleRate)
    {
        const double samplesPerBeat = bpm > 0.0 ? sampleRate * 60.0 / bpm : 0.0;

        ctx.transport.playing         = true;
        ctx.transport.playheadSamples = playhead;
        ctx.transport.bpm             = bpm;
        ctx.transport.ppqPosition     = samplesPerBeat > 0.0 ? (double) playhead / samplesPerBeat : 0.0;
        ctx.transport.ppqAtBlockEnd   = samplesPerBeat > 0.0
                                          ? (double) (playhead + numSamples) / samplesPerBeat : 0.0;
    }

    /** Given a track index and a beat position, returns that track's gain in dB
        at that beat (the last argument is the track's static gainDb, to return
        as a fallback for tracks with no automation of their own). Deliberately
        JUCE- and model-independent (the engine layer doesn't know what
        "automation" is) — the caller supplies a lambda that reads whatever
        automation representation it uses, e.g. model::AutomationLane::valueAt.
        Pass a default-constructed (empty) one to skip this entirely — every
        track then uses its static gainsDb for the whole render, exactly as
        before this parameter existed. */
    /** Per-track automation, one entry per pattern, applied by the tracks
        themselves — the same TrackAutomation the live engine uses, so an
        export and a playback run the identical code rather than two
        mechanisms that have to be kept agreeing. */
    using TrackAutomationList = std::vector<TrackAutomation>;

    /** Renders one instrument track per pattern, at the given per-track gains (dB),
        solo flags, clip start offsets (beats — the track stays silent until the
        transport reaches this point, then plays and loops indefinitely). Solo
        follows the same "solo overrides, mute always wins" rule as the live
        engine.

        If @p automation is set, each track is given its curves and applies
        them itself while rendering, ramping across each block. This used to
        require rendering every track in isolation at unity gain and folding it
        back in with a per-sample curve, because InstrumentTrack could only
        apply one flat gain per block; now that it ramps natively, that whole
        second code path is gone and an export runs exactly the same automation
        code as live playback. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           const std::vector<bool>&    soloFlags,
                                           const std::vector<double>&  clipStartBeats,
                                           double bpm,
                                           double sampleRate,
                                           double numSeconds,
                                           int    blockSize = 512,
                                           const TrackAutomationList* automation = nullptr)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        std::vector<std::unique_ptr<InstrumentTrack>> tracks;
        for (size_t i = 0; i < patterns.size(); ++i)
        {
            auto track = std::make_unique<InstrumentTrack>();
            track->prepare(sampleRate, blockSize);

            // One clip per track, given an effectively unbounded length so it
            // keeps looping indefinitely from its start — the same semantics
            // this render() has always modelled (see renderClips() below for
            // genuine multi-clip-per-track scheduling).
            ClipSlot slot;
            slot.pattern     = patterns[i];
            slot.startBeats  = i < clipStartBeats.size() ? clipStartBeats[i] : 0.0;
            slot.lengthBeats = 1.0e9;
            track->sequencer.submitClips(new std::vector<ClipSlot> { slot });

            if (i < gainsDb.size())
                track->gainDb.store(gainsDb[i]);
            if (i < soloFlags.size())
                track->solo.store(soloFlags[i]);

            // Handed over the same way the live engine does it; the track
            // picks it up on its first render() and applies it itself.
            if (automation != nullptr && i < automation->size())
                track->setAutomation(new TrackAutomation((*automation)[i]));

            tracks.push_back(std::move(track));
        }

        bool anySolo = false;
        for (auto& track : tracks)
            anySolo |= track->solo.load();

        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            // Tracks read ppqPosition to place themselves on their automation
            // curves, and the sequencers read the block's span to schedule.
            fillTransport(ctx, playhead, n, bpm, sampleRate);

            for (auto& track : tracks)
                track->render(block, noLiveMidi, ctx, false, anySolo);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Convenience overload: no solo flags, no clip-start offsets. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           const std::vector<bool>&    soloFlags,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, gainsDb, soloFlags, std::vector<double>{}, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload: no solo flags (no track is ever solo-silenced), no clip-start offsets. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, gainsDb, std::vector<bool>{}, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload: patterns at unity gain, no solo. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns, double bpm,
                                           double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, std::vector<float>(patterns.size(), 0.0f), bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload for a single pattern. */
    static juce::AudioBuffer<float> render(const Pattern& pattern, double bpm,
                                           double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(std::vector<Pattern> { pattern }, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Renders a single track from an explicit list of ClipSlots — for verifying
        genuine multi-clip-per-track scheduling (silence between clips, each
        clip's own length gating its end). The render() overloads above still
        model one clip per track (all the current UI can create), always with
        an unbounded length. */
    static juce::AudioBuffer<float> renderClips(const std::vector<ClipSlot>& clips,
                                                double bpm, double sampleRate, double numSeconds,
                                                int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        InstrumentTrack track;
        track.prepare(sampleRate, blockSize);
        track.sequencer.submitClips(new std::vector<ClipSlot>(clips));

        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            fillTransport(ctx, playhead, n, bpm, sampleRate);

            track.render(block, noLiveMidi, ctx, false, false);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Renders a single track's audio-clip player alone (bypassing patterns) at
        the given gain and clip-start offset — for verifying that a decoded
        audio clip plays back through the exact same per-track gain/peak
        pipeline as synth content, with the same clip-start gating. */
    static juce::AudioBuffer<float> renderAudioClip(const ClipData& clipData, double clipStartBeats,
                                                    float gainDb, double bpm, double sampleRate,
                                                    double numSeconds, int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        InstrumentTrack track;
        track.prepare(sampleRate, blockSize);
        track.gainDb.store(gainDb);
        track.audioPlayer.submitSingleClip(new ClipData(clipData), clipStartBeats);

        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            fillTransport(ctx, playhead, n, bpm, sampleRate);

            track.render(block, noLiveMidi, ctx, false, false);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Renders a single track's audio-clip player from an explicit list of
        AudioClipSlots — for verifying genuine multi-clip-per-track audio
        scheduling (silence between clips, each clip's own length gating when
        it ends even if the file has more samples left). renderAudioClip()
        above still models the single-clip case (unbounded length, no gating
        other than clip-start) — the only case the current UI can create. */
    static juce::AudioBuffer<float> renderAudioClips(const std::vector<AudioClipSlot>& clips,
                                                     float gainDb, double bpm, double sampleRate,
                                                     double numSeconds, int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        InstrumentTrack track;
        track.prepare(sampleRate, blockSize);
        track.gainDb.store(gainDb);
        track.audioPlayer.submitClips(new std::vector<AudioClipSlot>(clips));

        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            fillTransport(ctx, playhead, n, bpm, sampleRate);

            track.render(block, noLiveMidi, ctx, false, false);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Writes a buffer to a 24-bit WAV. Returns false on failure.

        Kept as the name the recording path and the bounce tool already call,
        but the writing itself now goes through engine::writeAudioFile so
        there is one place that knows how to produce a file. 24-bit because
        that is what this has always produced and what those callers expect;
        the export dialog is where a depth gets chosen. */
    static bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        ExportOptions options;
        options.format        = ExportFormat::Wav;
        options.bitsPerSample = 24;
        options.sampleRate    = sampleRate;

        return writeAudioFile(file, buffer, options);
    }
};

} // namespace looper::engine
