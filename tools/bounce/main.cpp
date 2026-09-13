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
#include "engine/DrumKitNode.h"
#include "engine/EffectChain.h"
#include "engine/GenerativeLoop.h"
#include "engine/GuitarNode.h"
#include <chrono>

#include "engine/PianoNode.h"
#include "engine/PluginHost.h"
#include "engine/PluginNode.h"
#include "engine/FilterEffect.h"
#include "engine/InstrumentTrack.h"
#include "engine/Metronome.h"
#include "engine/SessionPlayer.h"
#include "engine/MidiFileIO.h"
#include "engine/MidiRecorder.h"
#include "engine/TempoDetect.h"
#include "engine/TimeStretch.h"
#include "engine/MasteringProcessor.h"
#include "engine/OfflineRenderer.h"
#include "engine/ReverbEffect.h"
#include "model/AutomationLane.h"
#include "model/GuitarTonePresets.h"
#include "model/SynthTonePresets.h"
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
static std::unique_ptr<looper::engine::EffectProcessor> nodeForSlot(const looper::model::EffectSlot& slot)
{
    return looper::engine::makeConfiguredNode(slot);
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
    using namespace looper::engine;

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

        const auto takeFile = tempDir.getNonexistentChildFile("looper-take", ".wav");

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

        const auto longFile = tempDir.getNonexistentChildFile("looper-long-take", ".wav");
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
        const auto countInFile = tempDir.getNonexistentChildFile("looper-countin", ".wav");
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
        const auto abandonedFile = tempDir.getNonexistentChildFile("looper-abandoned", ".wav");
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
    // Note this deliberately compares against the render *without* a send bus
    // or master bus, matching what a stem export writes — the master chain is
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

    // Send-bus check: with a track sending fully into the bus, enabling the
    // send-bus reverb must change the output relative to the send bus being off.
    const auto  noSendBus   = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                      std::vector<double>{}, std::vector<float> { 1.0f },
                                                      false, 0.6f, 0.4f, 0.7f, bpm, sampleRate, seconds);
    const auto  withSendBus = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                      std::vector<double>{}, std::vector<float> { 1.0f },
                                                      true, 0.6f, 0.4f, 0.7f, bpm, sampleRate, seconds);
    const float rmsNoSendBus   = noSendBus.getRMSLevel(0, 0, noSendBus.getNumSamples());
    const float rmsWithSendBus = withSendBus.getRMSLevel(0, 0, withSendBus.getNumSamples());
    const bool  sendBusChanged = std::abs(rmsWithSendBus - rmsNoSendBus) > 1.0e-4f;

    // Send-bus DELAY check: same setup, but with the bus's effect type set to
    // Delay (1) instead of the default Reverb (0) — must differ from both
    // "bus off" and the reverb-based bus above, confirming the engine
    // genuinely switches which effect processes the send bus.
    const auto  withSendBusDelay     = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                               std::vector<double>{}, std::vector<float> { 1.0f },
                                                               true, 0.6f, 0.4f, 0.7f, bpm, sampleRate, seconds,
                                                               512, nullptr,
                                                               1, 250.0f, 0.4f);
    const float rmsWithSendBusDelay  = withSendBusDelay.getRMSLevel(0, 0, withSendBusDelay.getNumSamples());
    const bool  sendBusDelayWorks    = std::abs(rmsWithSendBusDelay - rmsNoSendBus) > 1.0e-4f
                                     && std::abs(rmsWithSendBusDelay - rmsWithSendBus) > 1.0e-4f;

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

    // MIDI import/export round-trip check: build a Song with three notes on
    // one track, export it to a temp .mid, re-import it into a fresh Song,
    // and confirm every note (and the tempo) survived — beat/pitch/velocity
    // within the rounding tolerance a real tick-based file format implies.
    bool midiRoundTripWorks = false;
    {
        using namespace looper::model;

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

    // Drum-kit check: a kick+snare pattern (kick on beats 0/2, snare on
    // beats 1/3, at 120bpm = 0.5s/beat) driven through DrumKitNode via the
    // normal sequencer path — each hit plays its full one-shot sample
    // regardless of the note's own lengthBeats (note-off is ignored unless
    // it's a hard stop; see DrumSampleVoice::stopNote), and an unassigned
    // pad (Hat, note 42, triggered but given no sample) must stay silent.
    ClipData drumHit;
    {
        const int n = (int) (0.15 * sampleRate); // short one-shot, well under the 0.5s gap between hits
        drumHit.audio.setSize(1, n);
        drumHit.sourceSampleRate = sampleRate;
        drumHit.numChannels      = 1;
        drumHit.lengthSamples    = n;
        float* data = drumHit.audio.getWritePointer(0);
        for (int i = 0; i < n; ++i)
            data[i] = 0.6f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / sampleRate);
    }

    Pattern drumPattern;
    drumPattern.lengthBeats = 4.0;
    drumPattern.notes.push_back({ 0.0, 0.1, 36, 1.0f }); // kick, beat 0
    drumPattern.notes.push_back({ 1.0, 0.1, 38, 1.0f }); // snare, beat 1
    drumPattern.notes.push_back({ 2.0, 0.1, 36, 1.0f }); // kick, beat 2
    drumPattern.notes.push_back({ 3.0, 0.1, 38, 1.0f }); // snare, beat 3
    drumPattern.notes.push_back({ 0.5, 0.1, 42, 1.0f }); // hat, no sample assigned — must stay silent

    std::vector<DrumPadAssignment> drumPads;
    drumPads.push_back({ 36, std::make_shared<ClipData>(drumHit) });
    drumPads.push_back({ 38, std::make_shared<ClipData>(drumHit) });
    drumPads.push_back({ 42, nullptr }); // deliberately unassigned

    const auto  drumBuffer = OfflineRenderer::renderDrumPattern(drumPads, drumPattern, bpm, sampleRate, 2.0);
    const int   shortWin   = (int) (0.1 * sampleRate);
    const float rmsKick1   = drumBuffer.getRMSLevel(0, 0, shortWin);
    const float rmsSnare1  = drumBuffer.getRMSLevel(0, (int) (0.5 * sampleRate), shortWin);
    const float rmsKick2   = drumBuffer.getRMSLevel(0, (int) (1.0 * sampleRate), shortWin);
    const float rmsSnare2  = drumBuffer.getRMSLevel(0, (int) (1.5 * sampleRate), shortWin);
    // Past kick1's 0.15s tone but before snare1 (0.5s), and overlapping
    // where the unassigned hat note fires (0.25s) — silence here confirms
    // both "the hit ended" and "the unassigned pad produced nothing".
    const float rmsGapAndHat = drumBuffer.getRMSLevel(0, (int) (0.3 * sampleRate), shortWin);

    const bool drumKitWorks = rmsKick1 > 0.01f && rmsSnare1 > 0.01f
                          && rmsKick2 > 0.01f && rmsSnare2 > 0.01f
                          && rmsGapAndHat < 1.0e-5f;

    // Generative-loop check (see engine/GenerativeLoop.h): a fixed-seed
    // generateDrumLoop and a fixed-seed generateMelodicLoop, rendered
    // through the same DrumKitNode/synth paths every other pattern here
    // goes through, confirming both actually produce sound rather than a
    // silently-misconfigured pattern (e.g. note numbers that don't match any
    // pad, or every note landing outside the render window). Exact onset
    // times aren't asserted beat-by-beat, since pulse counts move with
    // density/seed — instead this checks the two things euclideanRhythm's
    // floor-division construction guarantees for *any* seed: a voice with
    // rotation 0 always onsets on step 0, and generateDrumLoop's snare
    // (fixed at 2 pulses, rotated a quarter-bar) always lands on beats 1
    // and 3, exactly like the hand-written pattern above.
    bool generativeLoopWorks = false;
    {
        DrumLoopParams genDrumParams;
        genDrumParams.seed = 42;
        const auto genDrumPattern = generateDrumLoop(genDrumParams);

        std::vector<DrumPadAssignment> genPads;
        genPads.push_back({ genDrumParams.kickNote, std::make_shared<ClipData>(drumHit) });
        genPads.push_back({ genDrumParams.snareNote, std::make_shared<ClipData>(drumHit) });
        genPads.push_back({ genDrumParams.hatNote, std::make_shared<ClipData>(drumHit) });

        const auto genDrumBuffer = OfflineRenderer::renderDrumPattern(genPads, genDrumPattern, bpm, sampleRate, 2.0);
        const float rmsGenKick   = genDrumBuffer.getRMSLevel(0, 0, shortWin);
        const float rmsGenSnare1 = genDrumBuffer.getRMSLevel(0, (int) (0.5 * sampleRate), shortWin);
        const float rmsGenSnare2 = genDrumBuffer.getRMSLevel(0, (int) (1.5 * sampleRate), shortWin);

        MelodicLoopParams genMelodicParams;
        genMelodicParams.scale = Scale { ScaleType::Major, 60 };
        genMelodicParams.seed  = 42;
        const auto genMelodicPattern = generateMelodicLoop(genMelodicParams);

        const auto genMelodicBuffer =
            OfflineRenderer::render({ genMelodicPattern }, std::vector<float> { 0.0f }, bpm, sampleRate, 2.0);
        const float rmsGenMelodicStart = genMelodicBuffer.getRMSLevel(0, 0, shortWin);
        const float rmsGenMelodicWhole = genMelodicBuffer.getRMSLevel(0, 0, genMelodicBuffer.getNumSamples());

        generativeLoopWorks = rmsGenKick > 0.01f && rmsGenSnare1 > 0.01f && rmsGenSnare2 > 0.01f
                           && rmsGenMelodicStart > 0.001f && rmsGenMelodicWhole > 0.001f;
    }

    // Warp check (see engine/TempoDetect.h): a loop recorded at one tempo has
    // to *end up* the right musical length when stretched to another. The
    // detector's accuracy is covered by headless tests against synthetic click
    // trains; what those cannot cover is the part that matters here — that
    // detect -> warpStretchFactor -> timeStretch composes into audio which
    // actually lines up with the grid, rather than three individually correct
    // steps that disagree about which direction "faster" is.
    bool warpFitsTheGrid = false;
    {
        // Four beats at 160 BPM: a loop faster than the 120 BPM project.
        constexpr double kLoopBpm = 160.0;
        const double     loopSeconds = 4.0 * 60.0 / kLoopBpm;

        std::vector<float> loop((size_t) (loopSeconds * sampleRate), 0.0f);
        const double samplesPerLoopBeat = sampleRate * 60.0 / kLoopBpm;

        // A click on each of its four beats, so it has a detectable tempo.
        for (int beat = 0; beat < 4; ++beat)
        {
            const auto at = (size_t) ((double) beat * samplesPerLoopBeat);
            for (int i = 0; i < (int) (0.02 * sampleRate); ++i)
            {
                const size_t index = at + (size_t) i;
                if (index >= loop.size())
                    break;
                const double decay = std::exp(-40.0 * i / sampleRate);
                loop[index] += (float) (0.5 * decay
                    * (std::sin(2.0 * juce::MathConstants<double>::pi * 200.0 * i / sampleRate)
                     + 0.6 * std::sin(2.0 * juce::MathConstants<double>::pi * 1700.0 * i / sampleRate)));
            }
        }

        const auto estimate = detectTempo(loop, sampleRate);

        // Warped to the project's 120 BPM, the same call the app makes.
        const double factor  = warpStretchFactor(estimate.bpm, bpm, true);
        const auto   warped  = timestretch::timeStretch(loop, factor);

        // Four beats at 120 BPM is exactly 2 seconds. The vocoder pads its
        // output by a frame, so this checks the *musical* length is right to
        // within a small tolerance rather than demanding sample equality.
        const double warpedSeconds   = (double) warped.size() / sampleRate;
        const double expectedSeconds = 4.0 * 60.0 / bpm;

        warpFitsTheGrid = estimate.isUsable()
                       && std::abs(estimate.bpm - kLoopBpm) < 4.0
                       && factor > 1.0 // slower project => longer, not shorter
                       && std::abs(warpedSeconds - expectedSeconds) < 0.1;
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

    // Per-pad mix check: the same pattern again, but with the kick pulled
    // down 6dB and panned hard left, and the snare muted. Verifies each of
    // gain/pan/mute independently against the un-mixed render above.
    bool drumPadMixWorks = false;
    {
        std::vector<DrumPadAssignment> mixedPads;

        DrumPadAssignment kick;
        kick.noteNumber = 36;
        kick.clipData   = std::make_shared<ClipData>(drumHit);
        kick.gain       = juce::Decibels::decibelsToGain(-6.0f);
        kick.pan        = -1.0f; // hard left
        mixedPads.push_back(std::move(kick));

        DrumPadAssignment snare;
        snare.noteNumber = 38;
        snare.clipData   = std::make_shared<ClipData>(drumHit);
        snare.muted      = true;
        mixedPads.push_back(std::move(snare));

        const auto  mixed          = OfflineRenderer::renderDrumPattern(mixedPads, drumPattern, bpm, sampleRate, 2.0);
        const float mixedKickLeft  = mixed.getRMSLevel(0, 0, shortWin);
        const float mixedKickRight = mixed.getRMSLevel(1, 0, shortWin);
        const float mixedSnare     = mixed.getRMSLevel(0, (int) (0.5 * sampleRate), shortWin);

        // -6dB is a ~0.5 amplitude ratio against the same hit rendered flat.
        const float gainRatioKick = rmsKick1 > 0.0f ? mixedKickLeft / rmsKick1 : 0.0f;

        drumPadMixWorks = gainRatioKick > 0.47f && gainRatioKick < 0.53f // gain applied
                       && mixedKickRight < 1.0e-5f                        // panned fully off the right
                       && mixedKickLeft > 0.01f                           // ...but still present on the left
                       && mixedSnare < 1.0e-5f;                           // muted pad is silent
    }

    // Per-pad pitch check: the same one-shot transposed up an octave must
    // read through the sample twice as fast, so it ends around half as far
    // in — audible as a shorter, higher hit.
    bool drumPadPitchWorks = false;
    {
        std::vector<DrumPadAssignment> pitchedPads;
        DrumPadAssignment kick;
        kick.noteNumber = 36;
        kick.clipData   = std::make_shared<ClipData>(drumHit);
        kick.pitchRatio = 2.0f; // +12 semitones
        pitchedPads.push_back(std::move(kick));

        Pattern onlyKick;
        onlyKick.lengthBeats = 4.0;
        onlyKick.notes.push_back({ 0.0, 0.1, 36, 1.0f });

        const auto  pitched = OfflineRenderer::renderDrumPattern(pitchedPads, onlyKick, bpm, sampleRate, 1.0);
        const int   win     = (int) (0.02 * sampleRate);
        // The 0.15s source is consumed in ~0.075s at double speed: still
        // sounding just before that, silent just after.
        const float before  = pitched.getRMSLevel(0, (int) (0.05 * sampleRate), win);
        const float after   = pitched.getRMSLevel(0, (int) (0.09 * sampleRate), win);

        drumPadPitchWorks = before > 0.01f && after < 1.0e-5f;
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

        const auto swept = OfflineRenderer::render({ arp }, { 0.0f }, {}, {}, { 0.0f },
                                                   false, 0.5f, 0.5f, 0.5f,
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
                                       .getChildFile("looper_bounce_plugin_scan.tmp");

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

                    juce::AudioBuffer<float> sendBus(2, 512);
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

                        sendBus.setSize(2, n, false, false, true);
                        sendBus.clear();

                        juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                        track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

    // Stopping the transport silences a ringing guitar.
    //
    // The strings deliberately ignore note-offs — a guitar rings until it is
    // replucked or damped — so nothing in the MIDI stream can end a note once
    // muteOnNoteOff is 0, which is what the drop-tuned tones use. That makes
    // "the transport stopped" a separate event from "the note ended", and only
    // one of them is an articulation.
    //
    // Without this, pressing stop left the strings ringing for their full
    // decay: two seconds of guitar after the take had finished.
    bool guitarStopsOnTransportStop = false;
    {
        constexpr double stopRate  = 44100.0;
        constexpr int    stopBlock = 512;

        GuitarNode guitar;
        guitar.prepare(stopRate, stopBlock);
        guitar.setDecaySeconds(4.0f);   // a long ring, so stopping has to do the work
        guitar.setMuteOnNoteOff(0.0f);  // ...and note-offs cannot

        const int total = (int) (stopRate * 1.5);
        const int stopAt = (int) (stopRate * 0.5);

        juce::AudioBuffer<float> mix(2, total);
        mix.clear();

        for (int pos = 0; pos < total; pos += stopBlock)
        {
            const int n = std::min(stopBlock, total - pos);

            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 40, 0.9f), 0);

            ProcessContext context;
            context.sampleRate                   = stopRate;
            context.numSamples                   = n;
            context.transport.playing            = pos < stopAt;
            context.transport.playheadSamples    = pos;
            context.transport.bpm                = 120.0;
            context.transport.timeSigNumerator   = 4;
            context.transport.timeSigDenominator = 4;
            context.transport.ppqPosition        = (double) pos / (stopRate / 2.0);
            context.transport.ppqAtBlockEnd      = (double) (pos + n) / (stopRate / 2.0);

            juce::AudioBuffer<float> view(mix.getArrayOfWritePointers(), 2, pos, n);
            guitar.process(view, midi, context);
        }

        // Sounding before the stop, and gone shortly after it. "Shortly"
        // rather than "immediately" on purpose: the strings are damped, not
        // cut, because zeroing them would click.
        const float before = mix.getRMSLevel(0, stopAt - (int) (stopRate * 0.1), (int) (stopRate * 0.1));
        const float after  = mix.getRMSLevel(0, stopAt + (int) (stopRate * 0.2), (int) (stopRate * 0.3));

        guitarStopsOnTransportStop = before > 0.001f && after < before * 0.02f;

        std::cout << "guitar stop: before=" << before << " after=" << after << "\n";
    }

    // Tempo changes.
    //
    // The check the whole tempo-map conversion rests on, and the only one that
    // renders audio with two tempos in it: everything else proves the
    // *single*-tempo case is unchanged, which is the regression half of the
    // job rather than the feature half.
    //
    // Driven through Sequencer directly rather than OfflineRenderer, because
    // that harness renders at one constant BPM by construction. Building the
    // per-block snapshots from a real TempoMap here is exactly what
    // AudioEngine does live, so it exercises the converted scheduling path.
    bool tempoChangeMovesNotes = false;
    bool tempoRampAccelerates  = false;
    {
        constexpr double tempoRate  = 48000.0;
        constexpr int    tempoBlock = 512;

        // A note on every beat for eight beats, at 120bpm until beat 4 and
        // 60bpm after — so beats 4..7 should take twice as long as beats 0..3.
        Pattern pattern;
        pattern.lengthBeats = 64.0; // long enough not to loop within the render
        for (int beat = 0; beat < 8; ++beat)
            pattern.notes.push_back({ (double) beat, 0.25, 60, 0.9f });

        auto onsetsFor = [&](const TempoMap& map)
        {
            Sequencer sequencer;
            ClipSlot slot;
            slot.pattern     = pattern;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 64.0;
            sequencer.submitClips(new std::vector<ClipSlot> { slot });

            std::vector<int> onsets;
            const int total = (int) (tempoRate * 14.0);

            for (int pos = 0; pos < total; pos += tempoBlock)
            {
                const int n = std::min(tempoBlock, total - pos);

                ProcessContext context;
                context.sampleRate                   = tempoRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                context.transport.playheadSamples    = pos;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;
                context.transport.ppqPosition        = map.ppqFromSamples(pos);
                context.transport.ppqAtBlockEnd      = map.ppqFromSamples(pos + n);
                context.transport.bpm                = map.tempoAtBeat(context.transport.ppqPosition);

                juce::MidiBuffer midi;
                sequencer.renderBlock(midi, context);

                for (const auto metadata : midi)
                    if (metadata.getMessage().isNoteOn())
                        onsets.push_back(pos + metadata.samplePosition);
            }
            return onsets;
        };

        TempoMap steady;
        steady.setSampleRate(tempoRate);
        steady.setTempo(120.0);

        TempoMap changing;
        changing.setSampleRate(tempoRate);
        changing.setTempoChanges({ { 0.0, 120.0 }, { 4.0, 60.0 } });

        const auto steadyOnsets   = onsetsFor(steady);
        const auto changingOnsets = onsetsFor(changing);

        // Eight notes either way: a tempo change must move notes, not lose them.
        const bool bothPlayedEverything = steadyOnsets.size() == 8 && changingOnsets.size() == 8;

        bool beforeMatches = false, afterMoved = false, landsWhereMapSays = false;

        if (bothPlayedEverything)
        {
            // Before the change the two renders agree...
            beforeMatches = true;
            for (int i = 0; i < 4; ++i)
                if (std::abs(steadyOnsets[(size_t) i] - changingOnsets[(size_t) i]) > tempoBlock)
                    beforeMatches = false;

            // ...and after it they measurably do not. Without this a map that
            // was ignored entirely would pass everything else here.
            afterMoved = changingOnsets[7] > steadyOnsets[7] + (int) tempoRate;

            // And each onset is where the map says, not merely somewhere later.
            landsWhereMapSays = true;
            for (int beat = 0; beat < 8; ++beat)
            {
                const auto expected = (int) changing.samplesFromPpq((double) beat);
                if (std::abs(changingOnsets[(size_t) beat] - expected) > tempoBlock)
                    landsWhereMapSays = false;
            }
        }

        tempoChangeMovesNotes = bothPlayedEverything && beforeMatches && afterMoved
                             && landsWhereMapSays;

        // A ramp accelerates *through* the segment rather than stepping at it.
        //
        // Checked as the gap between consecutive onsets: under a step the gaps
        // are two constant values with one jump between them, while under a
        // ramp every gap is shorter than the last. That difference is the
        // whole feature, and a ramp implemented as a step would pass a
        // "notes moved" check but not this one.
        {
            TempoMap ramp;
            ramp.setSampleRate(tempoRate);
            ramp.setTempoChanges({ { 0.0, 60.0 }, { 8.0, 180.0, true } });

            const auto rampOnsets = onsetsFor(ramp);

            bool everyGapShorter = rampOnsets.size() == 8;
            for (size_t i = 2; i < rampOnsets.size() && everyGapShorter; ++i)
            {
                const int previousGap = rampOnsets[i - 1] - rampOnsets[i - 2];
                const int thisGap     = rampOnsets[i] - rampOnsets[i - 1];

                // Strictly shorter, allowing a block of scheduling slack.
                if (thisGap > previousGap - 1)
                    everyGapShorter = false;
            }

            // And each onset is where the integral says, not merely earlier.
            bool matchesIntegral = rampOnsets.size() == 8;
            for (int beat = 0; beat < 8 && matchesIntegral; ++beat)
                if (std::abs(rampOnsets[(size_t) beat] - (int) ramp.samplesFromPpq((double) beat))
                        > tempoBlock)
                    matchesIntegral = false;

            tempoRampAccelerates = everyGapShorter && matchesIntegral;

            std::cout << "tempo ramp: gaps=";
            for (size_t i = 1; i < rampOnsets.size(); ++i)
                std::cout << (rampOnsets[i] - rampOnsets[i - 1]) << " ";
            std::cout << "\n";
        }

        std::cout << "tempo map: notes=" << changingOnsets.size()
                  << " lastSteady=" << (steadyOnsets.size() == 8 ? steadyOnsets[7] : -1)
                  << " lastChanging=" << (changingOnsets.size() == 8 ? changingOnsets[7] : -1)
                  << " expectedLast=" << changing.samplesFromPpq(7.0) << "\n";
    }

    // Guitar checks. The DSP itself is covered by unit tests; what the bounce
    // tool adds is the *performance* model, which is what separates a guitar
    // from a synth with a plucked patch.
    bool guitarSounds          = false;
    bool guitarCutsSameString  = false;
    bool guitarPlaysSixAtOnce  = false;
    bool guitarPicksLowestFret = false;
    bool guitarHammerOn        = false;
    bool guitarPalmMuteChugs   = false;
    {
        // Renders a guitar node given (noteNumber, sampleOffset) note-ons.
        // @p channel carries the articulation: 2 is palm muted, see
        // PatternPlayback::channelFor.
        // @p settings, when given, configures the node the way a preset would -
        // so a check can measure the tone the user actually gets rather than
        // bare defaults.
        auto renderNotes = [&](const std::vector<std::pair<int, int>>& notes, double durationSeconds,
                               int channel = 1,
                               const looper::model::GuitarSettings* settings = nullptr)
        {
            const int totalSamples = (int) (sampleRate * durationSeconds);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);
            guitar.setDecaySeconds(4.0f);

            if (settings != nullptr)
            {
                guitar.setDecaySeconds(settings->decaySeconds);
                guitar.setBrightness(settings->brightness);
                guitar.setPickPosition(settings->pickPosition);
                guitar.setPickHardness(settings->pickHardness);
                guitar.setPickupResonanceHz(settings->pickupResonanceHz);
                guitar.setPickupQ(settings->pickupQ);
                guitar.setPalmMuteDecaySeconds(settings->palmMuteDecaySeconds);
                guitar.setPalmMuteBrightness(settings->palmMuteBrightness);
            }

            juce::MidiBuffer midi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                midi.clear();
                for (const auto& [note, offset] : notes)
                    if (offset >= pos && offset < pos + n)
                        midi.addEvent(juce::MidiMessage::noteOn(channel, note, 0.9f), offset - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                juce::AudioBuffer<float> view(mix.getArrayOfWritePointers(), 2, pos, n);
                guitar.process(view, midi, context);
            }
            return mix;
        };

        auto magnitude = [&](const juce::AudioBuffer<float>& audioBuffer, double frequency, int from, int count)
        {
            double real = 0.0, imaginary = 0.0;
            for (int i = 0; i < count && from + i < audioBuffer.getNumSamples(); ++i)
            {
                const double angle = 2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate;
                real      += audioBuffer.getSample(0, from + i) * std::cos(angle);
                imaginary += audioBuffer.getSample(0, from + i) * std::sin(angle);
            }
            return std::hypot(real, imaginary) / count;
        };

        // A single plucked low E must sound.
        const auto single = renderNotes({ { 40, 0 } }, 1.0);
        guitarSounds = single.getRMSLevel(0, 0, single.getNumSamples()) > 0.001f;

        // One note per string: E2 (40) then F2 (41). Only the low E string can
        // reach either, so the second note must *cut* the first — the single
        // most audible thing separating this from a polyphonic synth.
        const int  window = (int) (0.3 * sampleRate);
        const int  second = (int) (0.5 * sampleRate);
        const auto cut    = renderNotes({ { 40, 0 }, { 41, second } }, 1.5);

        const double e2Before = magnitude(cut, 82.41, (int) (0.05 * sampleRate), window);
        const double e2After  = magnitude(cut, 82.41, second + (int) (0.05 * sampleRate), window);
        const double f2After  = magnitude(cut, 87.31, second + (int) (0.05 * sampleRate), window);

        // E2 must be largely gone, and F2 present in its place.
        guitarCutsSameString = e2Before > 1.0e-4 && e2After < e2Before * 0.25 && f2After > e2After;

        // ...but six notes that fit six different strings must all ring: the
        // cut rule is per string, not a global monophony.
        const auto chord = renderNotes({ { 40, 0 }, { 45, 0 }, { 50, 0 },
                                         { 55, 0 }, { 59, 0 }, { 64, 0 } }, 1.0);
        const int  from  = (int) (0.05 * sampleRate);
        int        heard = 0;
        for (double f : { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 })
            if (magnitude(chord, f, from, window) > 1.0e-4)
                ++heard;

        guitarPlaysSixAtOnce = heard == 6;

        // Hammer-on: with every string already held, a further note that a held
        // string can reach must be *re-fretted* rather than struck. Two things
        // have to be true — the pitch moves on that string, and no new attack
        // appears, which is what makes a hammer-on softer than a picked note.
        {
            const int    hammerAt = (int) (0.5 * sampleRate);
            const double barSpan  = 0.25 * sampleRate;

            // Hold all six strings, then ask for a note only a held string can
            // take (F4 = 65, reachable on the high E at fret 1).
            std::vector<std::pair<int, int>> notes;
            for (int n : { 40, 45, 50, 55, 59, 64 })
                notes.push_back({ n, 0 });
            notes.push_back({ 65, hammerAt });

            const auto rendered = renderNotes(notes, 1.5);

            // Level just before and just after the hammer-on. A fresh pluck
            // would spike; a hammer-on must not.
            float before = 0.0f, after = 0.0f;
            for (int i = hammerAt - (int) barSpan; i < hammerAt; ++i)
                before = std::max(before, std::abs(rendered.getSample(0, i)));
            for (int i = hammerAt; i < hammerAt + (int) barSpan; ++i)
                after = std::max(after, std::abs(rendered.getSample(0, i)));

            // ...and the new pitch must actually be sounding afterwards.
            const double f4 = 349.23;
            const double f4After = magnitude(rendered, f4, hammerAt + 2000, (int) (0.3 * sampleRate));

            guitarHammerOn = after <= before && f4After > 1.0e-4;
        }

        // Palm muting: the articulation a riff is made of.
        //
        // Two claims, because either alone passes for something else. A note
        // that is merely *short* has been cut off; a note that is merely *dark*
        // has had its tone knob turned down. A chug is both, and it still has
        // to sound - a check that only measured shortness would be satisfied by
        // silence.
        {
            constexpr int kLowE = 40;

            // Driven by the preset rather than bare defaults: what matters is
            // whether a chug reads as a chug in the tone someone will actually
            // play, and the preset is where the two articulations are dialled
            // against each other.
            const auto metal = looper::model::presetForGuitarTone(GuitarTone::ModernMetal).guitar;

            const auto open  = renderNotes({ { kLowE, 0 } }, 1.2, 1, &metal);
            const auto muted = renderNotes({ { kLowE, 0 } }, 1.2, 2, &metal);

            // Length, measured well after a muted note should have gone: the
            // node is set to a 4-second decay above, against the default 0.18s
            // palm mute.
            const int   tailFrom = (int) (sampleRate * 0.35);
            const int   tailLen  = (int) (sampleRate * 0.30);
            const float openTail  = open.getRMSLevel(0, tailFrom, tailLen);
            const float mutedTail = muted.getRMSLevel(0, tailFrom, tailLen);

            // Darkness, measured *early* and relative to the fundamental. An
            // absolute high-frequency reading would just restate the level
            // difference; the ratio says the tone is different, not only
            // quieter.
            const int    earlyFrom = (int) (sampleRate * 0.01);
            const int    earlyLen  = (int) (sampleRate * 0.06);

            // High-band energy as a fraction of the window's total, over a comb
            // of frequencies rather than one bin, and measured *above* the
            // pickup's resonance.
            //
            // The band matters. Measured across 1.2-5kHz the two articulations
            // were indistinguishable, because the pickup's resonant peak sits
            // at 3kHz and imposes its own shape on everything passing through
            // it. Above that peak the string's own filters have authority
            // again: the palm-mute brightness control spans 2.6x there against
            // almost nothing in the midband.
            //
            // Not a ratio to the fundamental, which is what this measured
            // first: E2 is only about five cycles in a 60ms window, so the
            // correlation at 82Hz leaks badly and a bad denominator made the
            // muted note look fifteen times brighter than the open one. Divided
            // by the window's RMS instead, the figure is level-independent
            // without depending on estimating a low frequency in a short
            // window.
            auto brightnessOf = [&](const juce::AudioBuffer<float>& audioBuffer)
            {
                double high = 0.0;
                for (double f = 4500.0; f <= 12000.0; f *= 1.12)
                {
                    const double m = magnitude(audioBuffer, f, earlyFrom, earlyLen);
                    high += m * m;
                }

                const double rms = (double) audioBuffer.getRMSLevel(0, earlyFrom, earlyLen);
                return std::sqrt(high) / std::max(rms, 1.0e-12);
            };

            const double openBrightness  = brightnessOf(open);
            const double mutedBrightness = brightnessOf(muted);

            // And it does sound: a chug is an attack, not an absence.
            const float mutedAttack = muted.getRMSLevel(0, 0, (int) (sampleRate * 0.05));

            // Three claims, because any one alone passes for something else. A
            // note that is only *short* has been cut off. One that is only
            // *dark* has had its tone knob turned down. And a check for both
            // that forgot to require sound would be satisfied by silence.
            //
            // The darkness margin is deliberately modest: measured, a muted
            // note is about 18% darker than an open one through this preset.
            // That is a real and repeatable difference but not a dramatic one,
            // and asserting a bigger gap than the DSP actually produces is how
            // a sentinel ends up being tuned to rather than measured against.
            guitarPalmMuteChugs = mutedAttack > 0.001f
                               && mutedTail < openTail * 0.25f
                               && mutedBrightness < openBrightness * 0.9;

            std::cout << "palm mute: openTail=" << openTail << " mutedTail=" << mutedTail
                      << " openBright=" << openBrightness << " mutedBright=" << mutedBrightness
                      << " mutedAttack=" << mutedAttack << "\n";
        }

        // Allocation preference, checked directly rather than inferred from
        // the audio: E4 is reachable on every string (fret 24 on the low E
        // down to open on the high E), and the rule is to take the one needing
        // the lowest fret. The cut check above can't see this, because the
        // notes it uses are only reachable on one string either way.
        {
            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);

            juce::AudioBuffer<float> scratch(2, 512);
            scratch.clear();

            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.9f), 0); // E4

            ProcessContext context;
            context.sampleRate                   = sampleRate;
            context.numSamples                   = 512;
            context.transport.playing            = true;
            context.transport.bpm                = bpm;
            context.transport.timeSigNumerator   = 4;
            context.transport.timeSigDenominator = 4;
            guitar.process(scratch, midi, context);

            guitarPicksLowestFret = guitar.noteOnString(5) == 64  // open high E
                                 && guitar.noteOnString(0) == -1; // not fret 24 on the low E
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

    // Piano (docs/PLAN.md §34 step 3): does it sound, does a damper stop it,
    // and — the question this instrument actually turns on — what does full
    // polyphony cost?
    //
    // The CPU figure is measured here rather than assumed, because it is the
    // one design risk in the piano that is not about how it sounds. Every
    // voice is up to three waveguides, each running an eight-section allpass
    // cascade for stiffness, so a full pedalled chord is a different order of
    // cost from six guitar strings and the voice count has to be chosen
    // against a number rather than a guess.
    bool pianoSounds     = false;
    bool pianoDamps      = false;
    bool pianoTopRings   = false;
    bool pianoAffordsPolyphony = false;
    {
        auto renderPiano = [&](const std::vector<int>& notes, double renderSeconds,
                               double releaseAt, double* elapsedSecondsOut)
        {
            const int totalSamples = (int) (sampleRate * renderSeconds);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            PianoNode piano;
            piano.prepare(sampleRate, 512);

            const int releaseSample = releaseAt > 0.0 ? (int) (releaseAt * sampleRate) : -1;

            const auto startedAt = std::chrono::steady_clock::now();

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate        = sampleRate;
                context.numSamples        = n;
                context.transport.playing = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);

                juce::MidiBuffer blockMidi;
                if (pos == 0)
                    for (int note : notes)
                        blockMidi.addEvent(juce::MidiMessage::noteOn(1, note, 0.85f), 0);

                if (releaseSample >= pos && releaseSample < pos + n)
                    for (int note : notes)
                        blockMidi.addEvent(juce::MidiMessage::noteOff(1, note),
                                           releaseSample - pos);

                juce::AudioBuffer<float> block(mix.getArrayOfWritePointers(), 2, pos, n);
                piano.process(block, blockMidi, context);
            }

            if (elapsedSecondsOut != nullptr)
                *elapsedSecondsOut = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - startedAt).count();

            return mix;
        };

        // A middle-register chord, left to ring.
        const auto chord = renderPiano({ 48, 55, 60, 64, 67 }, 3.0, 0.0, nullptr);
        pianoSounds = chord.getRMSLevel(0, 0, chord.getNumSamples()) > 0.005f
                   && chord.getMagnitude(0, 0, chord.getNumSamples()) < 1.5f;

        // The same chord, released after a second: the dampers must stop it.
        const auto damped = renderPiano({ 48, 55, 60, 64, 67 }, 3.0, 1.0, nullptr);
        const float beforeRelease = damped.getRMSLevel(0, (int) (0.7 * sampleRate),
                                                       (int) (0.2 * sampleRate));
        const float afterRelease  = damped.getRMSLevel(0, (int) (2.5 * sampleRate),
                                                       (int) (0.4 * sampleRate));
        pianoDamps = beforeRelease > 0.005f && afterRelease < beforeRelease * 0.05f;

        // ...but the top of the keyboard has no dampers, so it rings anyway.
        const auto topNote = renderPiano({ 100 }, 2.0, 0.5, nullptr);
        const float topBefore = topNote.getRMSLevel(0, (int) (0.3 * sampleRate),
                                                    (int) (0.15 * sampleRate));
        const float topAfter  = topNote.getRMSLevel(0, (int) (1.5 * sampleRate),
                                                    (int) (0.4 * sampleRate));
        pianoTopRings = topBefore > 0.002f && topAfter > topBefore * 0.05f;

        std::cout << "piano detail: chordRms=" << chord.getRMSLevel(0, 0, chord.getNumSamples())
                  << " chordPeak=" << chord.getMagnitude(0, 0, chord.getNumSamples())
                  << " beforeRelease=" << beforeRelease << " afterRelease=" << afterRelease
                  << " topBefore=" << topBefore << " topAfter=" << topAfter
                  << " topWholeRms=" << topNote.getRMSLevel(0, 0, topNote.getNumSamples())
                  << " topPeak=" << topNote.getMagnitude(0, 0, topNote.getNumSamples()) << "\n";

        // Worst case: every voice sounding, in the bass where the strings are
        // longest and the stiffness cascade is deepest.
        std::vector<int> everything;
        for (int i = 0; i < PianoNode::kMaxVoices; ++i)
            everything.push_back(28 + i);

        double elapsed = 0.0;
        const double renderedSeconds = 4.0;
        const auto   allVoices = renderPiano(everything, renderedSeconds, 0.0, &elapsed);

        const double realtimeFactor = elapsed > 0.0 ? renderedSeconds / elapsed : 0.0;

        std::cout << "piano: voices=" << PianoNode::kMaxVoices
                  << " realtime=" << realtimeFactor << "x"
                  << " rms=" << allVoices.getRMSLevel(0, 0, full.getNumSamples()) << "\n";

        // Five times realtime for the *whole* instrument at full polyphony, so
        // a piano track leaves room for everything else in the project. Below
        // that the voice count is too high, not the machine too slow.
        pianoAffordsPolyphony = realtimeFactor > 5.0
                             && allVoices.getRMSLevel(0, 0, full.getNumSamples()) > 0.005f;
    }

    // The sustain pedal (docs/PLAN.md §34 step 4), through the real node and
    // as real CC64 — so this covers the message plumbing as well as the
    // dampers.
    bool pianoPedalSustains   = false;
    bool pianoPedalLifts      = false;
    bool pianoResonates       = false;
    bool pianoPedalStaysStable = false;
    {
        auto renderPedalled = [&](bool pedal, double liftAt, const std::vector<int>& notes,
                                  double lengthSeconds, bool holdKeys = false)
        {
            const int totalSamples = (int) (sampleRate * lengthSeconds);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            PianoNode piano;
            piano.prepare(sampleRate, 512);

            const int releaseSample = holdKeys ? -1 : (int) (0.3 * sampleRate);
            const int liftSample    = liftAt > 0.0 ? (int) (liftAt * sampleRate) : -1;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate        = sampleRate;
                context.numSamples        = n;
                context.transport.playing = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);

                juce::MidiBuffer blockMidi;

                if (pos == 0)
                {
                    if (pedal)
                        blockMidi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
                    for (int note : notes)
                        blockMidi.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), 0);
                }

                // Keys up early: with the pedal down the notes must ring on
                // regardless, which is the entire behaviour under test.
                if (releaseSample >= pos && releaseSample < pos + n)
                    for (int note : notes)
                        blockMidi.addEvent(juce::MidiMessage::noteOff(1, note),
                                           releaseSample - pos);

                if (liftSample >= pos && liftSample < pos + n)
                    blockMidi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0),
                                       liftSample - pos);

                juce::AudioBuffer<float> block(mix.getArrayOfWritePointers(), 2, pos, n);
                piano.process(block, blockMidi, context);
            }

            return mix;
        };

        const std::vector<int> chord { 48, 55, 60, 64 };

        // Keys released at 0.3s. Without the pedal the dampers stop it; with
        // the pedal held they cannot reach the strings.
        const auto dry     = renderPedalled(false, 0.0, chord, 2.5);
        const auto held    = renderPedalled(true,  0.0, chord, 2.5);

        const int  tailFrom  = (int) (1.5 * sampleRate);
        const int  tailCount = (int) (0.5 * sampleRate);
        const float dryTail  = dry.getRMSLevel(0, tailFrom, tailCount);
        const float heldTail = held.getRMSLevel(0, tailFrom, tailCount);

        pianoPedalSustains = heldTail > 0.002f && heldTail > dryTail * 10.0f;

        // ...and lifting it at 1.0s damps everything already released.
        const auto lifted = renderPedalled(true, 1.0, chord, 2.5);
        const float liftedBefore = lifted.getRMSLevel(0, (int) (0.8 * sampleRate),
                                                      (int) (0.15 * sampleRate));
        const float liftedAfter  = lifted.getRMSLevel(0, tailFrom, tailCount);

        pianoPedalLifts = liftedBefore > 0.002f && liftedAfter < liftedBefore * 0.05f;

        // Sympathetic resonance, isolated from sustain.
        //
        // The first version of this compared a released note pedalled against
        // unpedalled, and that measures the *pedal holding the note*, which
        // pianoPedalSustains already covers — it passed just as happily with
        // the soundboard coupling set to zero, which makes it worthless as a
        // check of the thing it is named after.
        //
        // Here both keys are **held down** for the whole render, so the notes
        // ring either way and sustain cannot account for any difference. What
        // is left is the soundboard path: with the pedal down the two notes
        // drive each other, and with it up they do not.
        const auto twoDry = renderPedalled(false, 0.0, { 40, 47 }, 2.0, /*holdKeys=*/true);
        const auto twoPed = renderPedalled(true,  0.0, { 40, 47 }, 2.0, /*holdKeys=*/true);

        double difference = 0.0;
        double reference  = 0.0;
        for (int i = 0; i < twoDry.getNumSamples(); ++i)
        {
            difference += std::abs(twoPed.getSample(0, i) - twoDry.getSample(0, i));
            reference  += std::abs(twoDry.getSample(0, i));
        }

        // Relative to the note itself, so this cannot pass on level alone.
        pianoResonates = reference > 1.0 && difference > reference * 0.01;

        // Stability: positive feedback across every ringing voice at once, so
        // a big pedalled chord left for a long time must not grow.
        const std::vector<int> bigChord { 36, 40, 43, 48, 52, 55, 60, 64, 67, 72 };
        const auto sustained = renderPedalled(true, 0.0, bigChord, 12.0);

        const float earlyPeak = sustained.getMagnitude(0, (int) (0.5 * sampleRate),
                                                       (int) (0.5 * sampleRate));
        const float latePeak  = sustained.getMagnitude(0, (int) (11.0 * sampleRate),
                                                       (int) (0.5 * sampleRate));

        std::cout << "piano pedal: dryTail=" << dryTail << " heldTail=" << heldTail
                  << " liftedAfter=" << liftedAfter << " resonanceDelta=" << difference << " resonanceRef=" << reference
                  << " earlyPeak=" << earlyPeak << " latePeak=" << latePeak << "\n";

        // Bounded, not merely falling. The first version asserted only that
        // the late peak was below the early one — which a signal that explodes
        // to 1327 and then collapses satisfies perfectly, and which is exactly
        // what the sympathetic feedback did before it was averaged. A runaway
        // has to fail this check, so the check is on the absolute level.
        pianoPedalStaysStable = std::isfinite(latePeak) && earlyPeak > 0.001f
                             && earlyPeak < 2.0f
                             && latePeak < earlyPeak;
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

    // Guitar phase 1 (docs/PLAN.md §33): stereo width and bridge coupling,
    // driven through the real GuitarNode rather than a bare string, because
    // both are properties of how the six strings are *combined*.
    bool guitarHasWidth = false;
    bool guitarStringsCouple = false;
    bool guitarStiffnessChangesTone = false;
    {
        auto renderGuitar = [&](float coupling, float width, bool strum)
        {
            const int totalSamples = (int) (sampleRate * 2.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);
            guitar.setDecaySeconds(4.0f);
            guitar.setCoupling(coupling);
            guitar.setWidth(width);

            juce::MidiBuffer midi;
            if (strum)
            {
                // An open E chord: several strings at once, which is where
                // coupling and width both actually show.
                for (int note : { 40, 47, 52, 56, 59, 64 })
                    midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), 0);
            }
            else
            {
                midi.addEvent(juce::MidiMessage::noteOn(1, 40, 0.9f), 0);
            }

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate        = sampleRate;
                context.numSamples        = n;
                context.transport.playing = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);

                juce::AudioBuffer<float> block(mix.getArrayOfWritePointers(), 2, pos, n);
                juce::MidiBuffer         blockMidi = pos == 0 ? midi : juce::MidiBuffer {};

                guitar.process(block, blockMidi, context);
            }

            return mix;
        };

        // --- Width. Panning the strings is the mechanism, so this needs more
        // than one string sounding: a single note is centred whatever the
        // width, because there is nothing to spread it against.
        const auto wide   = renderGuitar(0.0f, 0.6f, true);
        const auto narrow = renderGuitar(0.0f, 0.0f, true);

        double channelDifference = 0.0;
        for (int i = 0; i < wide.getNumSamples(); ++i)
            channelDifference += std::abs(wide.getSample(0, i) - wide.getSample(1, i));

        double narrowDifference = 0.0;
        for (int i = 0; i < narrow.getNumSamples(); ++i)
            narrowDifference += std::abs(narrow.getSample(0, i) - narrow.getSample(1, i));

        // Mono-compatibility, which is the reason this is panning rather than
        // a delay: folding to mono must not cancel anything, so the summed
        // level has to survive.
        double wideMono = 0.0, narrowMono = 0.0;
        for (int i = 0; i < wide.getNumSamples(); ++i)
        {
            const double w = 0.5 * (wide.getSample(0, i)   + wide.getSample(1, i));
            const double n = 0.5 * (narrow.getSample(0, i) + narrow.getSample(1, i));
            wideMono   += w * w;
            narrowMono += n * n;
        }

        guitarHasWidth = narrowDifference < 1.0e-6            // dead centre at width 0
                      && channelDifference > 1.0              // genuinely different channels
                      && wideMono > narrowMono * 0.6;         // and mono survives the fold

        // --- Stiffness (phase 2). Through the node rather than a bare string,
        // because the node is what scales it per string — a wound low E gets
        // the full amount and a plain high E a third of it, and a bug in that
        // scaling would leave the top strings sounding detuned rather than
        // stiff. The headless tests prove the partials actually stretch and
        // that the string stays in tune; this proves the wiring reaches them.
        auto renderStiff = [&](float stiffness)
        {
            const int totalSamples = (int) (sampleRate * 1.5);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);
            guitar.setDecaySeconds(4.0f);
            guitar.setBrightness(0.95f);
            guitar.setStiffness(stiffness);

            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 40, 0.9f), 0); // the low E

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate        = sampleRate;
                context.numSamples        = n;
                context.transport.playing = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);

                juce::AudioBuffer<float> block(mix.getArrayOfWritePointers(), 2, pos, n);
                juce::MidiBuffer         blockMidi = pos == 0 ? midi : juce::MidiBuffer {};
                guitar.process(block, blockMidi, context);
            }

            return mix;
        };

        const auto ideal = renderStiff(0.0f);
        const auto stiff = renderStiff(1.0f);

        double difference = 0.0;
        for (int i = 0; i < ideal.getNumSamples(); ++i)
            difference += std::abs(ideal.getSample(0, i) - stiff.getSample(0, i));

        guitarStiffnessChangesTone =
            ideal.getRMSLevel(0, 0, ideal.getNumSamples()) > 0.001f
         && stiff.getRMSLevel(0, 0, stiff.getNumSamples()) > 0.001f
         && difference > 1.0;

        // --- Coupling. A struck chord leaves energy circulating between the
        // strings, so the tail carries more than it does uncoupled.
        const auto coupled   = renderGuitar(1.0f, 0.0f, true);
        const auto uncoupled = renderGuitar(0.0f, 0.0f, true);

        const int tailFrom  = (int) (1.2 * sampleRate);
        const int tailCount = (int) (0.6 * sampleRate);

        const float coupledTail   = coupled.getRMSLevel(0, tailFrom, tailCount);
        const float uncoupledTail = uncoupled.getRMSLevel(0, tailFrom, tailCount);

        guitarStringsCouple = uncoupledTail > 1.0e-6f
                           && coupledTail > uncoupledTail * 1.02f;
    }

    // Group bus: a track that receives instead of generating (TrackType::Bus).
    // The risky part of that change is InstrumentTrack::render — a bus must
    // *not* clear the buffer its members already summed into, must still apply
    // its own gain and inserts, and must still be silenced by its own mute.
    // All four are checked here, because getting any of them wrong is silence
    // or a doubled signal rather than a subtle difference.
    bool groupBusWorks = false;
    {
        auto renderThroughBus = [&](float busGainDb, bool busMuted, bool routeIntoBus)
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack member;
            member.prepare(sampleRate, 512);
            member.active.store(true);

            InstrumentTrack busTrack;
            busTrack.prepare(sampleRate, 512);
            busTrack.active.store(true);
            busTrack.isBus.store(true);
            busTrack.gainDb.store(busGainDb);
            busTrack.muted.store(busMuted);

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            member.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockMix(2, n);
                blockMix.clear();

                // Exactly the order AudioEngine::processBlock uses: clear the
                // bus's input, render members into it, then render the bus.
                busTrack.prepareBusInput(n);

                if (routeIntoBus)
                    member.render(busTrack.busInput(), sendBus, noLiveMidi, context, false, false);
                else
                    member.render(blockMix, sendBus, noLiveMidi, context, false, false);

                busTrack.render(blockMix, sendBus, noLiveMidi, context, false, false);

                for (int ch = 0; ch < 2; ++ch)
                    mix.copyFrom(ch, pos, blockMix, ch, 0, n);
            }

            return mix;
        };

        const auto direct   = renderThroughBus(0.0f,  false, false); // member straight to the mix
        const auto throughBus = renderThroughBus(0.0f, false, true); // member via the bus
        const auto quietBus = renderThroughBus(-6.0f, false, true);
        const auto mutedBus = renderThroughBus(0.0f,  true,  true);

        const float rmsDirect  = direct.getRMSLevel(0, 0, direct.getNumSamples());
        const float rmsThrough = throughBus.getRMSLevel(0, 0, throughBus.getNumSamples());
        const float rmsQuietBus = quietBus.getRMSLevel(0, 0, quietBus.getNumSamples());
        const float rmsMuted   = mutedBus.getRMSLevel(0, 0, mutedBus.getNumSamples());

        const float busGainRatio = rmsThrough > 0.0f ? rmsQuietBus / rmsThrough : 0.0f;

        groupBusWorks = rmsDirect > 0.01f
                      // Routed through a unity bus, the group arrives intact:
                      // not silent (the bus cleared what it was given) and not
                      // doubled (it summed and then generated as well).
                      && std::abs(rmsThrough - rmsDirect) < rmsDirect * 0.02f
                      // The bus's own fader moves the whole group.
                      && busGainRatio > 0.47f && busGainRatio < 0.53f
                      // And muting the bus mutes the group, not just itself.
                      && rmsMuted < 1.0e-5f;
    }

    // Sidechain ducking: a steady tone compressed by a *separate* pulsing
    // signal, which is the whole feature — the bass has to dip where the kick
    // hits, not where the bass itself is loud.
    //
    // Measured here rather than headless because it is a claim about audio,
    // and it is the check that would catch the plumbing being wrong in the way
    // that matters: a compressor that quietly falls back to its own input
    // still compresses, still passes every "does it reduce gain" test, and is
    // completely useless. A steady tone can only dip *periodically* if the
    // detector really is the other signal.
    bool sidechainDucks = false;
    {
        const int totalSamples = (int) (sampleRate * 1.0);

        // The thing being ducked: a constant-amplitude tone, so any variation
        // in its output came from the sidechain and nothing else.
        juce::AudioBuffer<float> duckedTone(2, totalSamples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < totalSamples; ++i)
                duckedTone.setSample(ch, i, 0.4f * (float) std::sin(
                    2.0 * juce::MathConstants<double>::pi * 110.0 * i / sampleRate));

        // The detector: four short loud pulses a quarter-second apart.
        juce::AudioBuffer<float> kick(2, totalSamples);
        kick.clear();
        for (int pulse = 0; pulse < 4; ++pulse)
        {
            const int at = (int) (pulse * 0.25 * sampleRate);
            for (int i = 0; i < (int) (0.05 * sampleRate); ++i)
            {
                const int index = at + i;
                if (index >= totalSamples)
                    break;
                const double decay = std::exp(-20.0 * i / sampleRate);
                for (int ch = 0; ch < 2; ++ch)
                    kick.setSample(ch, index, (float) (0.9 * decay));
            }
        }

        CompressorEffect ducker;
        ducker.prepare(sampleRate, 512);
        ducker.setEnabled(true);
        ducker.setThresholdDb(-30.0f);
        ducker.setRatio(10.0f);
        ducker.setAttackMs(2.0f);
        ducker.setReleaseMs(120.0f);
        ducker.setSidechainInput(&kick);
        ducker.process(duckedTone);

        // At each pulse the tone must be pushed well down; between pulses it
        // must come back. Both halves matter: something permanently quieter is
        // not ducking, it is just a gain change.
        const int   window   = (int) (0.02 * sampleRate);
        float       atPulses = 0.0f;
        float       between  = 1.0f;

        for (int pulse = 0; pulse < 4; ++pulse)
        {
            const int hit = (int) (pulse * 0.25 * sampleRate) + (int) (0.005 * sampleRate);
            atPulses = juce::jmax(atPulses, duckedTone.getRMSLevel(0, hit, window));

            // Just before the next pulse, i.e. as released as it ever gets.
            const int recovered = (int) ((pulse + 1) * 0.25 * sampleRate) - window - 1;
            if (recovered > 0 && recovered + window < totalSamples)
                between = juce::jmin(between, duckedTone.getRMSLevel(0, recovered, window));
        }

        sidechainDucks = atPulses > 0.0f && between > 0.05f
                      && atPulses < between * 0.5f; // at least 6dB of duck

        // And with no sidechain routed, the same steady tone must come out
        // steady: proof the dip above is the routing and not the compressor
        // reacting to the tone itself.
        juce::AudioBuffer<float> unrouted(2, totalSamples);
        for (int ch = 0; ch < 2; ++ch)
            unrouted.copyFrom(ch, 0, duckedTone, ch, 0, totalSamples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < totalSamples; ++i)
                unrouted.setSample(ch, i, 0.4f * (float) std::sin(
                    2.0 * juce::MathConstants<double>::pi * 110.0 * i / sampleRate));

        CompressorEffect plain;
        plain.prepare(sampleRate, 512);
        plain.setEnabled(true);
        plain.setThresholdDb(-30.0f);
        plain.setRatio(10.0f);
        plain.setAttackMs(2.0f);
        plain.setReleaseMs(120.0f);
        plain.setSidechainInput(nullptr);
        plain.process(unrouted);

        const float plainEarly = unrouted.getRMSLevel(0, (int) (0.30 * sampleRate), window);
        const float plainLate  = unrouted.getRMSLevel(0, (int) (0.72 * sampleRate), window);
        const bool  plainSteady = plainEarly > 0.0f
                               && std::abs(plainLate - plainEarly) < plainEarly * 0.2f;

        sidechainDucks = sidechainDucks && plainSteady;
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

    // The Cyber Bass synth tone, end to end. Same purpose as the Modern
    // Metal check below: a preset made of ~20 hand-tuned fields can be
    // individually valid and still come out silent, thin, or clipping once
    // they're combined, and no per-field test sees that. Values come from
    // presetForSynthTone rather than being restated here.
    bool cyberBassHasWeight = false;
    {
        const auto preset = looper::model::presetForSynthTone(SynthTone::CyberBass);

        auto renderBass = [&](bool withPreset)
        {
            const int totalSamples = (int) (sampleRate * 1.5);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            if (withPreset)
            {
                const auto& s = preset.synth;
                track.synth.setWaveform(s.waveform);
                track.synth.setAttackMs(s.attackMs);
                track.synth.setDecayMs(s.decayMs);
                track.synth.setSustain(s.sustain);
                track.synth.setReleaseMs(s.releaseMs);
                track.synth.setFilterEnabled(s.filterEnabled);
                track.synth.setFilterMode(s.filterMode);
                track.synth.setFilterCutoff(s.filterCutoff);
                track.synth.setFilterResonance(s.filterResonance);
                track.synth.setFilterEnvAmount(s.filterEnvAmount);
                track.synth.setFilterEnvAttackMs(s.filterEnvAttackMs);
                track.synth.setFilterEnvDecayMs(s.filterEnvDecayMs);
                track.synth.setFilterEnvSustain(s.filterEnvSustain);
                track.synth.setFilterEnvReleaseMs(s.filterEnvReleaseMs);
                track.synth.setSubOscEnabled(s.subOscEnabled);
                track.synth.setSubOscLevel(s.subOscLevel);
                track.synth.setUnisonVoices(s.unisonVoices);
                track.synth.setUnisonDetuneCents(s.unisonDetuneCents);

                auto chain = std::make_unique<EffectChain>();
                for (const auto& slot : preset.effectChain)
                    if (auto node = nodeForSlot(slot))
                        chain->add(std::move(node));
                chain->prepare(sampleRate, 512);
                track.setEffectChain(chain.release());
            }

            // A held low note, which is what this sound is for.
            Pattern bassPattern;
            bassPattern.lengthBeats = 4.0;
            bassPattern.notes.push_back({ 0.0, 3.5, 36, 0.9f });

            ClipSlot slot;
            slot.pattern     = bassPattern;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
            }
            return mix;
        };

        const auto plain = renderBass(false);
        const auto cyber = renderBass(true);

        const float cyberRms  = cyber.getRMSLevel(0, 0, cyber.getNumSamples());
        const float cyberPeak = cyber.getMagnitude(0, 0, cyber.getNumSamples());

        // Measured as *harmonic richness*, not level. The default synth is a
        // bare sine, so at this note it has almost nothing above its own
        // fundamental; the preset is four detuned saws through a resonant
        // filter and a drive, which necessarily fills the band above it.
        // That ratio is what distinguishes "the preset was applied" from
        // "something made a sound" — RMS does not (measured: 0.136 vs 0.140,
        // i.e. indistinguishable), and a level check would therefore pass
        // for a preset wired into entirely the wrong fields.
        const double plainGrit = bandEnergy(plain, 300.0, 2000.0, sampleRate);
        const double cyberGrit = bandEnergy(cyber, 300.0, 2000.0, sampleRate);
        const double grit      = cyberGrit / std::max(plainGrit, 1.0e-18);

        cyberBassHasWeight = cyberRms > 1.0e-3f   // audible at all
                          && grit > 10.0          // and unmistakably not a sine
                          && cyberPeak < 1.0f;    // without clipping the bus

        std::cout << "cyber bass: rms=" << cyberRms << " peak=" << cyberPeak
                  << " grit=" << grit << "\n";
    }

    // The Modern Metal tone template, end to end: a plucked note through the
    // exact chain the button applies. This exists because the first version
    // of that preset shipped audibly thin - Drive's make-up gain falls as
    // 1/sqrt(drive) (see DriveEffect::process), so pushing drive near its
    // ceiling quietly cut the output to a fraction of its input, which no
    // per-pedal test could see. Values come from presetForGuitarTone rather
    // than being restated, so retuning the preset re-checks the real thing.
    bool metalToneHasBody = false;
    {
        const auto preset = looper::model::presetForGuitarTone(GuitarTone::ModernMetal);

        auto renderThroughPreset = [&](bool withChain)
        {
            const int totalSamples = (int) (sampleRate * 1.5);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);
            guitar.setDecaySeconds(preset.guitar.decaySeconds);
            guitar.setBrightness(preset.guitar.brightness);
            guitar.setPickPosition(preset.guitar.pickPosition);
            guitar.setPickHardness(preset.guitar.pickHardness);

            // The tuning matters as much as the pedals here: a GuitarNode
            // left in standard tuning can't reach a dropped low note at all
            // (it's below every open string), so it renders silence. This is
            // the engine-side half of what AudioEngine::setTrackGuitarTuning
            // does for a real track.
            for (int s = 0; s < kNumGuitarStrings; ++s)
                guitar.setOpenNote(s, preset.guitar.tuning[(size_t) s]);

            EffectChain chain;
            if (withChain)
                for (const auto& slot : preset.effectChain)
                    if (auto node = nodeForSlot(slot))
                        chain.add(std::move(node));
            chain.prepare(sampleRate, 512);

            // The preset's own lowest string, which is the note this tone is
            // actually about - drop C's low C2 rather than a standard low E.
            const int lowString = preset.guitar.tuning[0];

            juce::MidiBuffer midi;
            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                midi.clear();
                if (pos == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, lowString, 0.9f), 0);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                OfflineRenderer::fillTransport(context, pos, n, bpm, sampleRate);
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                juce::AudioBuffer<float> view(mix.getArrayOfWritePointers(), 2, pos, n);
                guitar.process(view, midi, context);
                chain.process(view);
            }
            return mix;
        };

        const auto dry     = renderThroughPreset(false);
        const auto through = renderThroughPreset(true);

        const float dryRms  = dry.getRMSLevel(0, 0, dry.getNumSamples());
        const float wetRms  = through.getRMSLevel(0, 0, through.getNumSamples());
        const float wetPeak = through.getMagnitude(0, 0, through.getNumSamples());

        // Three bands, because a guitar tone is not one number.
        //
        // This replaces a single `growl` ratio of 90-600Hz over 2-6kHz that
        // the presets were previously tuned against. That ratio was actively
        // wrong: 2-6kHz is the *presence band an electric guitar lives in*,
        // so maximising the ratio rewarded darkness and punished exactly the
        // region that makes a guitar cut. Two rounds of parameter tuning
        // chased it and produced a tone the user described as still not
        // sounding like a guitar. It is deliberately not kept alongside the
        // new figures — leaving it would keep the incentive that caused this.
        const double bodyBand     = bandEnergy(through, 90.0, 250.0, sampleRate);
        const double presenceBand = bandEnergy(through, 2000.0, 5000.0, sampleRate);
        const double fizzBand     = bandEnergy(through, 8000.0, 16000.0, sampleRate);
        const double midBand      = bandEnergy(through, 200.0, 5000.0, sampleRate);

        const double reference = std::max(midBand, 1.0e-18);
        const double presence  = presenceBand / reference;
        const double fizz      = fizzBand / reference;
        const double body      = bodyBand / reference;

        // Presence and fizz have to be judged *together*, which is precisely
        // what one ratio could not express: a tone can be bright because it
        // cuts, or bright because it is fizzy, and those want opposite
        // responses. A real cabinet leaves the first and destroys the second.
        metalToneHasBody = dryRms > 1.0e-4f
                        && wetRms >= dryRms
                        && wetPeak < 1.0f
                        && presence > 0.05   // it has to cut, not just rumble
                        && fizz < presence   // and the cab has to kill the top
                        && body > 0.02;      // while still having weight

        std::cout << "modern metal tone: dryRms=" << dryRms << " wetRms=" << wetRms
                  << " wetPeak=" << wetPeak
                  << " presence=" << presence << " fizz=" << fizz
                  << " body=" << body << "\n";
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

        auto runRack = [&](const looper::model::MasteringSettings& settings)
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

        const auto dry = runRack(looper::model::MasteringSettings {}); // disabled: must be untouched
        const auto raw = makeMix();
        masteringChangesSound = worstBufferDifference(dry, raw) < 1.0e-9f; // bypassed really is bypassed

        // Every preset's ceiling must hold on this deliberately nasty input.
        masteringHoldsCeiling = true;
        for (int i = 0; i < looper::engine::kNumMasteringPresets; ++i)
        {
            const auto preset   = (looper::engine::MasteringPreset) i;
            const auto settings = looper::model::presetForMastering(preset);
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
                    std::cout << "  ceiling breached by " << looper::engine::masteringPresetName(preset)
                              << ": peak=" << peak << " allowed=" << allowed << "\n";
                }

                // And it must actually be doing something.
                if (worstBufferDifference(out, raw) < 1.0e-4f)
                {
                    masteringChangesSound = false;
                    std::cout << "  " << looper::engine::masteringPresetName(preset)
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false, barSamples);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

            juce::AudioBuffer<float> sendBus(2, 512);
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

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
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

    looper::model::AutomationLane lane;
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
                                                            std::vector<double>{}, std::vector<float>{},
                                                            false, 0.5f, 0.5f, 0.0f,
                                                            bpm, sampleRate, seconds, 512, &perTrackCurves);
    const auto bassAloneNoAuto    = OfflineRenderer::render({ arp, bass }, { -100.0f, 0.0f }, std::vector<bool>{},
                                                            std::vector<double>{}, std::vector<float>{},
                                                            false, 0.5f, 0.5f, 0.0f,
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
              << "  sendBusChanged=" << (sendBusChanged ? 1 : 0)
              << "  sendBusDelayWorks=" << (sendBusDelayWorks ? 1 : 0)
              << "  multiClipGates=" << (multiClipGates ? 1 : 0)
              << "  audioTrackWorks=" << (audioTrackWorks ? 1 : 0)
              << "  multiClipAudioGates=" << (multiClipAudioGates ? 1 : 0)
              << "  midiRoundTripWorks=" << (midiRoundTripWorks ? 1 : 0)
              << "  midiRecordingWorks=" << (midiRecordingWorks ? 1 : 0)
              << "  warpFitsTheGrid=" << (warpFitsTheGrid ? 1 : 0)
              << "  drumKitWorks=" << (drumKitWorks ? 1 : 0)
              << "  generativeLoopWorks=" << (generativeLoopWorks ? 1 : 0)
              << "  drumPadMixWorks=" << (drumPadMixWorks ? 1 : 0)
              << "  drumPadPitchWorks=" << (drumPadPitchWorks ? 1 : 0)
              << "  pluginsScanned=" << pluginsScanned
              << "  pluginHostWorks=" << (pluginHostWorks ? 1 : 0)
              << "  guitarSounds=" << (guitarSounds ? 1 : 0)
              << "  guitarCutsSameString=" << (guitarCutsSameString ? 1 : 0)
              << "  guitarPlaysSixAtOnce=" << (guitarPlaysSixAtOnce ? 1 : 0)
              << "  guitarPicksLowestFret=" << (guitarPicksLowestFret ? 1 : 0)
              << "  guitarHammerOn=" << (guitarHammerOn ? 1 : 0)
              << "  guitarPalmMuteChugs=" << (guitarPalmMuteChugs ? 1 : 0)
              << "  tempoChangeMovesNotes=" << (tempoChangeMovesNotes ? 1 : 0)
              << "  tempoRampAccelerates=" << (tempoRampAccelerates ? 1 : 0)
              << "  guitarStopsOnTransportStop=" << (guitarStopsOnTransportStop ? 1 : 0)
              << "  chorusChangesSound=" << (chorusChangesSound ? 1 : 0)
              << "  chorusDepthMatters=" << (chorusDepthMatters ? 1 : 0)
              << "  wobbleChangesSound=" << (wobbleChangesSound ? 1 : 0)
              << "  wobbleDepthMatters=" << (wobbleDepthMatters ? 1 : 0)
              << "  wobbleTracksTempo=" << (wobbleTracksTempo ? 1 : 0)
              << "  filterEnvChangesSound=" << (filterEnvChangesSound ? 1 : 0)
              << "  subOscChangesSound=" << (subOscChangesSound ? 1 : 0)
              << "  unisonChangesSound=" << (unisonChangesSound ? 1 : 0)
              << "  compressorSquashes=" << (compressorSquashes ? 1 : 0)
              << "  sidechainDucks=" << (sidechainDucks ? 1 : 0)
              << "  groupBusWorks=" << (groupBusWorks ? 1 : 0)
              << "  guitarHasWidth=" << (guitarHasWidth ? 1 : 0)
              << "  guitarStringsCouple=" << (guitarStringsCouple ? 1 : 0)
              << "  guitarStiffnessChangesTone=" << (guitarStiffnessChangesTone ? 1 : 0)
              << "  cascadedStagesEnrich=" << (cascadedStagesEnrich ? 1 : 0)
              << "  pianoSounds=" << (pianoSounds ? 1 : 0)
              << "  pianoDamps=" << (pianoDamps ? 1 : 0)
              << "  pianoTopRings=" << (pianoTopRings ? 1 : 0)
              << "  pianoAffordsPolyphony=" << (pianoAffordsPolyphony ? 1 : 0)
              << "  pianoPedalSustains=" << (pianoPedalSustains ? 1 : 0)
              << "  pianoPedalLifts=" << (pianoPedalLifts ? 1 : 0)
              << "  pianoResonates=" << (pianoResonates ? 1 : 0)
              << "  pianoPedalStaysStable=" << (pianoPedalStaysStable ? 1 : 0)
              << "  tremoloModulates=" << (tremoloModulates ? 1 : 0)
              << "  gateClosesQuiet=" << (gateClosesQuiet ? 1 : 0)
              << "  metalToneHasBody=" << (metalToneHasBody ? 1 : 0)
              << "  cyberBassHasWeight=" << (cyberBassHasWeight ? 1 : 0)
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
    // correctly silencing the other track, a clip start that gates playback, a
    // send bus that changes the output whether it's reverb or delay, two
    // MIDI clips on one track each sounding only in their own window, a
    // decoded audio clip playing back through a track, two AUDIO clips on one
    // track likewise each sounding only in their own window, a MIDI file
    // export/import round trip that preserves tempo and every note, a drum
    // kit playing the right pad's one-shot sample at the right times while
    // an unassigned pad stays silent, per-pad gain/pan/mute and transposition
    // each doing what they say against that same kit, and the recorder's capture/handoff
    // logic (fed synthetic input, since there's no live mic here) together
    // confirm
    // the full render/gain/fx/automation/solo/clip/send-bus/audio/midi/drum/record path.
    const bool ok = rmsDry > 0.0f && std::isfinite(rmsDry)
                 && gainRatio > 0.47f && gainRatio < 0.53f
                 && delayChanged && filterAttenuates && reverbChanged && automationFades
                 && perTrackAutomationWorks
                 && soloMatchesArpOnly && stemsSumToMix && clipStartGates && sendBusChanged && sendBusDelayWorks && multiClipGates
                 && audioTrackWorks && multiClipAudioGates && midiRoundTripWorks && drumKitWorks
                 && midiRecordingWorks && warpFitsTheGrid && sidechainDucks && groupBusWorks
                 && guitarHasWidth && guitarStringsCouple && guitarStiffnessChangesTone
                 && cascadedStagesEnrich
                 && pianoSounds && pianoDamps && pianoTopRings && pianoAffordsPolyphony
                 && pianoPedalSustains && pianoPedalLifts && pianoResonates
                 && pianoPedalStaysStable
                 && generativeLoopWorks
                 && drumPadMixWorks && drumPadPitchWorks
                 && pluginHostWorks
                 && guitarSounds && guitarCutsSameString && guitarPlaysSixAtOnce
                 && guitarPicksLowestFret && guitarHammerOn && guitarPalmMuteChugs && tempoChangeMovesNotes && tempoRampAccelerates && guitarStopsOnTransportStop
                 && filterEnvChangesSound && subOscChangesSound && unisonChangesSound
                 && effectChainOrderMatters && effectChainRunsAllNodes
                 && sessionLaunchQuantizes && sessionStopWorks
                 && trackPanWorks && panAutomationWorks && trackInsertFilterWorks
                 && metronomeWorks && metronomeSilentWhenOff && recorderWorks;
    return ok ? 0 : 2;
}
