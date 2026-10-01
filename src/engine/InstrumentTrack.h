#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/AudioFilePlayerNode.h"
#include "engine/AutomationCurve.h"
#include "engine/EffectChain.h"
#include "engine/MixRouting.h"
#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SessionPlayer.h"
#include "engine/SynthInstrumentNode.h"

namespace soundsplice::engine
{
/** Sets every automated effect parameter in @p chain to its value at
    @p beat. Audio thread, once per block before the chain runs: the values
    step per block rather than ramping across it, which at a normal block
    size is finer than any knob is turned. A curve whose slot now holds
    another kind of effect is skipped. */
inline void applyEffectAutomation(EffectChain& chain, const TrackAutomation& automation, double beat) noexcept
{
    for (const auto& lane : automation.effects)
    {
        auto* node = chain.nodeAt((size_t) lane.slot);
        if (node != nullptr && node->kind() == lane.kind && ! lane.curve.empty())
            node->setParam(lane.paramId, lane.curve.valueAt(beat, 0.0f));
    }
}

/**
    One mixer channel: a synth driven by its own sequencer, *and* an audio-clip
    player, both summed into the same per-track gain, pan, mute, solo and
    post-gain peak metering. A track only uses whichever of these
    it's been given content for — an Instrument-type track gets a pattern for
    the synth, an Audio-type track gets a decoded clip via audioPlayer — but
    both nodes always exist on every pool slot, so there's no track-type
    branching in most of the engine.

    Tracks live in a fixed, pre-allocated pool inside the engine, so
    activating/deactivating a track is just an atomic flag — there is no real-time
    graph surgery. To apply per-track gain the synth renders into a scratch buffer
    which is then summed into the mix. The same render() is used by the live
    engine and the offline renderer, so the offline bounce genuinely exercises
    this path (gain included).

    Solo is resolved by the caller: it passes in whether *any* track in the pool
    is currently soloed (a single scan of atomics, done once per block), and this
    track goes silent if it's muted, or if some other track is soloed and this one
    isn't — the standard "solo overrides, mute always wins" behaviour.
*/
struct InstrumentTrack
{
    /** Where one block of this track goes (see renderRouted): its output -
        the master or a bus's input, or nowhere - and its sends, and whether
        mute and solo leave it audible, and how far to delay it. */
    struct Destinations
    {
        juce::AudioBuffer<float>*                                      output = nullptr;
        std::array<juce::AudioBuffer<float>*, mixrouting::kMaxSends>  sends {};
        std::array<float, mixrouting::kMaxSends>                       sendGains {};
        std::array<bool, mixrouting::kMaxSends>                        sendPreFader {};
        int                                                            sendCount = 0;
        bool                                                           audible   = true;
        int                                                            delay     = 0;
    };

    SynthInstrumentNode      synth;
    Sequencer                sequencer;
    SessionPlayer            session;
    AudioFilePlayerNode      audioPlayer;

    // This track's insert chain, applied to its own output before the fader.
    // Owned by the audio thread and replaced whole; see setEffectChain.
    EffectChain*                     effectChain_ = nullptr;
    rt::SpscRingBuffer<EffectChain*> effectChainInbox_   { 8 };
    rt::SpscRingBuffer<EffectChain*> effectChainReclaim_ { 16 };
    std::atomic<bool>        active      { false };
    std::atomic<bool>        muted       { false };
    std::atomic<bool>        solo        { false };

    std::atomic<float>       gainDb      { 0.0f };
    std::atomic<float>       pan         { 0.0f }; // -1 = hard left, 0 = centre, +1 = hard right

    // Routing (engine/MixRouting.h), set from the message thread: whether
    // this is a bus, which track its output feeds (-1: the master), and its
    // sends. A bus mixes what reaches busInput into its own chain and fader.
    std::atomic<bool>        isBus       { false };
    std::atomic<int>         outputBus   { -1 };
    std::array<std::atomic<int>, mixrouting::kMaxSends>   sendBus {};
    std::array<std::atomic<float>, mixrouting::kMaxSends> sendGain {};     // linear
    std::array<std::atomic<bool>, mixrouting::kMaxSends>  sendPreFader {};
    std::atomic<int>         sendCount   { 0 };
    juce::AudioBuffer<float> busInput;
    juce::MidiBuffer         trackMidi;
    juce::AudioBuffer<float> scratch;

    std::atomic<float>       channelPeak_[2] {};

    TrackAutomation*                     automation_ = nullptr; // audio-thread owned

    // Delay compensation (see compensate): a ring of the track's recent
    // output, and how far behind it the track is being played.
    juce::AudioBuffer<float> compensation_;
    int                      compensationWrite_ = 0;
    int                      compensationDelay_ = 0;
    bool                     compensationStale_ = false;
    rt::SpscRingBuffer<TrackAutomation*> automationInbox_   { 8 };  // message -> audio
    rt::SpscRingBuffer<TrackAutomation*> automationReclaim_ { 16 }; // audio -> message

    ~InstrumentTrack()
    {
        collectRetiredAutomation();
        delete automation_;

        TrackAutomation* straggler = nullptr;
        while (automationInbox_.pop(straggler))
            delete straggler;

        collectRetiredEffectChain();
        delete effectChain_;

        EffectChain* chainStraggler = nullptr;
        while (effectChainInbox_.pop(chainStraggler))
            delete chainStraggler;
    }

    /** Hands ownership of a rebuilt chain to the audio thread. Structural
        changes only — parameters are set on the live nodes' atomics. */
    void setEffectChain(EffectChain* chain)
    {
        if (! effectChainInbox_.push(chain))
            delete chain;
    }

    void collectRetiredEffectChain()
    {
        EffectChain* retired = nullptr;
        while (effectChainReclaim_.pop(retired))
            delete retired;
    }

    // ---- message thread ----
    /** Hands ownership of @p curves (this track's whole automation set) to the
        audio thread. nullptr clears automation. */
    void setAutomation(TrackAutomation* curves)
    {
        if (! automationInbox_.push(curves))
            delete curves;
    }

    /** Frees automation the audio thread has retired. Call periodically from
        the message thread (see AudioEngine::pump). */
    void collectRetiredAutomation()
    {
        TrackAutomation* retired = nullptr;
        while (automationReclaim_.pop(retired))
            delete retired;
    }

    void prepare(double sampleRate, int blockSize)
    {
        synth.prepare(sampleRate, blockSize);
        audioPlayer.prepare(sampleRate, blockSize);
        trackMidi.ensureSize(2048);
        scratch.setSize(2, juce::jmax(1, blockSize));
        busInput.setSize(2, juce::jmax(1, blockSize));
        busInput.clear();
        compensation_.setSize(2, kMaxCompensation);
        compensation_.clear();
        compensationWrite_ = 0;
        compensationDelay_ = 0;
    }

    /** The most delay compensation a track can apply, in samples: about 0.7 s
        at 48 kHz, far more than any sane plugin reports. A chain later than
        this is aligned as far as it can be. */
    static constexpr int kMaxCompensation = 1 << 15;

    /** How late this track's effects make it, in samples. Audio thread,
        before render(): the engine takes the latest of these and delays every
        other track to match. Picks up a newly submitted chain first, so the
        figure is for the chain that's about to play. */
    int chainLatency() noexcept
    {
        adoptIncomingChain();
        return effectChain_ != nullptr ? effectChain_->latencySamples() : 0;
    }

    /** Unity-centre linear pan law — see the note in render().
        Public for anything that has to undo a pan, as Make Stereo Track does. */
    static float panGainFor(int channel, float panPosition) noexcept
    {
        const float p = juce::jlimit(-1.0f, 1.0f, panPosition);
        if (channel == 0) return p <= 0.0f ? 1.0f : 1.0f - p;
        if (channel == 1) return p >= 0.0f ? 1.0f : 1.0f + p;
        return 1.0f;
    }

private:
    // Automation lookups. Each returns nullptr when that parameter isn't
    // automated, which is what makes the track fall back to its static value.
    const AutomationCurve* automationGain() const noexcept
    {
        return (automation_ != nullptr && ! automation_->gain.empty()) ? &automation_->gain : nullptr;
    }
    const AutomationCurve* automationPan() const noexcept
    {
        return (automation_ != nullptr && ! automation_->pan.empty()) ? &automation_->pan : nullptr;
    }

    static float decibelsAt(const AutomationCurve* curve, double beat, float staticDb)
    {
        return juce::Decibels::decibelsToGain(curve != nullptr ? curve->valueAt(beat, staticDb) : staticDb);
    }

    void adoptIncomingChain() noexcept
    {
        EffectChain* incomingChain = nullptr;
        while (effectChainInbox_.pop(incomingChain))
        {
            if (effectChain_ != nullptr)
                effectChainReclaim_.push(effectChain_);
            effectChain_ = incomingChain;
        }
    }

    /** Delays the first @p numSamples of scratch by @p delay samples, through
        a ring prepared in advance. Always written, so a delay that changes
        finds the track's recent audio already there; cleared once when that
        audio is stale (after the track was silent), rather than letting a
        moment from before the silence play. */
    void compensate(int numSamples, int delay) noexcept
    {
        const int size = compensation_.getNumSamples();
        if (size <= 0)
            return;

        delay = juce::jlimit(0, size - 1, delay);
        if (compensationStale_ || delay != compensationDelay_)
        {
            if (compensationStale_)
                compensation_.clear();
            compensationStale_ = false;
            compensationDelay_ = delay;
        }

        const int channels = juce::jmin(scratch.getNumChannels(), compensation_.getNumChannels());
        int       write    = compensationWrite_;
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* line    = compensation_.getWritePointer(ch);
            auto* samples = scratch.getWritePointer(ch);
            write         = compensationWrite_;
            for (int i = 0; i < numSamples; ++i)
            {
                line[write] = samples[i];
                if (delay > 0)
                {
                    int read = write - delay;
                    if (read < 0)
                        read += size;
                    samples[i] = line[read];
                }
                if (++write == size)
                    write = 0;
            }
        }
        compensationWrite_ = write;
    }

    static double beatsPerBlock(const ProcessContext& context) noexcept
    {
        if (context.sampleRate <= 0.0 || context.transport.bpm <= 0.0)
            return 0.0;
        return (double) context.numSamples * context.transport.bpm / (60.0 * context.sampleRate);
    }

public:
    /** Read by the UI thread for the mixer strip's meter. */
    float peak(int channel) const noexcept
    {
        return (channel >= 0 && channel < 2)
            ? channelPeak_[channel].load(std::memory_order_relaxed)
            : 0.0f;
    }

    /** Audio thread: render this track (post-gain) additively into @p mix,
        delayed so it lands as late as @p alignToLatency - the latest any
        track's effects make it - and so lines up with every other track.
        Unrouted: the offline renderer and the tests' way in. */
    void render(juce::AudioBuffer<float>& mix,
                const juce::MidiBuffer& liveMidi,
                const ProcessContext& context, bool receivesLiveMidi, bool anySoloActive,
                double launchQuantumSamples = 0.0, int alignToLatency = 0)
    {
        Destinations to;
        to.output  = &mix;
        to.audible = ! anySoloActive || solo.load(std::memory_order_relaxed);
        to.delay   = alignToLatency - chainLatency();
        renderRouted(liveMidi, context, receivesLiveMidi, launchQuantumSamples, to);
    }

    /** Audio thread: render this track into @p to's output and sends - the
        master, or the inputs of the buses it feeds. A bus starts from what
        has reached its busInput. */
    void renderRouted(const juce::MidiBuffer& liveMidi, const ProcessContext& context, bool receivesLiveMidi,
                      double launchQuantumSamples, const Destinations& to)
    {
        TrackAutomation* incoming = nullptr;
        while (automationInbox_.pop(incoming))
        {
            if (automation_ != nullptr)
                automationReclaim_.push(automation_); // rare drop-on-full leaks until dtor
            automation_ = incoming;
        }

        trackMidi.clear();

        // A launched session clip takes the track over completely: the
        // arrangement's clips are ignored while one is engaged, and its
        // sequencer is reset so nothing it left sounding hangs behind the
        // session clip. Stopping the session hands the track back. Summing
        // both would have no musical meaning.
        if (session.renderBlock(trackMidi, context, launchQuantumSamples))
            sequencer.reset(trackMidi);
        else
            sequencer.renderBlock(trackMidi, context);

        if (receivesLiveMidi)
            trackMidi.addEvents(liveMidi, 0, context.numSamples, 0);

        const bool audible = ! muted.load(std::memory_order_relaxed) && to.audible;

        if (! audible)
        {
            channelPeak_[0].store(0.0f, std::memory_order_relaxed);
            channelPeak_[1].store(0.0f, std::memory_order_relaxed);
            compensationStale_ = true; // what it holds is from before the silence
            return;
        }

        const int numSamples = context.numSamples;

        // No reallocation: scratch was prepared to the maximum block size.
        scratch.setSize(2, juce::jmax(1, numSamples), false, false, true);
        scratch.clear();
        synth.process(scratch, trackMidi, context);
        audioPlayer.process(scratch, trackMidi, context); // adds in; midi is ignored
        if (isBus.load(std::memory_order_relaxed))
            for (int ch = 0; ch < juce::jmin(scratch.getNumChannels(), busInput.getNumChannels()); ++ch)
                scratch.addFrom(ch, 0, busInput, ch, 0, juce::jmin(numSamples, busInput.getNumSamples()));

        // Inserts run on the summed track output, before gain — so lowering
        // the fader doesn't change the effect.
        adoptIncomingChain();

        if (effectChain_ != nullptr)
        {
            // Pushed every block rather than only on rebuild, like the rest
            // of this chain's live parameters — a tempo change mid-playback
            // reaches a wobble immediately instead of waiting for the next
            // structural rebuild.
            effectChain_->setBpm(context.transport.bpm);
            if (automation_ != nullptr)
                applyEffectAutomation(*effectChain_, *automation_, context.transport.ppqPosition);
            effectChain_->process(scratch);
        }

        compensate(numSamples, to.delay);

        // Pre-fader sends: what the effects made, at the send's level.
        for (int s = 0; s < to.sendCount; ++s)
            if (auto* target = to.sends[(size_t) s]; target != nullptr && to.sendPreFader[(size_t) s])
                for (int ch = 0; ch < juce::jmin(target->getNumChannels(), scratch.getNumChannels()); ++ch)
                    target->addFrom(ch, 0, scratch, ch, 0, numSamples, to.sendGains[(size_t) s]);

        const float staticGainDb = gainDb.load(std::memory_order_relaxed);
        const float staticPan    = pan.load(std::memory_order_relaxed);

        // Automation is evaluated at both ends of the block and ramped across
        // it, rather than held at one value per block. Within a straight
        // segment that ramp *is* the curve, so a fade is smooth to the sample;
        // only a breakpoint landing mid-block is approximated, and then by at
        // most one block. This is what the message thread can't do — it only
        // gets to set a value every ~33ms, which steps audibly on a fast move.
        const double beatAtStart = context.transport.ppqPosition;
        const double beatAtEnd   = beatAtStart + beatsPerBlock(context);

        const float gainStart = decibelsAt(automationGain(), beatAtStart, staticGainDb);
        const float gainEnd   = decibelsAt(automationGain(), beatAtEnd, staticGainDb);
        const float panStart  = automationPan() != nullptr ? automationPan()->valueAt(beatAtStart, staticPan) : staticPan;
        const float panEnd    = automationPan() != nullptr ? automationPan()->valueAt(beatAtEnd, staticPan) : staticPan;

        const int channels = scratch.getNumChannels();

        // Into @p target from @p ch, at the fader's ramp times @p scale.
        const auto addFaded = [&](juce::AudioBuffer<float>& target, int ch, float start, float end, float scale)
        {
            if (ch >= target.getNumChannels())
                return;
            if (start == end)
                target.addFrom(ch, 0, scratch, ch, 0, numSamples, start * scale);
            else
                target.addFromWithRamp(ch, 0, scratch.getReadPointer(ch), numSamples, start * scale, end * scale);
        };

        for (int ch = 0; ch < channels; ++ch)
        {
            // A linear pan law with a unity centre: at pan 0 both sides stay at 1.0, so a centred track sums
            // bit-identically to how it did before panning existed. An
            // equal-power law would drop every centred track to ~0.707.
            const float channelGainStart = gainStart * panGainFor(ch, panStart);
            const float channelGainEnd   = gainEnd   * panGainFor(ch, panEnd);

            if (to.output != nullptr)
                addFaded(*to.output, ch, channelGainStart, channelGainEnd, 1.0f);

            // Post-fader sends: the track as the fader and pan leave it.
            for (int s = 0; s < to.sendCount; ++s)
                if (auto* target = to.sends[(size_t) s]; target != nullptr && ! to.sendPreFader[(size_t) s])
                    addFaded(*target, ch, channelGainStart, channelGainEnd, to.sendGains[(size_t) s]);

            if (ch < 2)
            {
                // Metered against the loudest end of the ramp, so a peak can't
                // hide inside a fade.
                const float peakGain = std::max(std::abs(channelGainStart), std::abs(channelGainEnd));
                float       peak     = 0.0f;
                const float* data    = scratch.getReadPointer(ch);
                for (int i = 0; i < numSamples; ++i)
                    peak = std::max(peak, std::abs(data[i]) * peakGain);
                channelPeak_[ch].store(peak, std::memory_order_relaxed);
            }
        }
    }
};

} // namespace soundsplice::engine
