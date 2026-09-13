#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/AudioFilePlayerNode.h"
#include "engine/AutomationCurve.h"
#include "engine/DrumKitNode.h"
#include "engine/EffectChain.h"
#include "engine/GuitarNode.h"
#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SessionPlayer.h"
#include "engine/SynthInstrumentNode.h"

namespace looper::engine
{
/**
    One mixer channel: a synth, drum kit or guitar (see `instrument`) driven by its
    own sequencer, *and* an audio-clip player, both summed into the same
    per-track gain, mute, solo, pre-fader send, and post-gain peak metering.
    A track only uses whichever of these it's been given content for — an
    Instrument-type track gets a pattern for the synth, an Audio-type track
    gets a decoded clip via audioPlayer, a Drum-type track gets a pattern for
    the drum kit instead of the synth — but every node always exists on every
    pool slot, so there's no track-type branching in most of the engine.

    `instrument` is the one exception: unlike audioPlayer (which naturally
    stays silent with no clip submitted), every note-driven instrument produces
    *some* sound for any note it receives, so which one gets the track's notes
    has to be stated rather than left for content-gating to sort out.

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

    sendLevel sends a copy of the raw (pre-fader) synth output into the caller's
    shared send bus, independent of the track's own gainDb — so a track can be
    faded down in the main mix while still reaching the send bus at a fixed level
    (the usual "aux send" behaviour), or vice versa.
*/
struct InstrumentTrack
{
    SynthInstrumentNode      synth;
    DrumKitNode              drumKit;
    GuitarNode               guitar;
    Sequencer                sequencer;
    SessionPlayer            session;
    AudioFilePlayerNode      audioPlayer;

    // This track's insert chain, applied to its own output before the fader
    // (and therefore before the send too, so a send carries the processed
    // sound — the usual behaviour). Owned by the audio thread and replaced
    // whole; see setEffectChain.
    EffectChain*                     effectChain_ = nullptr;
    rt::SpscRingBuffer<EffectChain*> effectChainInbox_   { 8 };
    rt::SpscRingBuffer<EffectChain*> effectChainReclaim_ { 16 };
    std::atomic<bool>        active      { false };
    std::atomic<bool>        muted       { false };
    std::atomic<bool>        solo        { false };

    /** A group bus: its scratch is filled by other tracks before it renders,
        so it generates nothing of its own and must not clear what it was
        given. See AudioEngine::processBlock, which does the clearing at the
        top of the block instead. */
    std::atomic<bool>        isBus       { false };
    std::atomic<TrackInstrument> instrument { TrackInstrument::Synth }; // which node gets the notes
    std::atomic<float>       gainDb      { 0.0f };
    std::atomic<float>       pan         { 0.0f }; // -1 = hard left, 0 = centre, +1 = hard right
    std::atomic<float>       sendLevel   { 0.0f }; // 0..1, pre-fader
    juce::MidiBuffer         trackMidi;
    juce::AudioBuffer<float> scratch;

    /** Whether `scratch` holds this block's audio. Audio thread only. */
    bool hasBlockOutput_ = false;
    std::atomic<float>       channelPeak_[2] {};

    TrackAutomation*                     automation_ = nullptr; // audio-thread owned
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
        drumKit.prepare(sampleRate, blockSize);
        guitar.prepare(sampleRate, blockSize);
        audioPlayer.prepare(sampleRate, blockSize);
        trackMidi.ensureSize(2048);
        scratch.setSize(2, juce::jmax(1, blockSize));
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
    const AutomationCurve* automationSend() const noexcept
    {
        return (automation_ != nullptr && ! automation_->sendLevel.empty()) ? &automation_->sendLevel : nullptr;
    }

    static float decibelsAt(const AutomationCurve* curve, double beat, float staticDb)
    {
        return juce::Decibels::decibelsToGain(curve != nullptr ? curve->valueAt(beat, staticDb) : staticDb);
    }

    /** Unity-centre linear pan law — see the note in render(). */
    static float panGainFor(int channel, float panPosition) noexcept
    {
        const float p = juce::jlimit(-1.0f, 1.0f, panPosition);
        if (channel == 0) return p <= 0.0f ? 1.0f : 1.0f - p;
        if (channel == 1) return p >= 0.0f ? 1.0f : 1.0f + p;
        return 1.0f;
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

    /** Audio thread: render this track (post-gain) additively into @p mix, and
        its pre-fader send additively into @p sendBus. */
    /** @p sidechainInput is the detector signal for any compressor in this
        track's chain, or nullptr for "each compressor listens to its own
        input". Borrowed for this block only. */
    /** Readies a bus to receive this block: sizes and clears the buffer its
        members will sum into. Called by the engine before any member renders,
        because the bus itself renders *after* them and so cannot do it. */
    void prepareBusInput(int numSamples)
    {
        scratch.setSize(2, juce::jmax(1, numSamples), false, false, true);
        scratch.clear();
        hasBlockOutput_ = false;
    }

    /** The buffer a bus's members sum into — its scratch, before it renders.
        Only meaningful on a bus track, and only between prepareBusInput() and
        this track's own render(). */
    juce::AudioBuffer<float>& busInput() noexcept { return scratch; }

    /** This block's rendered audio (post-inserts, pre-fader), or nullptr if
        the track produced none — inactive, muted, or soloed out. Valid only
        until the next render(). */
    const juce::AudioBuffer<float>* blockOutput() const noexcept
    {
        return hasBlockOutput_ ? &scratch : nullptr;
    }

    void render(juce::AudioBuffer<float>& mix, juce::AudioBuffer<float>& sendBus,
                const juce::MidiBuffer& liveMidi,
                const ProcessContext& context, bool receivesLiveMidi, bool anySoloActive,
                double launchQuantumSamples = 0.0,
                const juce::AudioBuffer<float>* sidechainInput = nullptr)
    {
        // Cleared up front so an early return below cannot leave last block's
        // audio readable as if it were this block's — a stale detector signal
        // would duck another track to a kick that isn't playing any more.
        const bool bus = isBus.load(std::memory_order_relaxed);
        if (! bus)
            hasBlockOutput_ = false;

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

        // A bus is not silenced by another track's solo: soloing a kick has to
        // keep playing *through* the drum bus, and a bus that vanished when
        // anyone hit solo would take its members' audio with it. Its own mute
        // still works, and muting a bus mutes the whole group — which is one
        // of the two reasons to have one.
        const bool audible = ! muted.load(std::memory_order_relaxed)
                           && (bus || ! anySoloActive || solo.load(std::memory_order_relaxed));

        if (! audible)
        {
            channelPeak_[0].store(0.0f, std::memory_order_relaxed);
            channelPeak_[1].store(0.0f, std::memory_order_relaxed);
            return;
        }

        const int numSamples = context.numSamples;

        if (! bus)
        {
            // No reallocation: scratch was prepared to the maximum block size.
            scratch.setSize(2, juce::jmax(1, numSamples), false, false, true);
            scratch.clear();
            switch (instrument.load(std::memory_order_relaxed))
            {
                case TrackInstrument::Drum:   drumKit.process(scratch, trackMidi, context); break;
                case TrackInstrument::Guitar: guitar.process(scratch, trackMidi, context);  break;
                case TrackInstrument::Synth:  synth.process(scratch, trackMidi, context);   break;
            }
            audioPlayer.process(scratch, trackMidi, context); // adds in; midi is ignored
        }
        // A bus's scratch already holds everything routed into it, summed
        // there by its members' own render() calls. Clearing it here would
        // throw away the entire group, which is precisely what it is for.

        // Inserts run on the summed track output, before gain and before the
        // send is taken — so lowering the fader doesn't change the effect, and
        // the send carries the processed sound.
        EffectChain* incomingChain = nullptr;
        while (effectChainInbox_.pop(incomingChain))
        {
            if (effectChain_ != nullptr)
                effectChainReclaim_.push(effectChain_);
            effectChain_ = incomingChain;
        }

        if (effectChain_ != nullptr)
        {
            // Pushed every block rather than only on rebuild, like the rest
            // of this chain's live parameters — a tempo change mid-playback
            // reaches a wobble immediately instead of waiting for the next
            // structural rebuild.
            effectChain_->setBpm(context.transport.bpm);
            effectChain_->setSidechainInput(sidechainInput);
            effectChain_->process(scratch);
        }

        // Readable by other tracks as a sidechain source from here on: the
        // track's own sound, after its inserts but before its fader — the same
        // point the send is taken from, and for the same reason. Pulling a
        // fader down should change how loud a track is, not how hard it ducks
        // something else.
        hasBlockOutput_ = true;

        const float staticGainDb = gainDb.load(std::memory_order_relaxed);
        const float staticPan    = pan.load(std::memory_order_relaxed);
        const float staticSend   = sendLevel.load(std::memory_order_relaxed);

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
        const float sendStart = automationSend() != nullptr ? automationSend()->valueAt(beatAtStart, staticSend) : staticSend;
        const float sendEnd   = automationSend() != nullptr ? automationSend()->valueAt(beatAtEnd, staticSend) : staticSend;

        const int channels = juce::jmin(mix.getNumChannels(), scratch.getNumChannels());

        for (int ch = 0; ch < channels; ++ch)
        {
            // A linear pan law with a unity centre, the same one the drum pads
            // use: at pan 0 both sides stay at 1.0, so a centred track sums
            // bit-identically to how it did before panning existed. An
            // equal-power law would drop every centred track to ~0.707.
            const float channelGainStart = gainStart * panGainFor(ch, panStart);
            const float channelGainEnd   = gainEnd   * panGainFor(ch, panEnd);

            if (channelGainStart == channelGainEnd)
                mix.addFrom(ch, 0, scratch, ch, 0, numSamples, channelGainStart);
            else
                mix.addFromWithRamp(ch, 0, scratch.getReadPointer(ch), numSamples,
                                    channelGainStart, channelGainEnd);

            // The send stays pre-fader *and* pre-pan: it's a mono-ish aux
            // feed, and panning it would move the track's reverb around the
            // stereo field independently of the track, which isn't wanted.
            if (ch < sendBus.getNumChannels() && (sendStart > 0.0f || sendEnd > 0.0f))
            {
                if (sendStart == sendEnd)
                    sendBus.addFrom(ch, 0, scratch, ch, 0, numSamples, sendStart);
                else
                    sendBus.addFromWithRamp(ch, 0, scratch.getReadPointer(ch), numSamples, sendStart, sendEnd);
            }

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

} // namespace looper::engine
