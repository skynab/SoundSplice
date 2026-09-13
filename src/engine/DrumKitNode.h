#pragma once

#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ClipData.h"
#include "engine/Interpolation.h"
#include "engine/Node.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/** One pad's live assignment: a MIDI note number, the decoded sample that
    plays when it's triggered (nullptr = no sample assigned, silent), and that
    pad's own mix settings. Shared (not owned outright) so the same decoded
    file can back several pads, or the same pad across tracks, without
    re-decoding — see AudioEngine::decodeOrGetCached.

    The mix fields default to a no-op (unity gain, centred, no transposition,
    not muted), so a pad map built without touching them behaves exactly as it
    did before they existed. `muted` here is already the *effective* mute —
    the caller folds the kit's solo state into it (see
    AudioEngine::setTrackDrumKit), so the audio thread never has to scan the
    other pads to decide whether this one should sound. */
struct DrumPadAssignment
{
    int                       noteNumber = -1;
    std::shared_ptr<ClipData> clipData;

    float gain       = 1.0f; // linear
    float pan        = 0.0f; // -1 = hard left, 0 = centre, +1 = hard right
    float pitchRatio = 1.0f; // playback-speed multiplier (2^(semitones/12))
    bool  muted      = false;
};

/** A whole kit's current pad→sample mapping, swapped as one unit — same
    lock-free hand-off shape as Sequencer::ClipList and
    AudioFilePlayerNode::ClipList (a raw pointer to the whole list, submitted
    by the message thread, swapped in by the audio thread, retired for
    deletion only once the audio thread is done with it). */
using DrumPadMap = std::vector<DrumPadAssignment>;

class DrumKitNode;

/** One polyphonic one-shot voice: on startNote, looks up the triggered
    note's currently-assigned sample (via the owning DrumKitNode's pad map)
    and plays it once through to the end, ignoring note-off — a drum hit
    isn't a sustained voice the way a synth note is. The sample-rate
    correction and interpolated read are the same technique
    AudioFilePlayerNode already uses for clip playback. */
class DrumSampleVoice final : public juce::SynthesiserVoice
{
public:
    explicit DrumSampleVoice(DrumKitNode& owner) : owner_(owner) {}

    bool canPlaySound(juce::SynthesiserSound* sound) override;

    void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int /*pitchWheel*/) override
    {
        // The pad's mix settings are captured here, at trigger time, rather
        // than read per-sample: a one-shot that's already in flight keeps the
        // gain/pan/pitch it started with, even if the pad map is swapped
        // underneath it mid-hit.
        const auto* pad = padFor(midiNoteNumber);

        if (pad == nullptr || pad->muted)
        {
            currentClip_ = nullptr;
            clearCurrentNote();
            return;
        }

        currentClip_ = pad->clipData.get();
        position_    = 0.0;
        velocity_    = velocity;
        gain_        = pad->gain;
        pitchRatio_  = pad->pitchRatio;

        // A linear pan law with a unity centre — at pan 0 both sides stay at
        // 1.0, so a centred pad is bit-identical to the un-panned behaviour
        // this voice had before panning existed (an equal-power law would
        // drop the centre to ~0.707 and quietly change every existing kit).
        panLeft_  = pad->pan <= 0.0f ? 1.0f : 1.0f - pad->pan;
        panRight_ = pad->pan >= 0.0f ? 1.0f : 1.0f + pad->pan;
    }

    void stopNote(float /*velocity*/, bool allowTailOff) override
    {
        if (! allowTailOff)
        {
            // A hard stop (voice stealing, all-notes-off) — a sustained
            // synth would release its envelope; a one-shot just cuts.
            currentClip_ = nullptr;
            clearCurrentNote();
        }
        // Otherwise ignore the note-off entirely: the hit keeps playing to
        // its natural end regardless of how long the MIDI note was held.
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void renderNextBlock(juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (currentClip_ == nullptr || currentClip_->lengthSamples <= 0)
        {
            clearCurrentNote();
            return;
        }

        const double deviceRate = getSampleRate();
        // The pad's transposition is folded straight into the read speed —
        // resampling, so a pitched-down hit is also a longer one, the usual
        // one-shot sampler behaviour.
        const double ratio      = ((currentClip_->sourceSampleRate > 0.0 && deviceRate > 0.0)
                                      ? currentClip_->sourceSampleRate / deviceRate
                                      : 1.0) * (double) pitchRatio_;
        const int    length     = currentClip_->lengthSamples;
        const int    fileChans  = currentClip_->numChannels;
        const int    outChans   = output.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            if (position_ >= (double) length)
            {
                clearCurrentNote();
                break;
            }

            for (int ch = 0; ch < outChans; ++ch)
            {
                const int    srcCh  = juce::jmin(ch, fileChans - 1);
                const float* srcPtr = currentClip_->audio.getReadPointer(srcCh);
                const float  pan    = (ch == 0) ? panLeft_ : (ch == 1 ? panRight_ : 1.0f);
                output.addSample(ch, startSample + i,
                                 sampleLinear(srcPtr, length, position_) * velocity_ * gain_ * pan);
            }

            position_ += ratio;
        }
    }

private:
    const DrumPadAssignment* padFor(int noteNumber); // defined below, after DrumKitNode

    DrumKitNode&     owner_;
    const ClipData*  currentClip_ = nullptr;
    double           position_    = 0.0;
    float            velocity_    = 1.0f;
    float            gain_        = 1.0f;
    float            panLeft_     = 1.0f;
    float            panRight_    = 1.0f;
    float            pitchRatio_  = 1.0f;
};

/** The sound every DrumSampleVoice can play — any note, any channel; which
    sample (if any) actually plays is decided per-note by the pad map. */
struct DrumKitSound final : juce::SynthesiserSound
{
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

/** A polyphonic one-shot drum sampler: each MIDI note can have its own
    decoded sample (see setPadMap), triggered like any other instrument via
    the per-block MIDI buffer. Built the same way SynthInstrumentNode wraps
    juce::Synthesiser, reusing its polyphony and sample-accurate MIDI
    dispatch rather than a bespoke voice pool. */
class DrumKitNode final : public Node
{
public:
    DrumKitNode()
    {
        synth_.addSound(new DrumKitSound());
        for (int i = 0; i < kNumVoices; ++i)
            synth_.addVoice(new DrumSampleVoice(*this));
    }

    ~DrumKitNode() override
    {
        collectRetired();
        delete current_;

        DrumPadMap* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        synth_.setCurrentPlaybackSampleRate(sampleRate);
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const ProcessContext& /*context*/) override
    {
        DrumPadMap* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_); // rare drop-on-full leaks until dtor — acceptable here
            current_ = incoming;
        }

        synth_.renderNextBlock(buffer, midi, 0, buffer.getNumSamples());
    }

    // ---- message thread ----
    /** Hands ownership of @p map (a whole new pad→sample assignment) to the
        audio thread. */
    void setPadMap(DrumPadMap* map)
    {
        if (! inbox_.push(map))
            delete map;
    }

    /** Frees pad maps the audio thread has retired. Call periodically from the message thread. */
    void collectRetired()
    {
        DrumPadMap* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    // ---- audio thread ----
    /** Read-only lookup for DrumSampleVoice::startNote — hands back the whole
        pad (sample plus its mix settings) by pointer, never copying the
        shared_ptr, so no atomic refcount touch happens on the audio thread.
        The returned pointer stays valid for the block: the pad map is only
        ever swapped at the top of process(). */
    const DrumPadAssignment* padFor(int noteNumber) const noexcept
    {
        if (current_ == nullptr)
            return nullptr;
        for (const auto& pad : *current_)
            if (pad.noteNumber == noteNumber)
                return &pad;
        return nullptr;
    }

private:
    static constexpr int kNumVoices = 8; // simultaneous one-shot hits

    juce::Synthesiser synth_;
    DrumPadMap*       current_ = nullptr; // audio-thread owned

    rt::SpscRingBuffer<DrumPadMap*> inbox_   { 16 }; // message -> audio
    rt::SpscRingBuffer<DrumPadMap*> reclaim_ { 32 }; // audio -> message
};

inline bool DrumSampleVoice::canPlaySound(juce::SynthesiserSound* sound)
{
    return dynamic_cast<DrumKitSound*>(sound) != nullptr;
}

inline const DrumPadAssignment* DrumSampleVoice::padFor(int noteNumber)
{
    return owner_.padFor(noteNumber);
}

} // namespace looper::engine
