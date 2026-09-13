#pragma once

#include <atomic>
#include <vector>

#include "engine/AudioClipSlot.h"
#include "engine/Interpolation.h"
#include "engine/Node.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Plays a track's audio clips, slaved to the transport: each clip plays only
    within its own [startBeats, startBeats + lengthBeats) window (silence
    outside every clip's window), the same clip-list scheduling Sequencer
    applies to MIDI clips. Unlike a MIDI pattern, a clip never loops within
    its window — once the file's samples run out it stays silent for the
    rest of that window. File/device sample-rate differences are corrected
    with linear interpolation.

    Clip-list hand-off is lock-free and allocation-free on the audio thread:
      - the message thread builds a whole new clip list and submits the
        pointer (see AudioEngine::setTrackAudioClips, which also caches
        decoded audio by file path so resubmitting doesn't mean re-decoding),
      - the audio thread swaps it in and returns the retired list through a
        second FIFO for the message thread to delete. The audio thread only
        ever reads a slot's ClipData through a raw pointer (never copies the
        shared_ptr), so it never touches the refcount — deletion, including
        of any ClipData a retired list was the last owner of, happens only on
        the message thread via collectRetiredClips().
*/
class AudioFilePlayerNode final : public Node
{
public:
    using ClipList = std::vector<AudioClipSlot>;

    ~AudioFilePlayerNode() override
    {
        collectRetiredClips();
        delete current_;

        ClipList* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override { deviceSampleRate_ = sampleRate; }

    // ---- message thread ----
    /** Hands ownership of @p clips (a whole new clip list for this track) to the audio thread. */
    void submitClips(ClipList* clips)
    {
        if (! inbox_.push(clips))
            delete clips;
    }

    /** Convenience for the common single-clip case (e.g. the global preview
        player, which never needs more than one clip): plays once from
        @p clipStartBeats with no other window gating. */
    void submitSingleClip(ClipData* clip, double clipStartBeats = 0.0)
    {
        auto* clips = new ClipList();
        clips->push_back({ std::shared_ptr<ClipData>(clip), clipStartBeats, 1.0e9 });
        submitClips(clips);
    }

    /** Frees clips the audio thread has retired. Call periodically from the message thread. */
    void collectRetiredClips()
    {
        ClipList* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    // ---- audio thread ----
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/, const ProcessContext& context) override
    {
        ClipList* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_); // rare drop-on-full leaks until dtor — acceptable here
            current_ = incoming;
        }

        if (current_ == nullptr || current_->empty() || ! context.transport.playing
            || deviceSampleRate_ <= 0.0 || context.transport.bpm <= 0.0)
            return;

        const int    numSamples      = context.numSamples;
        const double blockStartBeats = context.transport.ppqPosition;
        const double blockEndBeats   = context.transport.ppqAtBlockEnd;

        if (blockEndBeats <= blockStartBeats)
            return;

        // Samples per beat *for this block*, taken from the block's own musical
        // span. An audio clip is anchored at a musical start but its audio
        // advances in real time, so this is only used to place the start — the
        // read position below stays in samples, which is what stops a tempo
        // change from stretching the audio.
        const double samplesPerBeat = (double) numSamples / (blockEndBeats - blockStartBeats);

        // Find the clip whose window overlaps this block — same
        // block-granularity selection rule as Sequencer::renderBlock (clips
        // are expected not to overlap; the first match wins).
        int    foundIndex = -1;
        double localStart = 0.0;

        for (int i = 0; i < (int) current_->size(); ++i)
        {
            const auto&  slot  = (*current_)[(size_t) i];
            const double startedBeatsAgo = blockStartBeats - slot.startBeats;

            // Window tested in beats, where the clip is actually placed; the
            // offset into the file is then a real-time distance in samples.
            const double local = startedBeatsAgo * samplesPerBeat;

            if (blockEndBeats > slot.startBeats && startedBeatsAgo < slot.lengthBeats)
            {
                foundIndex = i;
                localStart = local;
                break;
            }
        }

        if (foundIndex < 0)
            return; // between clips, or before/after every clip's window

        const auto& activeSlot = current_->at((size_t) foundIndex);
        const auto* clip       = activeSlot.clipData.get();
        if (clip == nullptr)
            return;

        // Per-clip trim (model::Clip::gainDb), already linear — see
        // AudioClipSlot::gain.
        const float clipGain = activeSlot.gain;

        const double ratio      = clip->sourceSampleRate > 0.0
                                      ? clip->sourceSampleRate / deviceSampleRate_
                                      : 1.0;
        const int    length     = clip->lengthSamples;
        const int    fileChans  = clip->numChannels;
        const int    outChans   = buffer.getNumChannels();

        double position = localStart * ratio;

        for (int i = 0; i < numSamples; ++i)
        {
            if (position >= 0.0 && position < (double) length)
            {
                for (int ch = 0; ch < outChans; ++ch)
                {
                    const int    srcCh  = juce::jmin(ch, fileChans - 1);
                    const float* srcPtr = clip->audio.getReadPointer(srcCh);
                    buffer.getWritePointer(ch)[i] += clipGain * sampleLinear(srcPtr, length, position);
                }
            }

            position += ratio;
        }
    }

private:
    double    deviceSampleRate_ = 0.0;
    ClipList* current_          = nullptr; // audio-thread owned

    rt::SpscRingBuffer<ClipList*> inbox_   { 16 }; // message -> audio
    rt::SpscRingBuffer<ClipList*> reclaim_ { 32 }; // audio -> message
};

} // namespace looper::engine
