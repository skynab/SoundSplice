#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/AudioClipSlot.h"
#include "engine/AudioRecorder.h"
#include "engine/AuditionPlayer.h"
#include "engine/ClipData.h"
#include "engine/DelayEffect.h"
#include "engine/EffectChain.h"
#include "engine/EffectSlotFactory.h"
#include "engine/FilterEffect.h"
#include "engine/MixerTrack.h"
#include "engine/MasteringPreset.h"
#include "engine/MasteringProcessor.h"
#include "engine/Metronome.h"
#include "engine/OfflineRenderer.h"
#include "engine/ReverbEffect.h"
#include "model/AutomationLane.h"
#include "model/MasteringPresets.h"
#include "model/Song.h"

/*
    End-to-end render checks: the engine's audio paths driven the way the app
    drives them, and the sound that comes out measured. Each is a claim about
    what reaches the speakers - a clip that starts where it says, an effect
    that is really in the chain, a recorder that loses nothing - which the
    per-DSP tests in tests/engine can't make on their own.

    These were soundsplice_bounce's checks, one main() with a single pass or
    fail. They're test cases here so a failure says which one, and so the
    seven the tool printed but never failed on now count.
*/

using namespace soundsplice::engine;

namespace
{
    constexpr double kBpm        = 120.0;
    constexpr double kSampleRate = 44100.0;
    constexpr int    kBlock      = 512;

    float rms(const juce::AudioBuffer<float>& buffer, int channel = 0)
    {
        return buffer.getRMSLevel(channel, 0, buffer.getNumSamples());
    }

    float rmsBetween(const juce::AudioBuffer<float>& buffer, double fromSeconds, double seconds, int channel = 0)
    {
        return buffer.getRMSLevel(channel, (int) (fromSeconds * kSampleRate), (int) (seconds * kSampleRate));
    }

    /** The largest sample-for-sample difference across every channel. */
    float worstDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const int channels = std::min(a.getNumChannels(), b.getNumChannels());
        const int samples  = std::min(a.getNumSamples(), b.getNumSamples());
        float     worst    = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < samples; ++n)
                worst = std::max(worst, std::abs(a.getSample(ch, n) - b.getSample(ch, n)));
        return worst;
    }

    /** As worstDifference, the left channel only. */
    float leftDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const int samples = std::min(a.getNumSamples(), b.getNumSamples());
        float     worst   = 0.0f;
        for (int n = 0; n < samples; ++n)
            worst = std::max(worst, std::abs(a.getSample(0, n) - b.getSample(0, n)));
        return worst;
    }

    /** One note of a part, in beats at kBpm. */
    struct PartNote
    {
        double start  = 0.0;
        double length = 0.0;
        int    note   = 60; // a semitone number, 69 being A 440
        float  level  = 0.8f;
    };

    /** @p notes, repeated every four beats for @p seconds, as a mono clip:
        sines through an envelope, as a simple synth plays them, so a part has
        attacks, sustains and the quiet between notes for effects to act on. */
    ClipData part(const std::vector<PartNote>& notes, double seconds = 8.0)
    {
        ClipData clip;
        const int n = (int) (seconds * kSampleRate);
        clip.audio.setSize(1, n);
        clip.audio.clear();
        clip.sourceSampleRate = kSampleRate;
        clip.numChannels      = 1;
        clip.lengthSamples    = n;

        const double samplesPerBeat = kSampleRate * 60.0 / kBpm;
        for (double bar = 0.0; bar * samplesPerBeat < n; bar += 4.0)
            for (const auto& note : notes)
            {
                juce::ADSR envelope;
                envelope.setSampleRate(kSampleRate);
                envelope.setParameters({ 0.005f, 0.12f, 0.7f, 0.25f });
                envelope.noteOn();

                const int    from = (int) ((bar + note.start) * samplesPerBeat);
                const int    off  = (int) ((bar + note.start + note.length) * samplesPerBeat);
                const double hz   = 440.0 * std::pow(2.0, (note.note - 69) / 12.0);
                for (int i = from; i < n; ++i)
                {
                    if (i == off)
                        envelope.noteOff();
                    if (i >= off && ! envelope.isActive())
                        break;
                    const double t = (double) (i - from) / kSampleRate;
                    clip.audio.addSample(0, i, note.level * 0.3f * envelope.getNextSample()
                                                   * (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * t));
                }
            }
        return clip;
    }

    /** C-E-G-C, one note a beat. */
    ClipData arpeggio()
    {
        return part({ { 0.0, 0.5, 60, 0.8f }, { 1.0, 0.5, 64, 0.8f }, { 2.0, 0.5, 67, 0.8f }, { 3.0, 0.5, 72, 0.8f } });
    }

    /** A root-note bass on beats 1 and 3. */
    ClipData bassline()
    {
        return part({ { 0.0, 1.0, 36, 0.9f }, { 2.0, 1.0, 43, 0.9f } });
    }

    /** A mono sine of @p seconds at @p hz. */
    ClipData tone(double seconds, double hz, float level)
    {
        ClipData clip;
        const int n = (int) (seconds * kSampleRate);
        clip.audio.setSize(1, n);
        clip.sourceSampleRate = kSampleRate;
        clip.numChannels      = 1;
        clip.lengthSamples    = n;
        for (int i = 0; i < n; ++i)
            clip.audio.setSample(0, i, level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / kSampleRate));
        return clip;
    }

    /** One MixerTrack rendered block by block, as the live callback does. */
    struct TrackRender
    {
        double          seconds = 1.0;
        double          bpm     = kBpm;
        const ClipData* clip    = nullptr; // from beat 0; none for no clip

        std::function<void(MixerTrack&)>      setUp;       // before rendering
        std::function<void(MixerTrack&, int)> beforeBlock; // with the block's start sample
    };

    juce::AudioBuffer<float> render(const TrackRender& spec)
    {
        const int                totalSamples = (int) (kSampleRate * spec.seconds);
        juce::AudioBuffer<float> mix(2, totalSamples);
        mix.clear();

        MixerTrack track;
        track.prepare(kSampleRate, kBlock);
        if (spec.setUp)
            spec.setUp(track);

        if (spec.clip != nullptr)
            track.audioPlayer.submitSingleClip(new ClipData(*spec.clip));

        for (int pos = 0; pos < totalSamples; pos += kBlock)
        {
            const int n = std::min(kBlock, totalSamples - pos);
            if (spec.beforeBlock)
                spec.beforeBlock(track, pos);

            ProcessContext context;
            context.sampleRate         = kSampleRate;
            context.numSamples         = n;
            context.transport.playing  = true;
            OfflineRenderer::fillTransport(context, pos, n, spec.bpm, kSampleRate);
            context.transport.timeSigNumerator   = 4;
            context.transport.timeSigDenominator = 4;

            juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
            track.render(blockView, context, false);
        }
        return mix;
    }

    /** Gives @p track a chain of @p nodes, in order. */
    void setChain(MixerTrack& track, std::vector<std::unique_ptr<EffectProcessor>> nodes)
    {
        auto chain = std::make_unique<EffectChain>();
        for (auto& node : nodes)
            chain->add(std::move(node));
        chain->prepare(kSampleRate, kBlock);
        track.setEffectChain(chain.release());
    }

    template <typename Node, typename Configure>
    std::function<void(MixerTrack&)> withNode(Configure configure)
    {
        return [configure](MixerTrack& track)
        {
            auto node = std::make_unique<Node>();
            node->effect.setEnabled(true);
            configure(node->effect);
            std::vector<std::unique_ptr<EffectProcessor>> nodes;
            nodes.push_back(std::move(node));
            setChain(track, std::move(nodes));
        };
    }
}

// ---- Mixing ---------------------------------------------------------------------

TEST_CASE("Track gain, solo and stems mix as they should", "[render]")
{
    const auto arp = arpeggio(), bass = bassline();

    // -6 dB roughly halves the level (10^(-6/20) ~= 0.501).
    const auto full  = OfflineRenderer::render({ arp }, std::vector<float> { 0.0f }, kBpm, kSampleRate, 4.0);
    const auto quiet = OfflineRenderer::render({ arp }, std::vector<float> { -6.0f }, kBpm, kSampleRate, 4.0);
    REQUIRE(rms(full) > 0.0f);
    const float ratio = rms(quiet) / rms(full);
    REQUIRE(ratio > 0.47f);
    REQUIRE(ratio < 0.53f);

    // Soloing the arp silences the bass completely.
    const auto soloArp = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { true, false }, kBpm, kSampleRate, 4.0);
    REQUIRE(std::abs(rms(soloArp) - rms(full)) < 1.0e-4f);

    // Stems sum to the mix: what stem export rests on. Against the render
    // without the master bus, which stems leave out so that this holds.
    const auto stemBass = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { false, true }, kBpm, kSampleRate, 4.0);
    const auto both     = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { false, false }, kBpm, kSampleRate, 4.0);

    juce::AudioBuffer<float> summed(both.getNumChannels(), both.getNumSamples());
    summed.clear();
    for (int ch = 0; ch < summed.getNumChannels(); ++ch)
    {
        summed.addFrom(ch, 0, soloArp, ch, 0, summed.getNumSamples());
        summed.addFrom(ch, 0, stemBass, ch, 0, summed.getNumSamples());
    }
    REQUIRE(rms(soloArp) > 1.0e-4f); // silent stems would sum to a silent mix and prove nothing
    REQUIRE(rms(stemBass) > 1.0e-4f);
    REQUIRE(worstDifference(summed, both) < 1.0e-6f);
}

TEST_CASE("A track's pan moves it between the channels, and centred changes nothing", "[render]")
{
    const auto arp     = arpeggio();
    const auto panned  = [&](float pan)
    {
        return render({ 2.0, kBpm, &arp, [pan](MixerTrack& track) { track.pan.store(pan); } });
    };
    const auto centred = panned(0.0f);
    const auto left    = panned(-1.0f);

    REQUIRE(rms(centred, 0) > 0.01f);
    REQUIRE(std::abs(rms(centred, 0) - rms(centred, 1)) < 1.0e-6f);
    REQUIRE(rms(left, 0) > 0.01f);
    REQUIRE(rms(left, 1) < 1.0e-6f);
    REQUIRE(std::abs(rms(left, 0) - rms(centred, 0)) < 1.0e-6f); // the unity-centre pan law
}

// ---- Clips --------------------------------------------------------------------------

TEST_CASE("Audio clips play through a track's gain, windows, offsets and fades", "[render]")
{
    const auto sine = tone(2.0, 440.0, 0.5f);

    // Through the track's gain, gated at its start.
    const auto full    = OfflineRenderer::renderAudioClip(sine, 0.0, 0.0f, kBpm, kSampleRate, 4.0);
    const auto quiet   = OfflineRenderer::renderAudioClip(sine, 0.0, -6.0f, kBpm, kSampleRate, 4.0);
    const auto delayed = OfflineRenderer::renderAudioClip(sine, 2.0, 0.0f, kBpm, kSampleRate, 4.0);
    REQUIRE(rms(full) > 0.01f);
    const float ratio = rms(quiet) / rms(full);
    REQUIRE(ratio > 0.47f);
    REQUIRE(ratio < 0.53f);
    REQUIRE(rmsBetween(delayed, 0.0, 1.0) < 1.0e-5f);
    REQUIRE(rmsBetween(delayed, 1.0, 3.0) > 0.01f);

    const auto slot = [&](double start, double offset = 0.0, double fade = 0.0)
    {
        AudioClipSlot clip;
        clip.clipData            = std::make_shared<ClipData>(sine);
        clip.startBeats          = start;
        clip.lengthBeats         = 4.0;
        clip.sourceOffsetSeconds = offset;
        clip.fades.inSeconds     = fade;
        clip.fades.outSeconds    = fade;
        return clip;
    };

    // Two clips on one track, 0-4 and 6-10 beats. No envelope tail, so the gap
    // is silent right away.
    const auto two = OfflineRenderer::renderAudioClips({ slot(0.0), slot(6.0) }, 0.0f, kBpm, kSampleRate, 6.0);
    REQUIRE(rmsBetween(two, 0.0, 2.0) > 0.01f);
    REQUIRE(rmsBetween(two, 2.5, 0.5) < 1.0e-5f);
    REQUIRE(rmsBetween(two, 3.0, 2.0) > 0.01f);

    // From a second into the two-second file: it runs out halfway through
    // its two-second window, which it only does if it really starts there.
    const auto offset = OfflineRenderer::renderAudioClips({ slot(0.0, 1.0) }, 0.0f, kBpm, kSampleRate, 2.0);
    REQUIRE(rmsBetween(offset, 0.0, 0.5) > 0.01f);
    REQUIRE(rmsBetween(offset, 1.5, 0.5) < 1.0e-5f);

    // Half-second fades at both ends: the first and last eighth of a second
    // are deep in them, the middle untouched.
    const auto faded = OfflineRenderer::renderAudioClips({ slot(0.0, 0.0, 0.5) }, 0.0f, kBpm, kSampleRate, 2.0);
    const float body = rmsBetween(faded, 0.75, 0.5);
    REQUIRE(body > 0.3f);
    REQUIRE(rmsBetween(faded, 0.0, 0.125) < 0.25f * body);
    REQUIRE(rmsBetween(faded, 1.875, 0.125) < 0.25f * body);
}

TEST_CASE("The audition player plays without a transport, ends, restarts and stops", "[render]")
{
    const auto sine = tone(2.0, 440.0, 0.5f);

    AuditionPlayer audition;
    audition.prepare(kSampleRate);
    audition.play(std::make_unique<ClipData>(sine));

    juce::AudioBuffer<float> block(2, kBlock);
    double                   opening = 0.0;
    float                    last    = 1.0f;
    for (int b = 0; b < (int) std::ceil(2.5 * kSampleRate / kBlock); ++b)
    {
        block.clear();
        audition.process(block, kBlock);
        if (b < 10)
            opening += block.getRMSLevel(0, 0, kBlock);
        last = block.getRMSLevel(0, 0, kBlock);
    }
    REQUIRE(opening > 0.1);
    REQUIRE(last < 1.0e-6f);
    REQUIRE_FALSE(audition.isPlaying());

    audition.play(std::make_unique<ClipData>(sine)); // from the top
    block.clear();
    audition.process(block, kBlock);
    REQUIRE(block.getRMSLevel(0, 0, kBlock) > 0.01f);
    REQUIRE(audition.isPlaying());

    audition.stop();
    block.clear();
    audition.process(block, kBlock);
    REQUIRE(block.getRMSLevel(0, 0, kBlock) < 1.0e-6f);
    REQUIRE_FALSE(audition.isPlaying());
    audition.collectRetired();
}

// ---- Automation ----------------------------------------------------------------------

TEST_CASE("Automation fades a track, and leaves its neighbour alone", "[render]")
{
    const auto arp = arpeggio(), bass = bassline();

    // A master-gain lane from -40 to 0 dB over the render, applied by hand as
    // the master does it.
    auto mix = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, kBpm, kSampleRate, 4.0);
    soundsplice::model::AutomationLane lane;
    lane.addPoint(0.0, -40.0f);
    lane.addPoint(kBpm / 60.0 * 4.0, 0.0f);
    for (int i = 0; i < mix.getNumSamples(); ++i)
    {
        const float gain = juce::Decibels::decibelsToGain(lane.valueAt((double) i / (kSampleRate * 60.0 / kBpm), 0.0f));
        for (int ch = 0; ch < mix.getNumChannels(); ++ch)
            mix.getWritePointer(ch)[i] *= gain;
    }
    REQUIRE(rmsBetween(mix, 0.0, 2.0) < rmsBetween(mix, 2.0, 2.0));

    // Per track, through the renderer: the arp fades, the bass, with no
    // curve, holds. Each isolated by turning the other right down.
    TrackAutomation fade;
    fade.gain.addPoint(0.0, -40.0f);
    fade.gain.addPoint(kBpm / 60.0 * 4.0, 0.0f);
    const OfflineRenderer::TrackAutomationList curves { fade, TrackAutomation {} };

    const auto arpAlone  = OfflineRenderer::render({ arp, bass }, { 0.0f, -100.0f }, std::vector<bool> {},
                                                   std::vector<double> {}, kBpm, kSampleRate, 4.0, kBlock, &curves);
    const auto bassAlone = OfflineRenderer::render({ arp, bass }, { -100.0f, 0.0f }, std::vector<bool> {},
                                                   std::vector<double> {}, kBpm, kSampleRate, 4.0, kBlock, &curves);
    REQUIRE(rmsBetween(arpAlone, 0.0, 2.0) < rmsBetween(arpAlone, 2.0, 2.0));

    // Loosely: the first pass starts from silence where the second has the
    // last bar's tail, but a leaked curve would differ by a multiple.
    const float bassFirst = rmsBetween(bassAlone, 0.0, 2.0), bassSecond = rmsBetween(bassAlone, 2.0, 2.0);
    REQUIRE(bassSecond > 0.01f);
    REQUIRE(std::abs(bassSecond - bassFirst) < 0.25f * std::max(bassFirst, bassSecond));
}

TEST_CASE("Pan automation sweeps a track from left to right", "[render]")
{
    TrackAutomation sweep;
    sweep.pan.addPoint(0.0, -1.0f);
    sweep.pan.addPoint(4.0 * kBpm / 60.0, 1.0f);
    const OfflineRenderer::TrackAutomationList curves { sweep };

    const auto swept = OfflineRenderer::render({ arpeggio() }, { 0.0f }, {}, {}, kBpm, kSampleRate, 4.0, kBlock, &curves);
    REQUIRE(rmsBetween(swept, 0.0, 0.5, 0) > rmsBetween(swept, 0.0, 0.5, 1) * 2.0f);
    REQUIRE(rmsBetween(swept, 3.5, 0.5, 1) > rmsBetween(swept, 3.5, 0.5, 0) * 2.0f);
}

// ---- Effects --------------------------------------------------------------------------

TEST_CASE("The master delay, filter and reverb each change the mix", "[render]")
{
    const auto dry = OfflineRenderer::render({ arpeggio(), bassline() }, { 0.0f, 0.0f }, kBpm, kSampleRate, 4.0);
    const auto through = [&](auto effect, auto configure)
    {
        juce::AudioBuffer<float> wet(dry);
        effect.prepare(kSampleRate, kBlock);
        effect.setEnabled(true);
        configure(effect);
        effect.process(wet);
        return wet;
    };

    const auto delayed = through(DelayEffect {}, [](DelayEffect& delay)
    {
        delay.setTimeMs(250.0f);
        delay.setFeedback(0.4f);
        delay.setMix(0.5f);
    });
    REQUIRE(std::abs(rms(delayed) - rms(dry)) > 1.0e-4f);

    const auto lowPassed = through(FilterEffect {}, [](FilterEffect& filter) // well below the notes
    {
        filter.setMode(0);
        filter.setCutoff(150.0f);
        filter.setResonance(0.707f);
    });
    REQUIRE(rms(lowPassed) < rms(dry));

    const auto reverbed = through(ReverbEffect {}, [](ReverbEffect& reverb)
    {
        reverb.setRoomSize(0.7f);
        reverb.setDamping(0.4f);
        reverb.setMix(0.4f);
    });
    REQUIRE(std::abs(rms(reverbed) - rms(dry)) > 1.0e-4f);
}

TEST_CASE("A track's insert filter is in its signal path", "[render]")
{
    const auto arp      = arpeggio();
    const auto withLowPass = [&](bool enabled)
    {
        return render({ 2.0, kBpm, &arp, [enabled](MixerTrack& track)
        {
            auto node = std::make_unique<FilterNode>();
            node->effect.setEnabled(enabled);
            node->effect.setMode(0);
            node->effect.setCutoff(150.0f);
            node->effect.setResonance(0.707f);
            std::vector<std::unique_ptr<EffectProcessor>> nodes;
            nodes.push_back(std::move(node));
            setChain(track, std::move(nodes));
        } });
    };
    const float plain = rms(withLowPass(false));
    REQUIRE(plain > 0.01f);
    REQUIRE(rms(withLowPass(true)) < plain * 0.9f);
}

TEST_CASE("An effect chain runs every node, in order", "[render]")
{
    // Gain and a hard clip: unlike the built-in filter, delay and reverb,
    // which are linear and so commute, these don't - halving before clipping
    // isn't clipping before halving. This tests EffectChain, not the DSP.
    struct Gain final : EffectProcessor
    {
        EffectKind kind() const noexcept override { return EffectKind::Filter; }
        void       prepare(double, int) override {}
        void       setEnabled(bool) override {}
        void       process(juce::AudioBuffer<float>& buffer) override { buffer.applyGain(0.25f); }
    };
    struct Clip final : EffectProcessor
    {
        EffectKind kind() const noexcept override { return EffectKind::Filter; }
        void       prepare(double, int) override {}
        void       setEnabled(bool) override {}
        void       process(juce::AudioBuffer<float>& buffer) override
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    buffer.setSample(ch, i, juce::jlimit(-0.02f, 0.02f, buffer.getSample(ch, i)));
        }
    };

    const auto arp   = arpeggio();
    const auto chain = [&](std::function<std::vector<std::unique_ptr<EffectProcessor>>()> nodes)
    {
        return render({ 1.0, kBpm, &arp, [nodes](MixerTrack& track) { setChain(track, nodes()); } });
    };
    const auto gainThenClip = chain([] {
        std::vector<std::unique_ptr<EffectProcessor>> n;
        n.push_back(std::make_unique<Gain>());
        n.push_back(std::make_unique<Clip>());
        return n;
    });
    const auto clipThenGain = chain([] {
        std::vector<std::unique_ptr<EffectProcessor>> n;
        n.push_back(std::make_unique<Clip>());
        n.push_back(std::make_unique<Gain>());
        return n;
    });
    const auto gainOnly = chain([] {
        std::vector<std::unique_ptr<EffectProcessor>> n;
        n.push_back(std::make_unique<Gain>());
        return n;
    });

    REQUIRE(rms(gainThenClip) > 1.0e-4f);
    REQUIRE(leftDifference(gainThenClip, clipThenGain) > 1.0e-3f);
    REQUIRE(leftDifference(gainThenClip, gainOnly) > 1.0e-3f); // the second node ran
}

TEST_CASE("A drive changes the sound, and its cabinet changes it again", "[render]")
{
    const auto arp   = arpeggio();
    const auto drive = [&](bool cabinet)
    {
        return render({ 1.0, kBpm, &arp, withNode<DriveNode>([cabinet](auto& effect)
        {
            effect.setDrive(12.0f);
            effect.setTone(0.5f);
            effect.setLevel(0.8f);
            effect.setCabinet(cabinet);
        }) });
    };
    const auto clean   = render({ 1.0, kBpm, &arp });
    const auto withCab = drive(true);

    REQUIRE(rms(clean) > 1.0e-4f);
    REQUIRE(leftDifference(clean, withCab) > 1.0e-3f);
    REQUIRE(leftDifference(withCab, drive(false)) > 1.0e-3f); // the cabinet is really in the path
}

TEST_CASE("Cascaded drive stages make the spectrum denser", "[render]")
{
    // Not "compresses more": sharing the drive out as the n-th root keeps the
    // total push the same, and three gentle stages have a softer knee than
    // one hard one. What a cascade does is multiply harmonics - each stage
    // distorts a signal that already has them - and that's what's measured:
    // harmonics 5 to 9 against the fundamental.
    const auto driven = [](int stages)
    {
        juce::AudioBuffer<float> buffer(1, (int) (kSampleRate * 0.5));
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample(0, i, 0.3f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / kSampleRate));

        DriveEffect drive;
        drive.prepare(kSampleRate, kBlock);
        drive.setEnabled(true);
        drive.setDrive(12.0f);
        drive.setTone(0.5f);
        drive.setLevel(1.0f);
        drive.setHardClip(false);
        drive.setCabinet(false); // linear, and only muddies the measurement
        drive.setStages(stages);
        drive.process(buffer);
        return buffer;
    };
    const auto density = [](const juce::AudioBuffer<float>& signal)
    {
        const int  from = (int) (0.1 * kSampleRate), count = (int) (0.3 * kSampleRate);
        const auto bin  = [&](double hz)
        {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < count; ++i)
            {
                const double angle = 2.0 * juce::MathConstants<double>::pi * hz * (double) i / kSampleRate;
                re += signal.getSample(0, from + i) * std::cos(angle);
                im += signal.getSample(0, from + i) * std::sin(angle);
            }
            return std::hypot(re, im) / (double) count;
        };
        double upper = 0.0;
        for (int harmonic = 5; harmonic <= 9; ++harmonic)
            upper += bin(220.0 * harmonic);
        const double fundamental = bin(220.0);
        return fundamental > 0.0 ? upper / fundamental : 0.0;
    };

    const double single = density(driven(1));
    REQUIRE(single > 0.0);
    REQUIRE(density(driven(3)) > single * 1.1);
}

TEST_CASE("A compressor squashes the peaks and a tremolo dips the level", "[render]")
{
    const auto arp   = arpeggio();
    const auto clean = render({ 1.0, kBpm, &arp });

    const auto compressed = render({ 1.0, kBpm, &arp, withNode<CompressorNode>([](auto& effect)
    {
        effect.setThresholdDb(-40.0f); // well under the arp, so it bites
        effect.setRatio(12.0f);
        effect.setAttackMs(1.0f);
        effect.setReleaseMs(80.0f);
    }) });

    // Lower peaks but still audible: turning everything down would pass a
    // peak test alone.
    const float cleanPeak = clean.getMagnitude(0, 0, clean.getNumSamples());
    REQUIRE(cleanPeak > 1.0e-3f);
    REQUIRE(compressed.getMagnitude(0, 0, compressed.getNumSamples()) < cleanPeak * 0.9f);
    REQUIRE(rms(compressed) > 1.0e-5f);

    const auto tremolo = render({ 1.0, kBpm, &arp, withNode<TremoloNode>([](auto& effect)
    {
        effect.setRateHz(6.0f);
        effect.setDepth(1.0f);
    }) });

    // Window by window, only where the clean part is loud: the arp has gaps
    // of its own, which no dip could be measured against.
    const int window      = (int) (kSampleRate * 0.02);
    float     deepestDip  = 1.0f;
    int       loudWindows = 0;
    for (int start = 0; start + window <= clean.getNumSamples(); start += window)
    {
        const float level = clean.getMagnitude(0, start, window);
        if (level < cleanPeak * 0.5f)
            continue;
        ++loudWindows;
        deepestDip = std::min(deepestDip, tremolo.getMagnitude(0, start, window) / level);
    }
    REQUIRE(loudWindows > 4);
    REQUIRE(deepestDip < 0.3f); // somewhere in a second at 6 Hz, most of the way to silence
}

TEST_CASE("A gate closes on the quiet between notes", "[render]")
{
    // On the gate itself, not a whole track: the claim is about the DSP node.
    const int burst = (int) (kSampleRate * 0.1), total = burst + (int) kSampleRate;

    juce::Random             random(1234);
    juce::AudioBuffer<float> open(2, total);
    for (int n = 0; n < total; ++n)
    {
        const float sample = n < burst ? std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * (float) n / (float) kSampleRate)
                                       : (random.nextFloat() * 2.0f - 1.0f) * 0.02f; // hiss, under the threshold
        open.setSample(0, n, sample);
        open.setSample(1, n, sample);
    }
    juce::AudioBuffer<float> gated(open);

    GateEffect gate;
    gate.prepare(kSampleRate, kBlock);
    gate.setEnabled(true);
    gate.setThresholdDb(-30.0f);
    gate.setRangeDb(60.0f);
    gate.setAttackMs(0.5f);
    gate.setHoldMs(15.0f);
    gate.setReleaseMs(60.0f);
    for (int pos = 0; pos < total; pos += kBlock)
    {
        juce::AudioBuffer<float> block(gated.getArrayOfWritePointers(), 2, pos, std::min(kBlock, total - pos));
        gate.process(block);
    }

    // Over the settled back half of the tail: hold and release take time.
    const int   settled = burst + (int) (kSampleRate * 0.5);
    const float openTail = open.getRMSLevel(0, settled, total - settled);
    REQUIRE(openTail > 1.0e-4f);
    REQUIRE(gated.getRMSLevel(0, settled, total - settled) < openTail * 0.1f);
}

TEST_CASE("A chorus changes the sound, and its depth sweeps it", "[render]")
{
    const auto arp    = arpeggio();
    const auto chorus = [&](float depth)
    {
        return render({ 1.0, kBpm, &arp, withNode<ChorusNode>([depth](auto& effect)
        {
            effect.setRateHz(3.0f);
            effect.setMix(1.0f);
            effect.setDepth(depth);
        }) });
    };
    const auto clean = render({ 1.0, kBpm, &arp });
    const auto swept = chorus(1.0f);

    REQUIRE(rms(clean) > 1.0e-4f);
    REQUIRE(leftDifference(clean, swept) > 1.0e-3f);
    REQUIRE(leftDifference(swept, chorus(0.0f)) > 1.0e-3f); // depth zero is a fixed comb
}

TEST_CASE("A wobble changes the sound, sweeps with depth, and follows the tempo", "[render]")
{
    const auto arp    = arpeggio();
    const auto wobble = [](float depth)
    {
        return withNode<WobbleNode>([depth](auto& effect)
        {
            effect.setRateInBeats(1.0f);
            effect.setMix(1.0f);
            effect.setDepth(depth);
        });
    };
    const auto clean = render({ 1.0, kBpm, &arp });
    const auto swept = render({ 1.0, kBpm, &arp, wobble(1.0f) });

    REQUIRE(rms(clean) > 1.0e-4f);
    REQUIRE(leftDifference(clean, swept) > 1.0e-3f);
    REQUIRE(leftDifference(swept, render({ 1.0, kBpm, &arp, wobble(0.0f) })) > 1.0e-3f);

    // Only the tempo differs, so only EffectChain::setBpm being fed the
    // transport's tempo can make these differ. Through an audio clip at beat
    // 0, which plays by samples, so the tempo doesn't also move the notes.
    const auto toneAt = [&](double bpm)
    {
        return render({ 1.0, bpm, nullptr, [wobble](MixerTrack& track)
        {
            track.audioPlayer.submitSingleClip(new ClipData(tone(1.0, 100.0, 1.0f)), 0.0);
            wobble(1.0f)(track);
        } });
    };
    REQUIRE(leftDifference(toneAt(kBpm), toneAt(kBpm * 2.0)) > 1.0e-3f);
}

TEST_CASE("The mastering rack is clean bypassed and every preset holds its ceiling", "[render]")
{
    // A tone with full-scale spikes, different in each channel: what tests a
    // limiter, with something for the widener to act on.
    const auto source = []
    {
        juce::AudioBuffer<float> buffer(2, (int) kSampleRate);
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float tone  = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * (float) n / (float) kSampleRate);
            const float spike = (n % 4000 < 40) ? 0.95f : 0.0f;
            buffer.setSample(0, n, tone + spike);
            buffer.setSample(1, n, tone - spike * 0.7f);
        }
        return buffer;
    };
    const auto mastered = [&](const soundsplice::model::MasteringSettings& settings)
    {
        auto buffer = source();
        MasteringProcessor rack;
        rack.prepare(kSampleRate, kBlock);
        rack.setEnabled(settings.enabled);
        rack.setLowShelf(settings.lowShelfHz, settings.lowShelfDb);
        rack.setPeak(settings.peakHz, settings.peakDb, settings.peakQ);
        rack.setHighShelf(settings.highShelfHz, settings.highShelfDb);
        rack.setExciter(settings.exciterAmount, settings.exciterCrossoverHz);
        rack.setWidth(settings.width);
        rack.setReverb(settings.reverbAmount, settings.reverbRoomSize);
        rack.setMaximizer(settings.maximizerInputDb, settings.maximizerCeilingDb, settings.maximizerReleaseMs);
        rack.setOutputGainDb(settings.outputGainDb);
        for (int pos = 0; pos < buffer.getNumSamples(); pos += kBlock)
        {
            juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, pos, std::min(kBlock, buffer.getNumSamples() - pos));
            rack.process(view);
        }
        return buffer;
    };

    const auto raw = source();
    REQUIRE(worstDifference(mastered({}), raw) < 1.0e-9f); // disabled is untouched

    // Everything upstream of the limiter adds level; its ceiling has to
    // survive all of it, scaled by the output gain after it.
    for (int i = 0; i < kNumMasteringPresets; ++i)
    {
        const auto preset   = (MasteringPreset) i;
        const auto settings = soundsplice::model::presetForMastering(preset);
        if (! settings.enabled)
            continue;

        INFO(masteringPresetName(preset));
        const auto  out     = mastered(settings);
        const float allowed = std::pow(10.0f, settings.maximizerCeilingDb / 20.0f)
                            * std::pow(10.0f, settings.outputGainDb / 20.0f) + 1.0e-3f;
        REQUIRE(out.getMagnitude(0, 0, out.getNumSamples()) <= allowed);
        REQUIRE(worstDifference(out, raw) >= 1.0e-4f); // and it does something
    }
}

TEST_CASE("The metronome clicks on the beat and is silent off it, or when off", "[render]")
{
    const auto clicks = [](bool enabled)
    {
        Metronome metronome;
        metronome.prepare(kSampleRate);
        metronome.setEnabled(enabled);
        metronome.setLevel(1.0f);

        juce::AudioBuffer<float> out(2, (int) (kSampleRate * 2.0));
        out.clear();
        for (int pos = 0; pos < out.getNumSamples(); pos += kBlock)
        {
            const int      n = std::min(kBlock, out.getNumSamples() - pos);
            ProcessContext context;
            context.sampleRate        = kSampleRate;
            context.numSamples        = n;
            context.transport.playing = true;
            OfflineRenderer::fillTransport(context, pos, n, kBpm, kSampleRate);
            context.transport.timeSigNumerator   = 4;
            context.transport.timeSigDenominator = 4;

            juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, pos, n);
            metronome.process(view, context, false);
        }
        return out;
    };

    // A beat every half second at 120 bpm.
    const auto on = clicks(true);
    REQUIRE(rmsBetween(on, 0.0, 0.02) > 0.01f);
    REQUIRE(rmsBetween(on, 0.5, 0.02) > 0.01f);
    REQUIRE(rmsBetween(on, 0.25, 0.02) < 1.0e-6f);

    // Off is silence, which is what keeps it out of what's heard live.
    REQUIRE(rms(clicks(false)) < 1.0e-9f);
}

// ---- MIDI -------------------------------------------------------------------------------

TEST_CASE("The audio recorder captures what it's given, to disk, and loses nothing", "[render][record]")
{
    // Fed directly, with no device: this covers capture, the hand-off and the
    // writer thread, by reading the file back, not live input reaching it.
    std::vector<float> ramp((size_t) kBlock);
    for (int i = 0; i < kBlock; ++i)
        ramp[(size_t) i] = (float) i / (float) kBlock;
    const float* channels[1] = { ramp.data() };

    juce::TimeSliceThread writer("RecordWriter");
    writer.startThread(juce::Thread::Priority::normal);
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto readBack = [&formats](const juce::File& file)
    {
        std::vector<float>                       out;
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> buffer(1, (int) reader->lengthInSamples);
            if (reader->read(&buffer, 0, (int) reader->lengthInSamples, 0, true, false))
                out.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples());
        }
        return out;
    };

    SECTION("A take, from where it started")
    {
        AudioRecorder recorder;
        recorder.prepare(kSampleRate, 1);

        recorder.process(channels, 1, kBlock, true, 0);
        REQUIRE(recorder.recordedSampleCount() == 0); // not armed: nothing

        REQUIRE(recorder.arm(temp.getNonexistentChildFile("soundsplice-take", ".wav"), writer));
        constexpr int64_t kStart = 88200; // not zero: the take has to remember it
        for (int block = 0; block < 3; ++block)
            recorder.process(channels, 1, kBlock, true, kStart + block * kBlock);
        REQUIRE(recorder.recordedSampleCount() == kBlock * 3);
        REQUIRE_FALSE(recorder.isFinished());
        REQUIRE(recorder.startPlayheadSamples() == kStart);

        // The next block after disarming is what finishes it.
        recorder.disarm();
        recorder.process(channels, 1, kBlock, true, kStart);
        REQUIRE(recorder.isFinished());
        REQUIRE(recorder.droppedSampleCount() == 0);

        const auto file     = recorder.finishTake();
        const auto captured = readBack(file);
        file.deleteFile();
        REQUIRE(captured.size() == (size_t) kBlock * 3);
        for (size_t i = 0; i < captured.size(); ++i)
            REQUIRE(std::abs(captured[i] - ramp[i % (size_t) kBlock]) <= 1.0e-6f); // one 24-bit step
    }

    SECTION("A take longer than the old 180-second cap comes back complete")
    {
        // 200 s at 8 kHz: past the cap, which was in seconds, without
        // writing 8.8M samples.
        constexpr double kRate = 8000.0;
        AudioRecorder    recorder;
        recorder.prepare(kRate, 1);
        recorder.arm(temp.getNonexistentChildFile("soundsplice-long-take", ".wav"), writer);

        // Paced: flat out, this loop outruns the writer thread and the
        // recorder rightly reports an overrun, which would be measuring the loop.
        const int blocks = (int) (200.0 * kRate) / kBlock;
        for (int block = 0; block < blocks; ++block)
        {
            recorder.process(channels, 1, kBlock, true, block * kBlock);
            if (block % 8 == 7)
                juce::Thread::sleep(1);
        }
        recorder.disarm();
        recorder.process(channels, 1, kBlock, true, 0);

        // Not "nothing dropped": bursts can outrun any disk. Every sample is
        // written or counted as lost, and capture went on past the cap.
        const int64_t recorded = recorder.recordedSampleCount();
        REQUIRE(recorded + recorder.droppedSampleCount() == (int64_t) blocks * kBlock);
        REQUIRE(recorded > (int64_t) (180.0 * kRate));

        const auto file = recorder.finishTake();
        REQUIRE((int64_t) readBack(file).size() == recorded); // and all of it reached the file
        file.deleteFile();
    }

    SECTION("A count-in isn't captured, and a take abandoned in it still finishes")
    {
        AudioRecorder recorder;
        recorder.prepare(kSampleRate, 1);
        recorder.arm(temp.getNonexistentChildFile("soundsplice-countin", ".wav"), writer, (int64_t) kBlock * 2);

        recorder.process(channels, 1, kBlock, true, 0);
        REQUIRE(recorder.recordedSampleCount() == 0);
        REQUIRE(recorder.leadInRemaining() > 0);

        recorder.process(channels, 1, kBlock, true, kBlock);     // the count-in ends
        recorder.process(channels, 1, kBlock, true, kBlock * 2); // and this is captured
        REQUIRE(recorder.recordedSampleCount() == kBlock);
        REQUIRE(recorder.startPlayheadSamples() == kBlock * 2); // where playing started, not arming
        recorder.disarm();
        recorder.process(channels, 1, kBlock, true, 0);
        recorder.finishTake().deleteFile();

        // Given up during a long count-in: it must still finish, or the app
        // waits forever for a take that never comes - and leave no empty file.
        AudioRecorder abandoned;
        abandoned.prepare(kSampleRate, 1);
        const auto file = temp.getNonexistentChildFile("soundsplice-abandoned", ".wav");
        abandoned.arm(file, writer, (int64_t) kBlock * 8);
        abandoned.process(channels, 1, kBlock, true, 0);
        abandoned.disarm();
        abandoned.process(channels, 1, kBlock, true, 0);
        REQUIRE(abandoned.isFinished());
        REQUIRE(abandoned.recordedSampleCount() == 0);
        REQUIRE(abandoned.finishTake() == juce::File {});
        REQUIRE_FALSE(file.existsAsFile());
    }

    writer.stopThread(2000);
}
