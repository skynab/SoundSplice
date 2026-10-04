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
#include "engine/DelayEffect.h"
#include "engine/MixerTrack.h"
#include "engine/ProcessContext.h"
#include "engine/ReverbEffect.h"

namespace soundsplice::engine
{
/**
    Renders audio clips offline (no audio device), reusing the exact same
    MixerTrack render path the live engine uses: the tests' and the
    headless bounce tool's way to inspect the output without hardware.
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

    /** Per-track automation, one entry per clip, applied by the tracks
        themselves — the same TrackAutomation the live engine uses, so an
        export and a playback run the identical code. */
    using TrackAutomationList = std::vector<TrackAutomation>;

    /** Renders one track per clip, at the given per-track gains (dB), solo
        flags and clip starts (beats — the track is silent until the transport
        reaches its clip, which then plays once to its end). Solo follows the
        live engine's "solo overrides, mute always wins" rule. With
        @p automation, each track is given its curves and applies them itself
        while rendering, ramping across each block. */
    static juce::AudioBuffer<float> render(const std::vector<ClipData>& clips,
                                           const std::vector<float>&    gainsDb,
                                           const std::vector<bool>&     soloFlags,
                                           const std::vector<double>&   clipStartBeats,
                                           double bpm,
                                           double sampleRate,
                                           double numSeconds,
                                           int    blockSize = 512,
                                           const TrackAutomationList* automation = nullptr)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        std::vector<std::unique_ptr<MixerTrack>> tracks;
        for (size_t i = 0; i < clips.size(); ++i)
        {
            auto track = std::make_unique<MixerTrack>();
            track->prepare(sampleRate, blockSize);
            track->audioPlayer.submitSingleClip(new ClipData(clips[i]),
                                                i < clipStartBeats.size() ? clipStartBeats[i] : 0.0);

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

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();

            ProcessContext ctx;
            ctx.sampleRate = sampleRate;
            ctx.numSamples = n;
            fillTransport(ctx, playhead, n, bpm, sampleRate);

            for (auto& track : tracks)
                track->render(block, ctx, anySolo);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Convenience overload: no clip-start offsets. */
    static juce::AudioBuffer<float> render(const std::vector<ClipData>& clips,
                                           const std::vector<float>&    gainsDb,
                                           const std::vector<bool>&     soloFlags,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(clips, gainsDb, soloFlags, std::vector<double>{}, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload: no solo flags, no clip-start offsets. */
    static juce::AudioBuffer<float> render(const std::vector<ClipData>& clips,
                                           const std::vector<float>&    gainsDb,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(clips, gainsDb, std::vector<bool>{}, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Renders a single track's audio-clip player alone at the given gain
        and clip-start offset, through the same per-track gain/peak pipeline
        the live engine uses. */
    static juce::AudioBuffer<float> renderAudioClip(const ClipData& clipData, double clipStartBeats,
                                                    float gainDb, double bpm, double sampleRate,
                                                    double numSeconds, int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        MixerTrack track;
        track.prepare(sampleRate, blockSize);
        track.gainDb.store(gainDb);
        track.audioPlayer.submitSingleClip(new ClipData(clipData), clipStartBeats);

        juce::AudioBuffer<float> block(2, blockSize);

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

            track.render(block, ctx, false);

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

        MixerTrack track;
        track.prepare(sampleRate, blockSize);
        track.gainDb.store(gainDb);
        track.audioPlayer.submitClips(new std::vector<AudioClipSlot>(clips));

        juce::AudioBuffer<float> block(2, blockSize);

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

            track.render(block, ctx, false);

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

} // namespace soundsplice::engine
