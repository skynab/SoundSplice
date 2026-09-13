#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ClipData.h"
#include "engine/Interpolation.h"
#include "rt/SpscRingBuffer.h"

namespace soundsplice::engine
{
/**
    Plays one buffer once, from its start, whatever the transport is doing:
    for hearing something that isn't part of the song, such as an effect
    previewed on a selection before it is applied.

    AudioFilePlayerNode can't do this. It is slaved to the transport, so it
    only sounds while the song is rolling, and then plays from wherever the
    playhead is, on top of the song. This player keeps its own position, and
    the device callback mixes it in outside processBlock, so like the
    metronome it can never end up in an exported file.

    Hand-off is the same lock-free pointer swap the other players use: the
    message thread submits a whole ClipData (or nullptr, to stop), the audio
    thread swaps it in and returns the one it replaced through a second queue,
    and the message thread deletes those in collectRetired(). The audio thread
    never allocates or frees.
*/
class AuditionPlayer
{
public:
    ~AuditionPlayer()
    {
        collectRetired();
        delete current_;

        ClipData* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    /** Message thread, while the device is stopped. */
    void prepare(double sampleRate) { deviceSampleRate_ = sampleRate; }

    // ---- message thread ----

    /** Plays @p clip from its beginning, replacing anything already playing. */
    void play(std::unique_ptr<ClipData> clip)
    {
        auto* raw = clip.release();
        if (inbox_.push(raw))
            playing_.store(raw != nullptr, std::memory_order_relaxed);
        else
            delete raw;
    }

    void stop()
    {
        if (inbox_.push(nullptr))
            playing_.store(false, std::memory_order_relaxed);
    }

    /** True from play() until the buffer has played out or stop() is called. */
    bool isPlaying() const noexcept { return playing_.load(std::memory_order_relaxed); }

    /** Frees buffers the audio thread has finished with. */
    void collectRetired()
    {
        ClipData* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    // ---- audio thread ----

    /** Adds the next @p numSamples of the audition into @p output. */
    void process(juce::AudioBuffer<float>& output, int numSamples) noexcept
    {
        ClipData* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_); // a drop on a full queue leaks until the destructor, as elsewhere

            current_  = incoming;
            position_ = 0.0;
        }

        if (current_ == nullptr || deviceSampleRate_ <= 0.0 || current_->lengthSamples <= 0)
            return;

        const int length = current_->lengthSamples;
        if (position_ >= (double) length)
            return;

        // Resampled on the way out, so a 44.1k selection previews at the
        // right pitch on a 48k device.
        const double ratio     = current_->sourceSampleRate > 0.0 ? current_->sourceSampleRate / deviceSampleRate_
                                                                 : 1.0;
        const int    fileChans = juce::jmax(1, current_->numChannels);
        const int    outChans  = output.getNumChannels();
        const int    count     = juce::jmin(numSamples, output.getNumSamples());

        for (int i = 0; i < count && position_ < (double) length; ++i)
        {
            for (int ch = 0; ch < outChans; ++ch)
            {
                const float* source = current_->audio.getReadPointer(juce::jmin(ch, fileChans - 1));
                output.getWritePointer(ch)[i] += sampleLinear(source, length, position_);
            }

            position_ += ratio;
        }

        if (position_ >= (double) length)
            playing_.store(false, std::memory_order_relaxed);
    }

private:
    double            deviceSampleRate_ = 0.0;
    ClipData*         current_          = nullptr; // audio-thread owned
    double            position_         = 0.0;     // in source samples
    std::atomic<bool> playing_ { false };

    rt::SpscRingBuffer<ClipData*> inbox_   { 8 };  // message -> audio
    rt::SpscRingBuffer<ClipData*> reclaim_ { 16 }; // audio -> message
};

} // namespace soundsplice::engine
