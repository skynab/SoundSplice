#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/AudioClipSlot.h"
#include "engine/AudioRecorder.h"
#include "engine/ClipData.h"
#include "engine/ClipSlot.h"
#include "engine/DelayEffect.h"
#include "engine/EffectChain.h"
#include <chrono>

#include "engine/PluginHost.h"
#include "engine/PluginNode.h"
#include "engine/FilterEffect.h"
#include "engine/InstrumentTrack.h"
#include "engine/Metronome.h"
#include "engine/SessionPlayer.h"
#include "engine/MidiFileIO.h"
#include "engine/MidiRecorder.h"
#include "engine/MasteringProcessor.h"
#include "engine/OfflineRenderer.h"
#include "engine/ReverbEffect.h"
#include "model/AutomationLane.h"
#include "model/MasteringPresets.h"
#include "engine/EffectSlotFactory.h"
#include "model/Song.h"

/** One chain node for @p slot, for the preset checks below - they need to
    render a `model::EffectSlot` chain the way a real track would.

    This used to be a hand-written if/else covering only the kinds the presets
    happened to use. It fell behind: drive gained asymmetry and oversampling
    and this copy kept setting neither, so the tool was measuring a signal
    path the app does not play - the one thing a verification tool must never
    do, and silently, since everything still ran and only the numbers lied.

    Now it goes through the same engine::makeEffectNode / engine::toSlotParams
    the app uses, so a kind or a parameter added anywhere reaches this
    automatically. Plugin slots still return null (no host here) and are
    skipped by the callers. */
static std::unique_ptr<soundsplice::engine::EffectProcessor> nodeForSlot(const soundsplice::model::EffectSlot& slot)
{
    return soundsplice::engine::makeConfiguredNode(slot);
}

/** Summed energy in [loHz, hiHz] of @p buf's left channel, by direct
    correlation at a geometric comb of frequencies (so each octave is
    weighted alike). Two fixed bands don't justify an FFT, and the same
    technique already measures the chorus's harmonic spill.

    Shared by the preset checks, which both need to say something about
    *timbre* rather than level: RMS barely moves between a sine and a
    driven, detuned saw at the same note, so a level comparison would pass
    for a preset that had been applied to entirely the wrong fields. */
static double bandEnergy(const juce::AudioBuffer<float>& buf, double loHz, double hiHz,
                         double sampleRate)
{
    const int   from  = (int) (0.05 * sampleRate);
    const int   count = (int) (0.5 * sampleRate);
    const auto* data  = buf.getReadPointer(0);

    double total = 0.0;
    for (double hz = loHz; hz <= hiHz; hz *= 1.15)
    {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < count; ++n)
        {
            const double phase = 2.0 * 3.14159265358979 * hz * (double) n / sampleRate;
            re += data[from + n] * std::cos(phase);
            im -= data[from + n] * std::sin(phase);
        }
        total += (re * re + im * im) / ((double) count * (double) count);
    }
    return total;
}

/** The largest sample-for-sample difference between two equally-sized
    buffers — "did this stage change anything at all", which several checks
    need and which no level comparison answers. */
static float worstBufferDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    const int channels = std::min(a.getNumChannels(), b.getNumChannels());
    const int samples  = std::min(a.getNumSamples(), b.getNumSamples());

    float worst = 0.0f;
    for (int ch = 0; ch < channels; ++ch)
        for (int n = 0; n < samples; ++n)
            worst = std::max(worst, std::abs(a.getReadPointer(ch)[n] - b.getReadPointer(ch)[n]));
    return worst;
}

// Headless bounce: renders a demo arpeggio to a WAV so the synth + sequencer
// audio path can be verified without an audio device. Also usable as a smoke test.
int main(int argc, char** argv)
{
    using namespace soundsplice::engine;

    // AudioRecorder check: feeds synthetic "input" directly into process() —
    // there's no live microphone in this headless verification, so this can
    // only confirm the capture + armed/finished handoff logic and the path to
    // disk, not that real hardware input reaches the callback.
    //
    // Recording streams to a file now rather than to a RAM buffer, so this
    // reads the written WAV back instead of inspecting a buffer — a stronger
    // check, since it covers the encoder and the background writer thread too.
    //
    // The old `capacityCapped` case is deliberately gone. It asserted that a
    // take longer than a fixed capacity was silently truncated, which is
    // precisely the data-loss bug this work removed; keeping it would pin the
    // behaviour the change exists to delete. `longTakeIsComplete` below is its
    // replacement and asserts the opposite.
    bool recorderWorks = false;
    {
        const double recSampleRate = 44100.0;
        const int    blockSize     = 512;

        std::vector<float> inputBlock((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
            inputBlock[(size_t) i] = (float) i / (float) blockSize; // a ramp, easy to verify exactly
        const float* channelPtrs[1] = { inputBlock.data() };

        juce::TimeSliceThread writerThread("BounceRecordWriter");
        writerThread.startThread(juce::Thread::Priority::normal);

        auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        // Reads a finished take back off disk, so what is verified is the file
        // the user would actually end up with.
        auto readBack = [&formats](const juce::File& file, std::vector<float>& out)
        {
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if (reader == nullptr)
                return false;

            juce::AudioBuffer<float> buffer(1, (int) reader->lengthInSamples);
            if (! reader->read(&buffer, 0, (int) reader->lengthInSamples, 0, true, false))
                return false;

            out.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples());
            return true;
        };

        const auto takeFile = tempDir.getNonexistentChildFile("soundsplice-take", ".wav");

        AudioRecorder recorder;
        recorder.prepare(recSampleRate, 1); // mono; no capacity to prepare any more

        // Not armed yet: must not capture, even while "playing".
        recorder.process(channelPtrs, 1, blockSize, true, 0);
        const bool capturesNothingWhenDisarmed = recorder.recordedSampleCount() == 0;

        const bool armSucceeds = recorder.arm(takeFile, writerThread);

        // Arm and record 3 blocks while playing, from a transport position that
        // isn't zero — the take has to remember where it started.
        constexpr int64_t kStartSample = 88200;
        for (int block = 0; block < 3; ++block)
            recorder.process(channelPtrs, 1, blockSize, true, kStartSample + block * blockSize);

        const bool capturedThreeBlocks       = recorder.recordedSampleCount() == blockSize * 3;
        const bool notFinishedWhileRecording = ! recorder.isFinished();
        const bool startPositionRemembered   = recorder.startPlayheadSamples() == kStartSample;

        // Disarm; the *next* process() call is what finalizes the take (and is
        // itself not captured, since it's already disarmed by then).
        recorder.disarm();
        recorder.process(channelPtrs, 1, blockSize, true, kStartSample);
        const bool finishedAfterDisarm  = recorder.isFinished();
        const bool nothingDropped       = recorder.droppedSampleCount() == 0;

        const auto writtenFile = recorder.finishTake();
        const bool takeFileExists = writtenFile.existsAsFile();

        std::vector<float> captured;
        bool contentMatches = readBack(writtenFile, captured)
                           && (int) captured.size() == blockSize * 3;

        for (int block = 0; block < 3 && contentMatches; ++block)
            for (int i = 0; i < blockSize; ++i)
                // 24-bit quantisation, not the float input, so the tolerance is
                // one LSB at that depth rather than an epsilon.
                if (std::abs(captured[(size_t) (block * blockSize + i)] - inputBlock[(size_t) i]) > 1.0e-6f)
                    contentMatches = false;

        writtenFile.deleteFile();

        // The regression this whole change exists for: a take far longer than
        // the old 180-second RAM capacity must come back complete. Run at a
        // deliberately silly block count rather than in real time — what is
        // being checked is that nothing caps it, not how fast the disk is.
        // A low rate deliberately: the claim is "200 seconds of audio, past
        // the old 180-second cap", and the cap was in seconds. 8kHz keeps that
        // claim exact while making it 1.6M samples rather than 8.8M, so the
        // check stays a smoke test rather than a disk benchmark.
        constexpr double kLongRate = 8000.0;

        AudioRecorder longRecorder;
        longRecorder.prepare(kLongRate, 1);

        const auto longFile = tempDir.getNonexistentChildFile("soundsplice-long-take", ".wav");
        longRecorder.arm(longFile, writerThread);

        // 200 seconds' worth: past the old cap, which would have discarded
        // everything after 180.
        //
        // Paced, because this loop is not real time. A live callback delivers
        // 512 samples every ~11.6ms and the writer thread drains far faster
        // than that, but pushed flat out this fills the FIFO in a few
        // milliseconds and the recorder correctly reports the overrun — which
        // would be measuring how fast this loop runs, not whether a long take
        // is capped. Sleeping every eighth block holds the producer to roughly
        // an eighth of the FIFO per millisecond, which any disk can drain.
        const int longBlocks = (int) (200.0 * kLongRate) / blockSize;
        for (int block = 0; block < longBlocks; ++block)
        {
            longRecorder.process(channelPtrs, 1, blockSize, true, block * blockSize);

            if (block % 8 == 7)
                juce::Thread::sleep(1);
        }

        longRecorder.disarm();
        longRecorder.process(channelPtrs, 1, blockSize, true, 0);

        const int64_t longPushed   = (int64_t) longBlocks * blockSize;
        const int64_t longRecorded  = longRecorder.recordedSampleCount();
        const int64_t longDropped   = longRecorder.droppedSampleCount();

        // What this asserts, and what it deliberately does not.
        //
        // Not "zero drops": this loop pushes 200 seconds of audio in about a
        // second, so it outruns any disk in bursts no matter how it is paced,
        // and requiring zero would be measuring the machine rather than the
        // recorder. `nothingDropped` above covers the realistic case.
        //
        // What matters for the regression is that every sample is *accounted
        // for* — written or explicitly counted as lost — and that capture kept
        // going far past the old 180-second cap instead of stopping dead at it.
        // The old code did neither: it froze at the cap and counted nothing.
        const bool longAccountsForEverything = longRecorded + longDropped == longPushed;
        const bool longRanPastTheOldCap      = longRecorded > (int64_t) (180.0 * kLongRate);

        const auto longWritten = longRecorder.finishTake();

        // And everything the recorder accepted actually reached the file —
        // which is the other half of "nothing is silently lost".
        std::vector<float> longSamples;
        const bool longTakeIsComplete = longAccountsForEverything
                                     && longRanPastTheOldCap
                                     && readBack(longWritten, longSamples)
                                     && (int64_t) longSamples.size() == longRecorded;
        longWritten.deleteFile();

        // Count-in: armed with a lead-in, the recorder must roll without
        // capturing, and — critically — a take abandoned *during* its count-in
        // must still finish. If it doesn't, isFinished() never goes true, the
        // owner waits forever on a take that never arrives, and recording is
        // dead until the app restarts.
        AudioRecorder countInRecorder;
        countInRecorder.prepare(recSampleRate, 1);
        const auto countInFile = tempDir.getNonexistentChildFile("soundsplice-countin", ".wav");
        countInRecorder.arm(countInFile, writerThread, (int64_t) blockSize * 2);

        countInRecorder.process(channelPtrs, 1, blockSize, true, 0);
        const bool countInCapturesNothing = countInRecorder.recordedSampleCount() == 0
                                         && countInRecorder.leadInRemaining() > 0;

        countInRecorder.process(channelPtrs, 1, blockSize, true, blockSize);     // lead-in now elapsed
        countInRecorder.process(channelPtrs, 1, blockSize, true, blockSize * 2); // this one captures
        const bool capturesAfterCountIn = countInRecorder.recordedSampleCount() == blockSize;

        // The count-in is skipped, so the take starts where playing started —
        // not where arming did.
        const bool countInStartSkipsLeadIn = countInRecorder.startPlayheadSamples() == blockSize * 2;
        countInRecorder.disarm();
        countInRecorder.process(channelPtrs, 1, blockSize, true, 0);
        countInRecorder.finishTake().deleteFile();

        AudioRecorder abandonedRecorder;
        abandonedRecorder.prepare(recSampleRate, 1);
        const auto abandonedFile = tempDir.getNonexistentChildFile("soundsplice-abandoned", ".wav");
        abandonedRecorder.arm(abandonedFile, writerThread, (int64_t) blockSize * 8); // a long count-in
        abandonedRecorder.process(channelPtrs, 1, blockSize, true, 0); // still counting in
        abandonedRecorder.disarm();                                    // ...and give up
        abandonedRecorder.process(channelPtrs, 1, blockSize, true, 0);

        // An abandoned take finishes *and* leaves no file behind: an empty WAV
        // header in the recordings folder is litter that looks like a take.
        const bool abandonedTakeFinishes = abandonedRecorder.isFinished()
                                        && abandonedRecorder.recordedSampleCount() == 0
                                        && abandonedRecorder.finishTake() == juce::File{}
                                        && ! abandonedFile.existsAsFile();

        writerThread.stopThread(2000);

        recorderWorks = capturesNothingWhenDisarmed && armSucceeds && capturedThreeBlocks
                     && notFinishedWhileRecording && startPositionRemembered && finishedAfterDisarm
                     && nothingDropped && takeFileExists && contentMatches && longTakeIsComplete
                     && countInCapturesNothing && capturesAfterCountIn && countInStartSkipsLeadIn
                     && abandonedTakeFinishes;
    }

    const double bpm        = 120.0;
    const double sampleRate = 44100.0;
    const double seconds    = 4.0;

    // Track 1: C-E-G-C arpeggio, one note per beat (the piano-roll demo).
    Pattern arp;
    arp.lengthBeats = 4.0;
    const int root     = 60;
    const int arpNotes[] = { 0, 4, 7, 12 };
    for (int i = 0; i < 4; ++i)
        arp.notes.push_back({ (double) i, 0.5, root + arpNotes[i], 0.8f });

    // Track 2: a simple root-note bass on beats 1 and 3, to exercise track summing.
    Pattern bass;
    bass.lengthBeats = 4.0;
    bass.notes.push_back({ 0.0, 1.0, 36, 0.9f });
    bass.notes.push_back({ 2.0, 1.0, 43, 0.9f });

    // The written file is the full two-track mix at unity gain.
    const auto buffer = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, bpm, sampleRate, seconds);

    // Per-track gain check: -6 dB should roughly halve the amplitude (10^(-6/20) ~= 0.501).
    const std::vector<Pattern> one { arp };
    const auto  full      = OfflineRenderer::render(one, std::vector<float> { 0.0f },  bpm, sampleRate, seconds);
    const auto  quiet     = OfflineRenderer::render(one, std::vector<float> { -6.0f }, bpm, sampleRate, seconds);
    const float rmsFull   = full.getRMSLevel(0, 0, full.getNumSamples());
    const float rmsQuiet  = quiet.getRMSLevel(0, 0, quiet.getNumSamples());
    const float gainRatio = rmsFull > 0.0f ? rmsQuiet / rmsFull : 0.0f;

    // Solo check: soloing track 1 (arp) must fully silence track 2 (bass), even
    // though bass is active and unmuted — the render should then match an
    // arp-only render (rmsFull, computed above) rather than the full mix.
    const auto  soloArp            = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { true, false },
                                                             bpm, sampleRate, seconds);
    const float rmsSoloArp         = soloArp.getRMSLevel(0, 0, soloArp.getNumSamples());
    const bool  soloMatchesArpOnly = std::abs(rmsSoloArp - rmsFull) < 1.0e-4f;

    // Stems sum to the mix.
    //
    // The property the whole stem export rests on: a stem is one track's share
    // of the mix, so putting them back together has to reconstruct it. If they
    // don't sum, they aren't stems — they are eight files that happen to have
    // come from the same project.
    //
    // Checked here rather than through AudioEngine because that needs an audio
    // device; OfflineRenderer takes the same per-track solo flags, so the
    // isolation mechanism being tested is the same one.
    //
    // Note this deliberately compares against the render *without* the master
    // bus, matching what a stem export writes — the master chain is
    // excluded from stems precisely so this sum holds.
    const auto stemArp  = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { true, false },
                                                  bpm, sampleRate, seconds);
    const auto stemBass = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { false, true },
                                                  bpm, sampleRate, seconds);
    const auto bothTracks = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { false, false },
                                                    bpm, sampleRate, seconds);

    juce::AudioBuffer<float> summedStems(bothTracks.getNumChannels(), bothTracks.getNumSamples());
    summedStems.clear();
    for (int ch = 0; ch < summedStems.getNumChannels(); ++ch)
    {
        summedStems.addFrom(ch, 0, stemArp,  ch, 0, summedStems.getNumSamples());
        summedStems.addFrom(ch, 0, stemBass, ch, 0, summedStems.getNumSamples());
    }

    // Both stems must actually contain something, or a pair of silent buffers
    // would sum to a silent mix and pass without proving anything.
    const float stemArpRms  = stemArp.getRMSLevel(0, 0, stemArp.getNumSamples());
    const float stemBassRms = stemBass.getRMSLevel(0, 0, stemBass.getNumSamples());

    const bool stemsSumToMix = stemArpRms > 1.0e-4f
                            && stemBassRms > 1.0e-4f
                            && worstBufferDifference(summedStems, bothTracks) < 1.0e-6f;


    // Clip-start check: delaying a track's clip start by 2 beats (1s at 120bpm)
    // must produce silence before that point and real signal after it.
    const auto  delayedStart      = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                            std::vector<double> { 2.0 }, bpm, sampleRate, seconds);
    const int   oneSecondSamples  = (int) sampleRate;
    const float rmsBeforeStart    = delayedStart.getRMSLevel(0, 0, oneSecondSamples);
    const float rmsAfterStart     = delayedStart.getRMSLevel(0, oneSecondSamples,
                                                             delayedStart.getNumSamples() - oneSecondSamples);
    const bool  clipStartGates    = rmsBeforeStart < 1.0e-5f && rmsAfterStart > 0.01f;

    // Multi-clip check: two clips on one track (0-4 beats, then 6-10 beats,
    // leaving a 2-beat gap and nothing after) must produce sound only inside
    // each clip's own window — real length gating, not the single-clip
    // "loop forever" case checked above. The synth has a 250ms ADSR release
    // tail, so "silence" is checked from 0.5s into the gap/tail onward, well
    // past any legitimate release decay from the last note (whose own note-off
    // already fires before the clip boundary).
    ClipSlot clipA; clipA.pattern = arp; clipA.startBeats = 0.0; clipA.lengthBeats = 4.0;
    ClipSlot clipB; clipB.pattern = arp; clipB.startBeats = 6.0; clipB.lengthBeats = 4.0;
    const auto multiClip = OfflineRenderer::renderClips({ clipA, clipB }, bpm, sampleRate, 6.0);

    const int   halfSec       = (int) sampleRate / 2;
    const float rmsClipA      = multiClip.getRMSLevel(0, 0 * halfSec, 4 * halfSec); // 0-2s: clip A
    const float rmsGap        = multiClip.getRMSLevel(0, 5 * halfSec, 1 * halfSec); // 2.5-3s: late in the gap
    const float rmsClipB      = multiClip.getRMSLevel(0, 6 * halfSec, 4 * halfSec); // 3-5s: clip B
    const float rmsTail       = multiClip.getRMSLevel(0, 11 * halfSec, 1 * halfSec); // 5.5-6s: late in the tail
    const bool  multiClipGates = rmsClipA > 0.01f && rmsGap < 1.0e-5f
                              && rmsClipB > 0.01f && rmsTail < 1.0e-5f;

    // Audio-clip-track check: a decoded audio clip (a plain 440 Hz tone, no
    // synth involved) must play back through a track's audioPlayer, going
    // through the exact same gain pipeline as synth content, with the same
    // clip-start gating. This is the first time a per-track audio clip is
    // actually audible — previously TrackType::Audio tracks were silently inert.
    ClipData sineClip;
    {
        const int n = (int) (2.0 * sampleRate); // 2-second tone
        sineClip.audio.setSize(1, n);
        sineClip.sourceSampleRate = sampleRate;
        sineClip.numChannels      = 1;
        sineClip.lengthSamples    = n;
        float* data = sineClip.audio.getWritePointer(0);
        for (int i = 0; i < n; ++i)
            data[i] = 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate);
    }

    const auto  audioFull       = OfflineRenderer::renderAudioClip(sineClip, 0.0, 0.0f, bpm, sampleRate, 4.0);
    const auto  audioQuiet      = OfflineRenderer::renderAudioClip(sineClip, 0.0, -6.0f, bpm, sampleRate, 4.0);
    const float rmsAudioFull    = audioFull.getRMSLevel(0, 0, audioFull.getNumSamples());
    const float rmsAudioQuiet   = audioQuiet.getRMSLevel(0, 0, audioQuiet.getNumSamples());
    const float audioGainRatio  = rmsAudioFull > 0.0f ? rmsAudioQuiet / rmsAudioFull : 0.0f;

    // Same clip, started 2 beats in (1s at 120bpm): silent before, sounding after.
    const auto  audioDelayed        = OfflineRenderer::renderAudioClip(sineClip, 2.0, 0.0f, bpm, sampleRate, 4.0);
    const float rmsBeforeAudioStart = audioDelayed.getRMSLevel(0, 0, oneSecondSamples);
    const float rmsAfterAudioStart  = audioDelayed.getRMSLevel(0, oneSecondSamples,
                                                               audioDelayed.getNumSamples() - oneSecondSamples);

    const bool audioTrackWorks = rmsAudioFull > 0.01f
                              && audioGainRatio > 0.47f && audioGainRatio < 0.53f
                              && rmsBeforeAudioStart < 1.0e-5f && rmsAfterAudioStart > 0.01f;

    // Multi-clip audio check: two clips on one track (0-4 beats, then 6-10
    // beats, a 2-beat gap, nothing after) must sound only inside each clip's
    // own window — the audio equivalent of multiClipGates above, exercising
    // AudioFilePlayerNode's clip-list scheduling for the first time (a track
    // with one audio clip has always had an unbounded window; this is the
    // first genuine per-clip audio length gating). Audio has no envelope
    // tail (unlike the synth), so it can go silent right at each boundary.
    AudioClipSlot audioClipA;
    audioClipA.clipData    = std::make_shared<ClipData>(sineClip);
    audioClipA.startBeats  = 0.0;
    audioClipA.lengthBeats = 4.0;
    AudioClipSlot audioClipB;
    audioClipB.clipData    = std::make_shared<ClipData>(sineClip);
    audioClipB.startBeats  = 6.0;
    audioClipB.lengthBeats = 4.0;

    const auto  multiAudioClip     = OfflineRenderer::renderAudioClips({ audioClipA, audioClipB },
                                                                       0.0f, bpm, sampleRate, 6.0);
    const float rmsAudioClipA      = multiAudioClip.getRMSLevel(0, 0 * halfSec, 4 * halfSec); // 0-2s: clip A
    const float rmsAudioGap        = multiAudioClip.getRMSLevel(0, 5 * halfSec, 1 * halfSec); // 2.5-3s: the gap
    const float rmsAudioClipB      = multiAudioClip.getRMSLevel(0, 6 * halfSec, 4 * halfSec); // 3-5s: clip B
    const bool  multiClipAudioGates = rmsAudioClipA > 0.01f && rmsAudioGap < 1.0e-5f && rmsAudioClipB > 0.01f;

    // Source-offset check: the same two-second tone played from one second
    // in, in a four-beat (two-second) window. Only the file's last second is
    // left to play, so the window must fall silent halfway through — which it
    // only does if playback really starts at the offset rather than at the
    // start of the file.
    AudioClipSlot audioClipOffset;
    audioClipOffset.clipData            = std::make_shared<ClipData>(sineClip);
    audioClipOffset.startBeats          = 0.0;
    audioClipOffset.lengthBeats         = 4.0;
    audioClipOffset.sourceOffsetSeconds = 1.0;

    const auto  offsetAudioClip   = OfflineRenderer::renderAudioClips({ audioClipOffset },
                                                                      0.0f, bpm, sampleRate, 2.0);
    const float rmsOffsetPlaying  = offsetAudioClip.getRMSLevel(0, 0, 1 * halfSec);         // 0-0.5s
    const float rmsOffsetFinished = offsetAudioClip.getRMSLevel(0, 3 * halfSec, 1 * halfSec); // 1.5-2s
    const bool  sourceOffsetPlays = rmsOffsetPlaying > 0.01f && rmsOffsetFinished < 1.0e-5f;

    // Clip-fade check: the same tone with half-second linear fades at both
    // ends. The first and last eighth of a second sit deep in the fades (a
    // quarter of full level at most), while the middle is untouched. A fade
    // that was never applied, or that ran from the wrong end, fails one side.
    AudioClipSlot audioClipFaded;
    audioClipFaded.clipData         = std::make_shared<ClipData>(sineClip);
    audioClipFaded.startBeats       = 0.0;
    audioClipFaded.lengthBeats      = 4.0;
    audioClipFaded.fades.inSeconds  = 0.5;
    audioClipFaded.fades.outSeconds = 0.5;

    const auto  fadedAudioClip = OfflineRenderer::renderAudioClips({ audioClipFaded },
                                                                   0.0f, bpm, sampleRate, 2.0);
    const float rmsFadeHead    = fadedAudioClip.getRMSLevel(0, 0, halfSec / 4);                          // 0-0.125s
    const float rmsFadeBody    = fadedAudioClip.getRMSLevel(0, 2 * halfSec - halfSec / 2, halfSec);      // 0.75-1.25s
    const float rmsFadeTail    = fadedAudioClip.getRMSLevel(0, 4 * halfSec - halfSec / 4, halfSec / 4);  // 1.875-2s
    const bool  clipFadesShape = rmsFadeBody > 0.3f
                              && rmsFadeHead < 0.25f * rmsFadeBody
                              && rmsFadeTail < 0.25f * rmsFadeBody;

    // MIDI import/export round-trip check: build a Song with three notes on
    // one track, export it to a temp .mid, re-import it into a fresh Song,
    // and confirm every note (and the tempo) survived — beat/pitch/velocity
    // within the rounding tolerance a real tick-based file format implies.
    bool midiRoundTripWorks = false;
    {
        using namespace soundsplice::model;

        Song original;
        original.bpm = 128.0;
        auto& track = addTrack(original, TrackType::Instrument, "Test");
        Clip  clip;
        clip.id                  = allocateId(original);
        clip.type                = ClipType::Instrument;
        clip.startBeats          = 0.0;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        clip.pattern.notes.push_back({ 0.0, 0.5, 60, 0.8f });
        clip.pattern.notes.push_back({ 1.0, 1.0, 64, 0.6f });
        clip.pattern.notes.push_back({ 2.5, 0.25, 67, 1.0f });
        track.clips.push_back(clip);

        const juce::File midiTemp = juce::File::getCurrentWorkingDirectory().getChildFile("midi_roundtrip_test.mid");
        const bool       exportOk = exportMidiFile(midiTemp, original);

        Song reimported;
        reimported.bpm            = 90.0; // deliberately different, so import setting it is actually verified
        const auto importResult   = importMidiFile(midiTemp, reimported);
        midiTemp.deleteFile();

        midiRoundTripWorks = exportOk && importResult.ok && importResult.tracksImported == 1
                          && importResult.extraTempoEventsIgnored == 0
                          && std::abs(reimported.bpm - 128.0) < 0.5
                          && reimported.tracks.size() == 1
                          && reimported.tracks[0].clips.size() == 1;

        if (midiRoundTripWorks)
        {
            const auto& notes = reimported.tracks[0].clips[0].pattern.notes;
            midiRoundTripWorks = notes.size() == 3
                && std::abs(notes[0].startBeats - 0.0) < 0.01 && std::abs(notes[0].lengthBeats - 0.5) < 0.01
                && notes[0].noteNumber == 60 && std::abs(notes[0].velocity - 0.8f) < 0.01f
                && std::abs(notes[1].startBeats - 1.0) < 0.01 && std::abs(notes[1].lengthBeats - 1.0) < 0.01
                && notes[1].noteNumber == 64 && std::abs(notes[1].velocity - 0.6f) < 0.01f
                && std::abs(notes[2].startBeats - 2.5) < 0.01 && std::abs(notes[2].lengthBeats - 0.25) < 0.01
                && notes[2].noteNumber == 67 && std::abs(notes[2].velocity - 1.0f) < 0.01f;
        }
    }


    // MIDI-recording check (see engine/MidiRecorder.h, engine/MidiCapture.h):
    // drives a synthetic performance through the *whole* capture chain the
    // way the app does — blocks pushed at MidiRecorder::process with a
    // count-in to skip, drained from the ring on the message-thread side,
    // sample times converted to beats, paired into notes — and then renders
    // the resulting Pattern through the same synth path every other pattern
    // here goes through.
    //
    // The unit tests already prove the pairing rules in isolation; what they
    // cannot prove is that the produced Pattern is real musical data rather
    // than a struct that only satisfies its own tests. That is what this
    // checks: notes at the beats they were played, audible, and silence where
    // nothing was played.
    bool midiRecordingWorks = false;
    {
        MidiRecorder recorder;

        const int    block        = 512;
        const double samplesPerBeat = 60.0 / bpm * sampleRate;
        const auto   beatToSample = [&](double beat) { return (int64_t) (beat * samplesPerBeat); };

        // One bar of count-in, exactly as AudioEngine::beginMidiRecording
        // computes it — the take must start at the first note actually
        // played, not where the transport was armed.
        const int64_t leadIn = beatToSample(4.0);
        recorder.arm(leadIn);

        // Two notes: one at take-beat 0, one at take-beat 1, each a beat
        // long. Held against the block grid rather than aligned to it, so the
        // block-relative offset arithmetic is genuinely exercised.
        struct Played { double beat; int note; bool on; };
        const std::vector<Played> performance {
            { 0.0, 60, true }, { 1.0, 60, false },
            { 1.0, 67, true }, { 2.0, 67, false },
        };

        std::vector<RecordedMidiEvent> take;
        const int64_t totalSamples = leadIn + beatToSample(3.0);

        for (int64_t playhead = 0; playhead < totalSamples; playhead += block)
        {
            // Whatever falls inside this block, at its offset within it —
            // what AudioEngine::captureMidi hands over each callback.
            std::vector<RecordedMidiEvent> blockEvents;
            for (const auto& played : performance)
            {
                const int64_t at = leadIn + beatToSample(played.beat);
                if (at >= playhead && at < playhead + block)
                    blockEvents.push_back({ at - playhead, played.note, played.on ? 0.8f : 0.0f, played.on });
            }

            recorder.process(blockEvents.data(), (int) blockEvents.size(), block, true, playhead);
            recorder.drain(take); // the app's timer, every tick
        }

        recorder.disarm();
        recorder.process(nullptr, 0, block, true, totalSamples);
        recorder.drain(take);

        const int64_t startSample = recorder.startPlayheadSamples();
        const int64_t endSample   = recorder.endPlayheadSamples();

        // The count-in must have been skipped: capture starts a bar in, not
        // at zero. Compared against the block grid, since capture begins on
        // the first *block* after the lead-in elapses.
        const bool countInSkipped = startSample >= leadIn && startSample < leadIn + block;

        std::vector<TimedMidiEvent> timed;
        const double takeStartBeats = (double) startSample / samplesPerBeat;
        for (const auto& event : take)
            timed.push_back({ (double) event.timeSamples / samplesPerBeat - takeStartBeats,
                              event.noteNumber, event.velocity, event.noteOn });

        const double takeEndBeats = (double) endSample / samplesPerBeat - takeStartBeats;
        auto notes = MidiCapture::notesFromEvents(std::move(timed), takeEndBeats);

        const bool notesPaired = notes.size() == 2
                              && notes[0].noteNumber == 60 && notes[1].noteNumber == 67
                              && std::abs(notes[0].startBeats - 0.0) < 0.05
                              && std::abs(notes[1].startBeats - 1.0) < 0.05
                              && std::abs(notes[0].lengthBeats - 1.0) < 0.05
                              && std::abs(notes[1].lengthBeats - 1.0) < 0.05;

        Pattern recorded;
        recorded.lengthBeats = MidiCapture::clipLengthForTake(takeEndBeats, 4.0);
        recorded.notes       = std::move(notes);

        const auto  recordedBuffer = OfflineRenderer::render({ recorded }, std::vector<float> { 0.0f },
                                                             bpm, sampleRate, 2.0);
        const int   shortWin    = (int) (0.1 * sampleRate);
        const float rmsRecNote1 = recordedBuffer.getRMSLevel(0, 0, shortWin);
        const float rmsRecNote2 = recordedBuffer.getRMSLevel(0, (int) (0.5 * sampleRate), shortWin);
        // Past both notes' one-beat length (they end at 1.0s), so silence
        // here confirms the recorded lengths are real rather than notes
        // running on to the end of the clip.
        const float rmsRecAfter = recordedBuffer.getRMSLevel(0, (int) (1.4 * sampleRate), shortWin);

        midiRecordingWorks = countInSkipped && notesPaired
                          && recorder.droppedEventCount() == 0
                          && rmsRecNote1 > 0.001f && rmsRecNote2 > 0.001f
                          && rmsRecAfter < 0.001f;
    }

    // Pan-automation export check: a curve sweeping hard left to hard right
    // across the render must land the energy on the left early and the right
    // late. Now goes through the same TrackAutomation the live engine uses,
    // rather than a renderer-only callback.
    bool panAutomationWorks = false;
    {
        const double renderSeconds = 4.0;
        const double totalBeats    = renderSeconds * bpm / 60.0;

        TrackAutomation sweep;
        sweep.pan.addPoint(0.0, -1.0f);
        sweep.pan.addPoint(totalBeats, 1.0f);

        const OfflineRenderer::TrackAutomationList curves { sweep };

        const auto swept = OfflineRenderer::render({ arp }, { 0.0f }, {}, {},
                                                   bpm, sampleRate, renderSeconds, 512, &curves);

        const int window = (int) (sampleRate * 0.5);
        const int lateAt = swept.getNumSamples() - window;

        const float earlyLeft  = swept.getRMSLevel(0, 0, window);
        const float earlyRight = swept.getRMSLevel(1, 0, window);
        const float lateLeft   = swept.getRMSLevel(0, lateAt, window);
        const float lateRight  = swept.getRMSLevel(1, lateAt, window);

        panAutomationWorks = earlyLeft > earlyRight * 2.0f && lateRight > lateLeft * 2.0f;
    }

    // Plugin-hosting check, against a *real* plugin rather than a mock: scan
    // whatever effect plugins this machine has, instantiate one, run audio
    // through it as a chain node, and require it to change the signal.
    //
    // A machine with no plugins (CI on Linux, say) is not a failure of this
    // code, so the check passes when none are found — pluginsScanned is
    // printed alongside so it's visible whether anything was actually
    // exercised, rather than the check quietly meaning nothing.
    bool pluginHostWorks = false;
    int  pluginsScanned  = 0;
    {
        juce::ScopedJuceInitialiser_GUI juceInit; // plugin formats want a message loop

        PluginHost host;
        const auto deadMansPedal = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("soundsplice_bounce_plugin_scan.tmp");

        for (const auto& format : host.availableFormats())
            host.scanFormat(format, deadMansPedal, 24); // capped: probing instantiates each one

        deadMansPedal.deleteFile();

        // A stereo effect, not an instrument — something that transforms audio
        // handed to it.
        const PluginEntry* chosen = nullptr;
        const auto entries = host.knownPlugins();
        pluginsScanned = (int) entries.size();
        for (const auto& entry : entries)
        {
            if (! entry.isInstrument && entry.numInputs >= 2 && entry.numOutputs >= 2)
            {
                chosen = &entry;
                break;
            }
        }

        if (chosen == nullptr)
        {
            pluginHostWorks = true; // nothing to host here; not this code's fault
        }
        else
        {
            std::string error;
            auto instance = host.createInstance(chosen->format, chosen->identifier,
                                                sampleRate, 512, &error);
            if (instance == nullptr)
            {
                std::cerr << "plugin instantiation failed: " << error << "\n";
            }
            else
            {
                // Render the same part twice: once plain, once through the
                // plugin as a chain node. A plugin at its defaults might be
                // transparent, so this asserts it *ran* (no crash, buffer
                // intact and finite) and reports whether it altered the sound.
                auto renderThroughPlugin = [&](std::unique_ptr<juce::AudioPluginInstance> plugin,
                                               bool bypassed = false)
                {
                    const int totalSamples = (int) (sampleRate * 1.0);
                    juce::AudioBuffer<float> mix(2, totalSamples);
                    mix.clear();

                    InstrumentTrack track;
                    track.prepare(sampleRate, 512);

                    if (plugin != nullptr)
                    {
                        auto chain = std::make_unique<EffectChain>();
                        auto node  = std::make_unique<PluginNode>(std::move(plugin));
                        node->setBypassed(bypassed);
                        chain->add(std::move(node));
                        chain->prepare(sampleRate, 512);
                        track.setEffectChain(chain.release());
                    }

                    ClipSlot slot;
                    slot.pattern     = arp;
                    slot.startBeats  = 0.0;
                    slot.lengthBeats = 1.0e9;
                    track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

                    juce::MidiBuffer         noLiveMidi;

                    for (int pos = 0; pos < totalSamples; pos += 512)
                    {
                        const int n = std::min(512, totalSamples - pos);

                        ProcessContext context;
                        context.sampleRate                   = sampleRate;
                        context.numSamples                   = n;
                        context.transport.playing            = true;
                        OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                        context.transport.timeSigNumerator   = 4;
                        context.transport.timeSigDenominator = 4;


                        juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                        track.render(blockView, noLiveMidi, context, false, false);
                    }
                    return mix;
                };

                const auto hosted = renderThroughPlugin(std::move(instance));

                // A bypassed plugin must pass the signal through untouched —
                // identical to having no chain at all. Unlike "did the plugin
                // colour the sound", which depends on whichever plugin this
                // machine happened to offer, this is deterministic and tests
                // PluginNode's own bypass path.
                auto second = host.createInstance(chosen->format, chosen->identifier, sampleRate, 512);
                const auto bypassedRender = renderThroughPlugin(std::move(second), true);
                const auto noPluginRender = renderThroughPlugin(nullptr);

                float bypassDelta = 0.0f;
                for (int i = 0; i < bypassedRender.getNumSamples(); ++i)
                    bypassDelta = std::max(bypassDelta,
                                           std::abs(bypassedRender.getSample(0, i)
                                                    - noPluginRender.getSample(0, i)));

                // Every sample must be finite: a plugin writing past its buffer
                // or returning NaN is the failure mode that matters most, since
                // it poisons the whole mix downstream.
                bool  allFinite = true;
                float peak      = 0.0f;
                for (int ch = 0; ch < hosted.getNumChannels() && allFinite; ++ch)
                    for (int i = 0; i < hosted.getNumSamples(); ++i)
                    {
                        const float sample = hosted.getSample(ch, i);
                        if (! std::isfinite(sample)) { allFinite = false; break; }
                        peak = std::max(peak, std::abs(sample));
                    }

                pluginHostWorks = allFinite && peak > 0.0f && bypassDelta < 1.0e-9f;
                std::cerr << "hosted plugin: " << chosen->name << " (" << chosen->format
                          << "), peak=" << peak << ", bypassDelta=" << bypassDelta << "\n";
            }
        }
    }

    // Chain-order check.
    //
    // Note the trap here: the three built-ins (filter, delay, reverb) are all
    // linear and time-invariant, and LTI systems *commute* — filter-then-delay
    // and delay-then-filter produce bit-comparable output (measured: a peak
    // difference of 2e-7, pure float ordering). A first attempt at this check
    // used them and reported "order doesn't matter", which was true and told
    // us nothing about the chain. Reordering the built-ins genuinely won't
    // change the sound, and that's correct DSP rather than a bug.
    //
    // So the mechanism is tested with two deliberately non-commuting nodes —
    // a gain and a hard clip, where halving before clipping differs from
    // clipping before halving. This tests EffectChain, not the DSP.
    // A drive pedal in a real chain: it must audibly change the sound, and the
    // cabinet must audibly change it again. Both are claims about what comes
    // out of the speakers, so both are measured here rather than only in the
    // headless DSP tests, which exercise the shaper in isolation.
    bool driveChangesSound  = false;
    bool driveCabinetWorks  = false;
    {
        auto renderDrive = [&](int layout) // 0 = none, 1 = drive+cab, 2 = drive, no cab
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            if (layout != 0)
            {
                auto chain = std::make_unique<EffectChain>();
                auto node  = std::make_unique<DriveNode>();
                node->effect.setEnabled(true);
                node->effect.setDrive(12.0f);
                node->effect.setTone(0.5f);
                node->effect.setLevel(0.8f);
                node->effect.setCabinet(layout == 1);
                chain->add(std::move(node));
                chain->prepare(sampleRate, 512);
                track.setEffectChain(chain.release());
            }

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto worstDifference = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto clean     = renderDrive(0);
        const auto withCab   = renderDrive(1);
        const auto noCab     = renderDrive(2);

        driveChangesSound = clean.getRMSLevel(0, 0, clean.getNumSamples()) > 1.0e-4f
                         && worstDifference(clean, withCab) > 1.0e-3f;

        // The cabinet is the difference between distortion and fizz, so it
        // has to actually be in the path rather than merely stored.
        driveCabinetWorks = worstDifference(withCab, noCab) > 1.0e-3f;
    }

    // Cascaded gain stages (docs/PLAN.md §33 phase 3).
    //
    // The first version of this check asserted the cascade *compresses* more,
    // and it measured the opposite: with the drive shared out as the n-th root
    // so the total push stays put, three gentle stages have a softer composite
    // knee than one hard one, so the output grows *more* freely, not less.
    // That was a wrong claim, not a wrong implementation.
    //
    // What a cascade really does is multiply harmonics: each stage distorts a
    // signal that already has harmonics, so the spectrum gets denser rather
    // than merely louder. That is measurable, and it is the reason to cascade.
    bool cascadedStagesEnrich = false;
    {
        auto renderDrive = [&](int stages)
        {
            const int totalSamples = (int) (sampleRate * 0.5);
            juce::AudioBuffer<float> driveBuffer(1, totalSamples);

            for (int i = 0; i < totalSamples; ++i)
                driveBuffer.setSample(0, i, 0.3f * (float) std::sin(
                    2.0 * juce::MathConstants<double>::pi * 220.0 * i / sampleRate));

            DriveEffect drive;
            drive.prepare(sampleRate, 512);
            drive.setEnabled(true);
            drive.setDrive(12.0f);
            drive.setTone(0.5f);
            drive.setLevel(1.0f);
            drive.setHardClip(false);
            drive.setCabinet(false); // linear, and only muddies the measurement
            drive.setStages(stages);
            drive.process(driveBuffer);

            std::vector<float> out((size_t) totalSamples);
            for (int i = 0; i < totalSamples; ++i)
                out[(size_t) i] = driveBuffer.getSample(0, i);
            return out;
        };

        // Harmonics 5..9 against the fundamental — the high-order content that
        // only appears once something distorts an already-distorted signal.
        auto densityOf = [&](const std::vector<float>& signal)
        {
            const int from = (int) (0.1 * sampleRate), count = (int) (0.3 * sampleRate);

            auto binAt = [&](double frequency)
            {
                double real = 0.0, imaginary = 0.0;
                for (int i = 0; i < count; ++i)
                {
                    const double angle = 2.0 * juce::MathConstants<double>::pi
                                       * frequency * (double) i / sampleRate;
                    real      += signal[(size_t) (from + i)] * std::cos(angle);
                    imaginary += signal[(size_t) (from + i)] * std::sin(angle);
                }
                return std::hypot(real, imaginary) / (double) count;
            };

            const double fundamental = binAt(220.0);
            double       upper       = 0.0;
            for (int harmonic = 5; harmonic <= 9; ++harmonic)
                upper += binAt(220.0 * harmonic);

            return fundamental > 0.0 ? upper / fundamental : 0.0;
        };

        const auto single  = renderDrive(1);
        const auto cascade = renderDrive(3);

        const double singleDensity  = densityOf(single);
        const double cascadeDensity = densityOf(cascade);

        std::cout << "drive stages: 1x density=" << singleDensity
                  << " 3x density=" << cascadeDensity << "\n";

        cascadedStagesEnrich = singleDensity > 0.0
                            && cascadeDensity > singleDensity * 1.1;
    }

    // Compressor and tremolo in a real chain. Both are claims about what comes
    // out of the speakers, so both are measured here and not only headless.
    bool compressorSquashes = false;
    bool tremoloModulates   = false;
    {
        auto renderPedal = [&](int layout) // 0 = clean, 1 = compressor, 2 = tremolo
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            if (layout != 0)
            {
                auto chain = std::make_unique<EffectChain>();
                if (layout == 1)
                {
                    auto node = std::make_unique<CompressorNode>();
                    node->effect.setEnabled(true);
                    node->effect.setThresholdDb(-40.0f); // well under the arp, so it bites
                    node->effect.setRatio(12.0f);
                    node->effect.setAttackMs(1.0f);
                    node->effect.setReleaseMs(80.0f);
                    chain->add(std::move(node));
                }
                else
                {
                    auto node = std::make_unique<TremoloNode>();
                    node->effect.setEnabled(true);
                    node->effect.setRateHz(6.0f);
                    node->effect.setDepth(1.0f);
                    chain->add(std::move(node));
                }
                chain->prepare(sampleRate, 512);
                track.setEffectChain(chain.release());
            }

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        const auto clean      = renderPedal(0);
        const auto compressed = renderPedal(1);
        const auto tremmed    = renderPedal(2);

        // Compression must reduce the peak while leaving the part audible —
        // a "compressor" that merely turned everything down would pass a
        // peak test alone, so the RMS floor is checked too.
        const float cleanPeak = clean.getMagnitude(0, 0, clean.getNumSamples());
        const float compPeak  = compressed.getMagnitude(0, 0, compressed.getNumSamples());
        compressorSquashes = cleanPeak > 1.0e-3f && compPeak < cleanPeak * 0.9f
                          && compressed.getRMSLevel(0, 0, compressed.getNumSamples()) > 1.0e-5f;

        // Comparing the quietest window of each render would prove nothing:
        // the arp has near-silent gaps of its own, so the clean render's
        // quietest window is already ~0 and no dip can beat it. Instead,
        // compare the two renders window by window and only over windows the
        // clean one actually fills — that isolates what the tremolo did from
        // what the part was doing anyway.
        const int   window     = (int) (sampleRate * 0.02);
        const float cleanPeakW = clean.getMagnitude(0, 0, clean.getNumSamples());

        float deepestDip = 1.0f;
        int   loudWindows = 0;
        for (int start = 0; start + window <= clean.getNumSamples(); start += window)
        {
            const float cleanLevel = clean.getMagnitude(0, start, window);
            if (cleanLevel < cleanPeakW * 0.5f)
                continue; // the part isn't playing here; nothing to modulate

            ++loudWindows;
            deepestDip = std::min(deepestDip, tremmed.getMagnitude(0, start, window) / cleanLevel);
        }

        // Somewhere in a second at 6Hz, a full-depth tremolo has to have taken
        // a loud passage most of the way to silence.
        tremoloModulates = loudWindows > 4 && deepestDip < 0.3f;
    }

    // The mastering rack, end to end through the real processor. Two claims
    // that a per-stage unit test can't make: that the rack as assembled
    // changes the sound at all, and that its limiter's ceiling survives
    // everything upstream of it — the EQ, exciter, widener and reverb all
    // add level, and a ceiling that only holds in isolation isn't one.
    bool masteringChangesSound   = false;
    bool masteringHoldsCeiling   = false;
    {
        auto makeMix = [&]
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> buf(2, totalSamples);
            for (int n = 0; n < totalSamples; ++n)
            {
                // Loud, broadband and transient-heavy: a tone plus periodic
                // full-scale spikes, which is what actually tests a limiter.
                const float tone  = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi
                                                    * 220.0f * (float) n / (float) sampleRate);
                const float spike = (n % 4000 < 40) ? 0.95f : 0.0f;
                buf.setSample(0, n, tone + spike);
                buf.setSample(1, n, tone - spike * 0.7f); // not identical, so width has something to act on
            }
            return buf;
        };

        auto runRack = [&](const soundsplice::model::MasteringSettings& settings)
        {
            auto rackBuffer = makeMix();

            MasteringProcessor rack;
            rack.prepare(sampleRate, 512);
            rack.setEnabled(settings.enabled);
            rack.setLowShelf(settings.lowShelfHz, settings.lowShelfDb);
            rack.setPeak(settings.peakHz, settings.peakDb, settings.peakQ);
            rack.setHighShelf(settings.highShelfHz, settings.highShelfDb);
            rack.setExciter(settings.exciterAmount, settings.exciterCrossoverHz);
            rack.setWidth(settings.width);
            rack.setReverb(settings.reverbAmount, settings.reverbRoomSize);
            rack.setMaximizer(settings.maximizerInputDb, settings.maximizerCeilingDb,
                              settings.maximizerReleaseMs);
            rack.setOutputGainDb(settings.outputGainDb);

            for (int pos = 0; pos < rackBuffer.getNumSamples(); pos += 512)
            {
                const int n = std::min(512, rackBuffer.getNumSamples() - pos);
                juce::AudioBuffer<float> view(rackBuffer.getArrayOfWritePointers(), 2, pos, n);
                rack.process(view);
            }
            return rackBuffer;
        };

        const auto dry = runRack(soundsplice::model::MasteringSettings {}); // disabled: must be untouched
        const auto raw = makeMix();
        masteringChangesSound = worstBufferDifference(dry, raw) < 1.0e-9f; // bypassed really is bypassed

        // Every preset's ceiling must hold on this deliberately nasty input.
        masteringHoldsCeiling = true;
        for (int i = 0; i < soundsplice::engine::kNumMasteringPresets; ++i)
        {
            const auto preset   = (soundsplice::engine::MasteringPreset) i;
            const auto settings = soundsplice::model::presetForMastering(preset);
            const auto out      = runRack(settings);

            const float peak    = out.getMagnitude(0, 0, out.getNumSamples());
            const float ceiling = std::pow(10.0f, settings.maximizerCeilingDb / 20.0f);

            if (settings.enabled)
            {
                // Output gain is applied after the limiter, so the ceiling it
                // guarantees is scaled by it too.
                const float allowed = ceiling * std::pow(10.0f, settings.outputGainDb / 20.0f) + 1.0e-3f;
                if (peak > allowed)
                {
                    masteringHoldsCeiling = false;
                    std::cout << "  ceiling breached by " << soundsplice::engine::masteringPresetName(preset)
                              << ": peak=" << peak << " allowed=" << allowed << "\n";
                }

                // And it must actually be doing something.
                if (worstBufferDifference(out, raw) < 1.0e-4f)
                {
                    masteringChangesSound = false;
                    std::cout << "  " << soundsplice::engine::masteringPresetName(preset)
                              << " changed nothing\n";
                }
            }
        }

        std::cout << "mastering: bypassIsClean=" << (masteringChangesSound ? 1 : 0)
                  << " ceilingHolds=" << (masteringHoldsCeiling ? 1 : 0) << "\n";
    }

    // A gate is only worth having if it actually quiets a noisy tail between
    // notes rather than merely being wired in — rendered directly through
    // GateEffect (not a whole track) since the claim is about the DSP node
    // itself, not about anything upstream of it.
    bool gateClosesQuiet = false;
    {
        const int burstSamples  = (int) (sampleRate * 0.1);  // a loud 100ms note
        const int tailSamples   = (int) (sampleRate * 1.0);  // then quiet "noise"
        const int totalSamples  = burstSamples + tailSamples;

        // The gate doesn't slam shut - hold plus release take real time to
        // bring it down to -rangeDb, so the RMS comparison is taken from the
        // settled back half of the tail rather than the whole thing, or the
        // still-closing front half would wash out the difference.
        const int settleSamples = (int) (sampleRate * 0.5);

        juce::Random rng(1234);
        auto makeSource = [&]
        {
            juce::AudioBuffer<float> buf(2, totalSamples);
            for (int n = 0; n < totalSamples; ++n)
            {
                const float sample = n < burstSamples
                    ? std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * (float) n / (float) sampleRate)
                    : (rng.nextFloat() * 2.0f - 1.0f) * 0.02f; // low-level hiss, well under threshold
                buf.setSample(0, n, sample);
                buf.setSample(1, n, sample);
            }
            return buf;
        };

        auto tailRms = [&](const juce::AudioBuffer<float>& buf)
        {
            const int start = burstSamples + settleSamples;
            return buf.getRMSLevel(0, start, totalSamples - start);
        };

        auto gateOff = makeSource();
        auto gateOn  = makeSource();

        GateEffect gate;
        gate.prepare(sampleRate, 512);
        gate.setEnabled(true);
        gate.setThresholdDb(-30.0f);
        gate.setRangeDb(60.0f);
        gate.setAttackMs(0.5f);
        gate.setHoldMs(15.0f);
        gate.setReleaseMs(60.0f);

        for (int pos = 0; pos < totalSamples; pos += 512)
        {
            const int n = std::min(512, totalSamples - pos);
            juce::AudioBuffer<float> block(gateOn.getArrayOfWritePointers(), 2, pos, n);
            gate.process(block);
        }

        const float offTailRms = tailRms(gateOff);
        const float onTailRms  = tailRms(gateOn);
        gateClosesQuiet = offTailRms > 1.0e-4f && onTailRms < offTailRms * 0.1f;
    }

    // The chorus in a real chain: it must audibly change the sound, and its
    // depth must audibly change it again — a chorus whose sweep did nothing
    // would be a fixed comb filter wearing the name.
    bool chorusChangesSound = false;
    bool chorusDepthMatters = false;
    {
        auto renderChorus = [&](int layout) // 0 = clean, 1 = swept, 2 = depth zero
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            if (layout != 0)
            {
                auto chain = std::make_unique<EffectChain>();
                auto node  = std::make_unique<ChorusNode>();
                node->effect.setEnabled(true);
                node->effect.setRateHz(3.0f);
                node->effect.setMix(1.0f);
                node->effect.setDepth(layout == 1 ? 1.0f : 0.0f);
                chain->add(std::move(node));
                chain->prepare(sampleRate, 512);
                track.setEffectChain(chain.release());
            }

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto worstDifference = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto clean  = renderChorus(0);
        const auto swept  = renderChorus(1);
        const auto static_ = renderChorus(2);

        chorusChangesSound = clean.getRMSLevel(0, 0, clean.getNumSamples()) > 1.0e-4f
                          && worstDifference(clean, swept) > 1.0e-3f;

        // Depth zero is a fixed comb; depth one sweeps. If they matched, the
        // modulation — the whole effect — would not be in the signal path.
        chorusDepthMatters = worstDifference(swept, static_) > 1.0e-3f;
    }

    // The wobble in a real chain: it must audibly change the sound, its depth
    // must audibly change it again, and — the point of the whole pedal —
    // rendering the same passage at two different tempos must produce
    // different audio even though nothing else changed. That last one is the
    // only thing here that actually proves InstrumentTrack::render is pushing
    // ProcessContext::transport.bpm into the chain via EffectChain::setBpm; a
    // wobble that silently ignored bpm would pass every other check in this
    // file and still be wrong.
    bool wobbleChangesSound = false;
    bool wobbleDepthMatters = false;
    bool wobbleTracksTempo  = false;
    {
        auto renderWobble = [&](int layout, double bpmForRender) // 0 = clean, 1 = swept, 2 = depth zero
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            if (layout != 0)
            {
                auto chain = std::make_unique<EffectChain>();
                auto node  = std::make_unique<WobbleNode>();
                node->effect.setEnabled(true);
                node->effect.setRateInBeats(1.0f);
                node->effect.setMix(1.0f);
                node->effect.setDepth(layout == 1 ? 1.0f : 0.0f);
                chain->add(std::move(node));
                chain->prepare(sampleRate, 512);
                track.setEffectChain(chain.release());
            }

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpmForRender, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto worstDifference = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto clean   = renderWobble(0, bpm);
        const auto swept   = renderWobble(1, bpm);
        const auto static_ = renderWobble(2, bpm);

        wobbleChangesSound = clean.getRMSLevel(0, 0, clean.getNumSamples()) > 1.0e-4f
                          && worstDifference(clean, swept) > 1.0e-3f;

        // Depth zero is a fixed low-pass; depth one sweeps. If they matched,
        // the sweep — the whole effect — would not be in the signal path.
        wobbleDepthMatters = worstDifference(swept, static_) > 1.0e-3f;

        // Same rate-in-beats, only the tempo differs — the sweep must land
        // at different points in the audio at each tempo. Fed through the
        // audio-clip path (AudioFilePlayerNode) rather than the sequencer:
        // a clip starting at beat 0 plays at a position that's purely
        // sample-based (see AudioFilePlayerNode::process — its window and
        // read position both reduce to the raw sample count when
        // startBeats is 0), so changing bpm here doesn't also shift when
        // anything sounds, the way it would through a beat-scheduled MIDI
        // pattern. That keeps this test isolated to one thing: whether
        // InstrumentTrack::render actually pushes context.transport.bpm into
        // the chain via EffectChain::setBpm.
        auto renderWobbleAt = [&](double bpmForRender)
        {
            const int totalSamples = (int) sampleRate; // one second
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            ClipData tone;
            tone.audio.setSize(1, totalSamples);
            tone.sourceSampleRate = sampleRate;
            tone.numChannels      = 1;
            tone.lengthSamples    = totalSamples;
            float* data = tone.audio.getWritePointer(0);
            for (int n = 0; n < totalSamples; ++n)
                data[n] = (float) std::sin(2.0 * 3.14159265358979 * 100.0 * n / sampleRate);

            InstrumentTrack track;
            track.prepare(sampleRate, 512);
            track.audioPlayer.submitSingleClip(new ClipData(tone), 0.0);

            auto chain = std::make_unique<EffectChain>();
            auto node  = std::make_unique<WobbleNode>();
            node->effect.setEnabled(true);
            node->effect.setRateInBeats(1.0f);
            node->effect.setMix(1.0f);
            node->effect.setDepth(1.0f);
            chain->add(std::move(node));
            chain->prepare(sampleRate, 512);
            track.setEffectChain(chain.release());

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpmForRender, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        const auto atBpm       = renderWobbleAt(bpm);
        const auto atDoubleBpm = renderWobbleAt(bpm * 2.0);
        wobbleTracksTempo = worstDifference(atBpm, atDoubleBpm) > 1.0e-3f;
    }

    // New synth DSP (see engine::SynthVoice / SynthVoiceSettings): the filter
    // envelope, the sub-oscillator, and unison. Each renders one sustained
    // note through InstrumentTrack's real synth path with the capability off
    // vs. on (everything else identical) and asserts the two renders differ -
    // the same "does the parameter do something" pattern chorusChangesSound/
    // wobbleChangesSound above already use. Every default (filterEnvAmount 0,
    // subOscEnabled false, unisonVoices 1) takes SynthVoice's original,
    // untouched fast path — which is exactly what rmsDry and every other
    // sentinel above staying unchanged after this feature landed confirms.
    bool filterEnvChangesSound = false;
    bool subOscChangesSound    = false;
    bool unisonChangesSound    = false;
    {
        auto renderSustainedNote = [&](const std::function<void(SynthInstrumentNode&)>& configure)
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);
            track.synth.setWaveform(1); // saw - real harmonic content to filter/detune
            track.synth.setAttackMs(2.0f);
            track.synth.setDecayMs(50.0f);
            track.synth.setSustain(1.0f);
            track.synth.setReleaseMs(50.0f);
            configure(track.synth);

            Pattern single;
            single.lengthBeats = 4.0;
            single.notes.push_back({ 0.0, 4.0, 45, 0.9f }); // one long note, well under Nyquist

            ClipSlot slot;
            slot.pattern     = single;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto worstDiff = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto filterEnvOff = renderSustainedNote([](SynthInstrumentNode& synth)
        {
            synth.setFilterEnabled(true);
            synth.setFilterCutoff(300.0f);
        });
        const auto filterEnvOn = renderSustainedNote([](SynthInstrumentNode& synth)
        {
            synth.setFilterEnabled(true);
            synth.setFilterCutoff(300.0f);
            synth.setFilterEnvAmount(4000.0f);
            synth.setFilterEnvAttackMs(1.0f);
            synth.setFilterEnvDecayMs(200.0f);
            synth.setFilterEnvSustain(0.1f);
            synth.setFilterEnvReleaseMs(50.0f);
        });
        filterEnvChangesSound = filterEnvOff.getRMSLevel(0, 0, filterEnvOff.getNumSamples()) > 1.0e-4f
                             && worstDiff(filterEnvOff, filterEnvOn) > 1.0e-3f;

        const auto subOscOff = renderSustainedNote([](SynthInstrumentNode&) {});
        const auto subOscOn  = renderSustainedNote([](SynthInstrumentNode& synth)
        {
            synth.setSubOscEnabled(true);
            synth.setSubOscLevel(0.5f);
        });
        subOscChangesSound = worstDiff(subOscOff, subOscOn) > 1.0e-3f;

        const auto unisonOff = renderSustainedNote([](SynthInstrumentNode&) {});
        const auto unisonOn  = renderSustainedNote([](SynthInstrumentNode& synth)
        {
            synth.setUnisonVoices(5);
            synth.setUnisonDetuneCents(20.0f);
        });
        unisonChangesSound = worstDiff(unisonOff, unisonOn) > 1.0e-3f;
    }

    bool effectChainOrderMatters = false;
    bool effectChainRunsAllNodes = false;
    {
        struct GainNode final : EffectProcessor
        {
            EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
            void prepare(double, int) override {}
            void setEnabled(bool) override {}
            void process(juce::AudioBuffer<float>& buffer) override { buffer.applyGain(0.25f); }
        };

        struct ClipNode final : EffectProcessor
        {
            EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
            void prepare(double, int) override {}
            void setEnabled(bool) override {}
            void process(juce::AudioBuffer<float>& buffer) override
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        buffer.setSample(ch, i, juce::jlimit(-0.02f, 0.02f, buffer.getSample(ch, i)));
            }
        };

        // 0 = gain then clip, 1 = clip then gain, 2 = gain only.
        auto renderChain = [&](int layout)
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            auto chain = std::make_unique<EffectChain>();
            if (layout == 0)      { chain->add(std::make_unique<GainNode>()); chain->add(std::make_unique<ClipNode>()); }
            else if (layout == 1) { chain->add(std::make_unique<ClipNode>()); chain->add(std::make_unique<GainNode>()); }
            else                  { chain->add(std::make_unique<GainNode>()); }
            chain->prepare(sampleRate, 512);
            track.setEffectChain(chain.release());

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto peakDifference = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto gainThenClip = renderChain(0);
        const auto clipThenGain = renderChain(1);
        const auto gainOnly     = renderChain(2);

        effectChainOrderMatters = gainThenClip.getRMSLevel(0, 0, gainThenClip.getNumSamples()) > 1.0e-4f
                               && peakDifference(gainThenClip, clipThenGain) > 1.0e-3f;

        // ...and a two-node chain must differ from a one-node chain, or the
        // second node isn't being run at all.
        effectChainRunsAllNodes = peakDifference(gainThenClip, gainOnly) > 1.0e-3f;
    }

    // Session-launch check: the whole point of the session grid is that a clip
    // launched mid-bar starts at the *next bar line*, not immediately. Renders
    // one track whose session slot is launched a fraction of a bar in, and
    // requires silence until the boundary and sound after it.
    bool sessionLaunchQuantizes = false;
    bool sessionStopWorks       = false;
    {
        const double samplesPerBeat = sampleRate * 60.0 / bpm;
        const double barSamples     = samplesPerBeat * 4.0; // 4/4

        // A clip that hits on every beat, so "is it sounding" is easy to read.
        Pattern sessionPattern;
        sessionPattern.lengthBeats = 4.0;
        for (int i = 0; i < 4; ++i)
            sessionPattern.notes.push_back({ (double) i, 0.5, 60, 0.9f });

        auto renderSession = [&](bool stopAfterFirstBar)
        {
            const int totalSamples = (int) (barSamples * 3.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            auto* slots = new SessionPlayer::SlotList();
            slots->push_back({ true, sessionPattern });
            track.session.submitSlots(slots);

            juce::MidiBuffer         noLiveMidi;
            bool                     launched = false, stopped = false;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                // Launch a quarter of the way into the first bar: the clip
                // must not start here, but at the bar line that follows.
                if (! launched && pos >= (int) (barSamples * 0.25))
                {
                    track.session.requestLaunch(0);
                    launched = true;
                }
                if (stopAfterFirstBar && ! stopped && pos >= (int) (barSamples * 1.25))
                {
                    track.session.requestStop();
                    stopped = true;
                }

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false, barSamples);
            }
            return mix;
        };

        const auto launchedMix = renderSession(false);

        // Just before the bar line the clip must still be silent; just after,
        // sounding. A launch that ignored quantization would fill both.
        const int   probe        = (int) (sampleRate * 0.15);
        const float beforeLaunch = launchedMix.getRMSLevel(0, (int) (barSamples * 0.5), probe);
        const float afterLaunch  = launchedMix.getRMSLevel(0, (int) barSamples + 1000, probe);

        sessionLaunchQuantizes = beforeLaunch < 1.0e-6f && afterLaunch > 0.01f;

        // Stopping mid-bar likewise takes effect at the next bar line: asked
        // for a quarter into bar 1, it happens at bar 2. So the clip is still
        // sounding halfway through bar 1 and gone by bar 2.5 (the track has no
        // arrangement clips here to fall back to).
        const auto  stoppedMix  = renderSession(true);
        const float beforeStop  = stoppedMix.getRMSLevel(0, (int) (barSamples * 1.5), probe);
        const float afterStop   = stoppedMix.getRMSLevel(0, (int) (barSamples * 2.5), probe);

        sessionStopWorks = beforeStop > 0.01f && afterStop < 1.0e-6f;
    }

    // Per-track pan check: the same part hard-panned left must vanish from
    // the right channel while staying present on the left, and a centred
    // track must be identical on both — the unity-centre pan law is what
    // keeps every existing project's balance unchanged.
    bool trackPanWorks = false;
    {
        auto renderPanned = [&](float panPosition)
        {
            const int totalSamples = (int) (sampleRate * 2.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);
            track.pan.store(panPosition);

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }

            return std::make_pair(mix.getRMSLevel(0, 0, totalSamples), mix.getRMSLevel(1, 0, totalSamples));
        };

        const auto centred = renderPanned(0.0f);
        const auto left    = renderPanned(-1.0f);

        trackPanWorks = centred.first > 0.01f
                     && std::abs(centred.first - centred.second) < 1.0e-6f // centre is balanced
                     && left.first > 0.01f                                  // still there on the left
                     && left.second < 1.0e-6f                               // gone from the right
                     && std::abs(left.first - centred.first) < 1.0e-6f;     // and unchanged in level
    }

    // Per-track insert check: the same part rendered through one track twice,
    // once with that track's own insert low-pass enabled well below the note
    // content. Proves the insert chain is actually in the per-track path —
    // the existing rmsDry sentinel already proves the other half, that
    // *disabled* inserts leave the signal bit-identical, since OfflineRenderer
    // renders through InstrumentTrack and now runs three (bypassed) inserts
    // per block.
    bool trackInsertFilterWorks = false;
    {
        auto renderOneTrack = [&](bool filterEnabled)
        {
            const int totalSamples = (int) (sampleRate * 2.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);
            auto chain = std::make_unique<EffectChain>();
            auto filter = std::make_unique<FilterNode>();
            filter->effect.setEnabled(filterEnabled);
            filter->effect.setMode(0); // low-pass
            filter->effect.setCutoff(150.0f);
            filter->effect.setResonance(0.707f);
            chain->add(std::move(filter));
            chain->prepare(sampleRate, 512);
            track.setEffectChain(chain.release());

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                = sampleRate;
                context.numSamples                = n;
                context.transport.playing         = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;


                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, noLiveMidi, context, false, false);
            }

            return mix.getRMSLevel(0, 0, totalSamples);
        };

        const float plain    = renderOneTrack(false);
        const float filtered = renderOneTrack(true);

        trackInsertFilterWorks = plain > 0.01f && filtered < plain * 0.9f;
    }

    // Metronome check: at 120bpm a beat lands every 0.5s, so the click must
    // be audible right at each beat and silent between them. Also confirms
    // the accent logic runs without disturbing the beat grid.
    bool metronomeWorks = false;
    {
        Metronome metronome;
        metronome.prepare(sampleRate);
        metronome.setEnabled(true);
        metronome.setLevel(1.0f);

        const int totalSamples = (int) (sampleRate * 2.0);
        juce::AudioBuffer<float> clickBuffer(2, totalSamples);
        clickBuffer.clear();

        constexpr int blockSize = 512;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);

            ProcessContext context;
            context.sampleRate                 = sampleRate;
            context.numSamples                 = n;
            context.transport.playing          = true;
            OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
            context.transport.timeSigNumerator = 4;
            context.transport.timeSigDenominator = 4;

            // A view onto this block of the output, so the click accumulates
            // into one buffer exactly as it does in the live callback.
            juce::AudioBuffer<float> blockView(clickBuffer.getArrayOfWritePointers(), 2, pos, n);
            metronome.process(blockView, context, false);
        }

        const int   win        = (int) (0.02 * sampleRate);
        const float atBeat0    = clickBuffer.getRMSLevel(0, 0, win);
        const float atBeat1    = clickBuffer.getRMSLevel(0, (int) (0.5 * sampleRate), win);
        const float betweenHit = clickBuffer.getRMSLevel(0, (int) (0.25 * sampleRate), win);

        metronomeWorks = atBeat0 > 0.01f && atBeat1 > 0.01f && betweenHit < 1.0e-6f;
    }

    // ...and that a disabled metronome is completely silent, which is what
    // keeps it out of an export (OfflineRenderer has no metronome at all, so
    // a bounce can never contain one — this guards the live path).
    bool metronomeSilentWhenOff = false;
    {
        Metronome metronome;
        metronome.prepare(sampleRate);
        metronome.setEnabled(false);

        const int totalSamples = (int) (sampleRate * 1.0);
        juce::AudioBuffer<float> quietBuffer(2, totalSamples);
        quietBuffer.clear();

        ProcessContext context;
        context.sampleRate                   = sampleRate;
        context.numSamples                   = totalSamples;
        context.transport.playing            = true;
        context.transport.playheadSamples    = 0;
        context.transport.bpm                = bpm;
        context.transport.timeSigNumerator   = 4;
        context.transport.timeSigDenominator = 4;
        metronome.process(quietBuffer, context, false);

        metronomeSilentWhenOff = quietBuffer.getRMSLevel(0, 0, totalSamples) < 1.0e-9f;
    }

    const juce::File out = juce::File::getCurrentWorkingDirectory()
                               .getChildFile(argc > 1 ? argv[1] : "bounce.wav");

    // Delay check: applying the master delay must change the signal.
    juce::AudioBuffer<float> wet(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        wet.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    DelayEffect delay;
    delay.prepare(sampleRate, 512);
    delay.setEnabled(true);
    delay.setTimeMs(250.0f);
    delay.setFeedback(0.4f);
    delay.setMix(0.5f);
    delay.process(wet);

    const float rmsDry       = buffer.getRMSLevel(0, 0, buffer.getNumSamples());
    const float rmsWet       = wet.getRMSLevel(0, 0, wet.getNumSamples());
    const bool  delayChanged = std::abs(rmsWet - rmsDry) > 1.0e-4f;

    // Filter check: a low-pass well below the note content should reduce the level.
    juce::AudioBuffer<float> filtered(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        filtered.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    FilterEffect filter;
    filter.prepare(sampleRate, 512);
    filter.setEnabled(true);
    filter.setMode(0); // low-pass
    filter.setCutoff(150.0f);
    filter.setResonance(0.707f);
    filter.process(filtered);

    const float rmsFiltered      = filtered.getRMSLevel(0, 0, filtered.getNumSamples());
    const bool  filterAttenuates = rmsFiltered < rmsDry;

    // Reverb check: enabling reverb must change the signal.
    juce::AudioBuffer<float> reverbed(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        reverbed.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    ReverbEffect reverb;
    reverb.prepare(sampleRate, 512);
    reverb.setEnabled(true);
    reverb.setRoomSize(0.7f);
    reverb.setDamping(0.4f);
    reverb.setMix(0.4f);
    reverb.process(reverbed);

    const float rmsReverbed  = reverbed.getRMSLevel(0, 0, reverbed.getNumSamples());
    const bool  reverbChanged = std::abs(rmsReverbed - rmsDry) > 1.0e-4f;

    // Automation check: a -40 dB -> 0 dB master-gain ramp should fade the clip in.
    juce::AudioBuffer<float> automated(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        automated.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    soundsplice::model::AutomationLane lane;
    lane.addPoint(0.0, -40.0f);
    lane.addPoint(bpm / 60.0 * seconds, 0.0f);

    const double samplesPerBeat = sampleRate * 60.0 / bpm;
    for (int i = 0; i < automated.getNumSamples(); ++i)
    {
        const double beat = (double) i / samplesPerBeat;
        const float  g    = juce::Decibels::decibelsToGain(lane.valueAt(beat, 0.0f));
        for (int ch = 0; ch < automated.getNumChannels(); ++ch)
            automated.getWritePointer(ch)[i] *= g;
    }

    const int   half            = automated.getNumSamples() / 2;
    const float rmsFirstHalf    = automated.getRMSLevel(0, 0, half);
    const float rmsSecondHalf   = automated.getRMSLevel(0, half, automated.getNumSamples() - half);
    const bool  automationFades = rmsFirstHalf < rmsSecondHalf;

    // Per-track automation check: unlike the master-gain trick above (a plain
    // post-render multiply, since master gain applies uniformly to the whole
    // mix), per-track automation can't be applied after tracks are already
    // summed — this exercises the real OfflineRenderer::render(automation)
    // path. Track 1 (arp) gets a -40 dB -> 0 dB fade; track 2 (bass) gets none.
    TrackAutomation arpAutomation;
    arpAutomation.gain.addPoint(0.0, -40.0f);
    arpAutomation.gain.addPoint(bpm / 60.0 * seconds, 0.0f);
    const TrackAutomation noAutomation; // empty: bass keeps its static gain

    const OfflineRenderer::TrackAutomationList perTrackCurves { arpAutomation, noAutomation };

    // Isolate each track (the other silenced at -100 dB) so the comparison
    // below reflects one track's automation state, not the fixed two-track mix.
    const auto arpAloneAutomated  = OfflineRenderer::render({ arp, bass }, { 0.0f, -100.0f }, std::vector<bool>{},
                                                            std::vector<double>{},
                                                            bpm, sampleRate, seconds, 512, &perTrackCurves);
    const auto bassAloneNoAuto    = OfflineRenderer::render({ arp, bass }, { -100.0f, 0.0f }, std::vector<bool>{},
                                                            std::vector<double>{},
                                                            bpm, sampleRate, seconds, 512, &perTrackCurves);

    const int   halfArp             = arpAloneAutomated.getNumSamples() / 2;
    const float rmsArpFirstHalf     = arpAloneAutomated.getRMSLevel(0, 0, halfArp);
    const float rmsArpSecondHalf    = arpAloneAutomated.getRMSLevel(0, halfArp, arpAloneAutomated.getNumSamples() - halfArp);
    const bool  perTrackAutoFades   = rmsArpFirstHalf < rmsArpSecondHalf;

    const int   halfBass            = bassAloneNoAuto.getNumSamples() / 2;
    const float rmsBassFirstHalf    = bassAloneNoAuto.getRMSLevel(0, 0, halfBass);
    const float rmsBassSecondHalf   = bassAloneNoAuto.getRMSLevel(0, halfBass, bassAloneNoAuto.getNumSamples() - halfBass);
    // A loose tolerance: two loop iterations of the same pattern aren't
    // perfectly identical (envelope/voice state carries small differences
    // across the loop boundary), but a real automation leak would show up as
    // a multiple, not ~10-15% — the arp check above swings 10x for contrast.
    const bool  nonAutomatedStable  = rmsBassSecondHalf > 0.01f
                                    && std::abs(rmsBassSecondHalf - rmsBassFirstHalf)
                                           < 0.25f * std::max(rmsBassFirstHalf, rmsBassSecondHalf);

    const bool perTrackAutomationWorks = perTrackAutoFades && nonAutomatedStable;

    // The written file is the wet (delayed) mix.
    if (! OfflineRenderer::writeWav(out, wet, sampleRate))
    {
        std::cerr << "Failed to write " << out.getFullPathName() << "\n";
        return 1;
    }

    std::cout << "wrote " << out.getFullPathName()
              << "  frames=" << wet.getNumSamples()
              << "  rmsDry=" << rmsDry
              << "  rmsWet=" << rmsWet
              << "  rmsFiltered=" << rmsFiltered
              << "  gainRatio(-6dB)=" << gainRatio
              << "  delayChanged=" << (delayChanged ? 1 : 0)
              << "  filterAtten=" << (filterAttenuates ? 1 : 0)
              << "  reverbChanged=" << (reverbChanged ? 1 : 0)
              << "  automationFades=" << (automationFades ? 1 : 0)
              << "  perTrackAutomationWorks=" << (perTrackAutomationWorks ? 1 : 0)
              << "  soloMatchesArpOnly=" << (soloMatchesArpOnly ? 1 : 0)
              << "  stemsSumToMix=" << (stemsSumToMix ? 1 : 0)
              << "  clipStartGates=" << (clipStartGates ? 1 : 0)
              << "  multiClipGates=" << (multiClipGates ? 1 : 0)
              << "  audioTrackWorks=" << (audioTrackWorks ? 1 : 0)
              << "  multiClipAudioGates=" << (multiClipAudioGates ? 1 : 0)
              << "  sourceOffsetPlays=" << (sourceOffsetPlays ? 1 : 0)
              << "  clipFadesShape=" << (clipFadesShape ? 1 : 0)
              << "  midiRoundTripWorks=" << (midiRoundTripWorks ? 1 : 0)
              << "  midiRecordingWorks=" << (midiRecordingWorks ? 1 : 0)
              << "  pluginsScanned=" << pluginsScanned
              << "  pluginHostWorks=" << (pluginHostWorks ? 1 : 0)
              << "  chorusChangesSound=" << (chorusChangesSound ? 1 : 0)
              << "  chorusDepthMatters=" << (chorusDepthMatters ? 1 : 0)
              << "  wobbleChangesSound=" << (wobbleChangesSound ? 1 : 0)
              << "  wobbleDepthMatters=" << (wobbleDepthMatters ? 1 : 0)
              << "  wobbleTracksTempo=" << (wobbleTracksTempo ? 1 : 0)
              << "  filterEnvChangesSound=" << (filterEnvChangesSound ? 1 : 0)
              << "  subOscChangesSound=" << (subOscChangesSound ? 1 : 0)
              << "  unisonChangesSound=" << (unisonChangesSound ? 1 : 0)
              << "  compressorSquashes=" << (compressorSquashes ? 1 : 0)
              << "  cascadedStagesEnrich=" << (cascadedStagesEnrich ? 1 : 0)
              << "  tremoloModulates=" << (tremoloModulates ? 1 : 0)
              << "  gateClosesQuiet=" << (gateClosesQuiet ? 1 : 0)
              << "  masteringChangesSound=" << (masteringChangesSound ? 1 : 0)
              << "  masteringHoldsCeiling=" << (masteringHoldsCeiling ? 1 : 0)
              << "  driveChangesSound=" << (driveChangesSound ? 1 : 0)
              << "  driveCabinetWorks=" << (driveCabinetWorks ? 1 : 0)
              << "  effectChainOrderMatters=" << (effectChainOrderMatters ? 1 : 0)
              << "  effectChainRunsAllNodes=" << (effectChainRunsAllNodes ? 1 : 0)
              << "  sessionLaunchQuantizes=" << (sessionLaunchQuantizes ? 1 : 0)
              << "  sessionStopWorks=" << (sessionStopWorks ? 1 : 0)
              << "  trackPanWorks=" << (trackPanWorks ? 1 : 0)
              << "  panAutomationWorks=" << (panAutomationWorks ? 1 : 0)
              << "  trackInsertFilterWorks=" << (trackInsertFilterWorks ? 1 : 0)
              << "  metronomeWorks=" << (metronomeWorks ? 1 : 0)
              << "  metronomeSilentWhenOff=" << (metronomeSilentWhenOff ? 1 : 0)
              << "  recorderWorks=" << (recorderWorks ? 1 : 0) << "\n";

    // Non-silent output, a correct -6 dB gain ratio, a delay that alters the
    // signal, a low-pass that attenuates, a reverb that changes the signal, a
    // gain ramp that fades in, a sample-accurate per-track automation curve
    // that fades one track while leaving an unautomated sibling stable, solo
    // correctly silencing the other track, a clip start that gates playback, two
    // MIDI clips on one track each sounding only in their own window, a
    // decoded audio clip playing back through a track, two AUDIO clips on one
    // track likewise each sounding only in their own window, a MIDI file
    // export/import round trip that preserves tempo and every note, and the
    // recorder's capture/handoff logic (fed synthetic input, since there's no
    // live mic here) together confirm the full
    // render/gain/fx/automation/solo/clip/audio/midi/record path.
    const bool ok = rmsDry > 0.0f && std::isfinite(rmsDry)
                 && gainRatio > 0.47f && gainRatio < 0.53f
                 && delayChanged && filterAttenuates && reverbChanged && automationFades
                 && perTrackAutomationWorks
                 && soloMatchesArpOnly && stemsSumToMix && clipStartGates && multiClipGates
                 && audioTrackWorks && multiClipAudioGates && sourceOffsetPlays && clipFadesShape && midiRoundTripWorks
                 && midiRecordingWorks
                 && cascadedStagesEnrich
                 && pluginHostWorks
                 && filterEnvChangesSound && subOscChangesSound && unisonChangesSound
                 && effectChainOrderMatters && effectChainRunsAllNodes
                 && sessionLaunchQuantizes && sessionStopWorks
                 && trackPanWorks && panAutomationWorks && trackInsertFilterWorks
                 && metronomeWorks && metronomeSilentWhenOff && recorderWorks;
    return ok ? 0 : 2;
}
