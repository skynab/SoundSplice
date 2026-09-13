#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Exciter.h"
#include "engine/Maximizer.h"
#include "engine/ReverbEffect.h"
#include "engine/ShelfPeakFilter.h"
#include "engine/StereoWidener.h"

namespace looper::engine
{
/**
    The mastering rack: the master bus's last stage.

    Fixed order, and the order is the design:

        low shelf -> peak -> high shelf -> exciter -> widener -> reverb
                  -> maximizer -> output gain

    - **EQ first**, so everything after it works on the tone you actually
      want. Exciting or limiting first means shaping the result of decisions
      the EQ then changes underneath.
    - **Exciter after EQ**, so the harmonics it generates come from the
      corrected signal rather than from something you were about to cut.
    - **Widener after both**, because width is a decision about the finished
      tone, and because widening before a mono-ish EQ move would partly undo
      it.
    - **Reverb before the limiter**, so its tail is subject to the ceiling
      like everything else. After it, reverb could push the output past the
      ceiling the limiter just guaranteed.
    - **Maximizer last but one**, because a brickwall is only a brickwall if
      nothing adds level after it. Only a plain gain follows, and that's
      applied *below* unity in practice.

    Parameters are atomics set from the message thread and read once per
    block, the same arrangement every other effect here uses: a knob turn must
    not rebuild anything, since that would reset the reverb tail and the
    limiter's envelope.

    Every stage is a no-op at its default, so an untouched rack is
    bit-identical to no rack at all — which is what makes it safe to leave
    always-inserted on the master bus.
*/
class MasteringProcessor
{
public:
    void prepare(double sampleRate, int blockSize)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        for (auto& channel : filters_)
            for (auto& filter : channel)
                filter.prepare(sampleRate_);

        filters_[0][0].setShape(ShelfPeakFilter::Shape::LowShelf);
        filters_[1][0].setShape(ShelfPeakFilter::Shape::LowShelf);
        filters_[0][1].setShape(ShelfPeakFilter::Shape::Peaking);
        filters_[1][1].setShape(ShelfPeakFilter::Shape::Peaking);
        filters_[0][2].setShape(ShelfPeakFilter::Shape::HighShelf);
        filters_[1][2].setShape(ShelfPeakFilter::Shape::HighShelf);

        exciter_.prepare(sampleRate_);
        reverb_.prepare(sampleRate_, blockSize);
        reverb_.setEnabled(true); // gated by reverbAmount, like the send bus
        maximizer_.prepare(sampleRate_, 2);
    }

    void setEnabled(bool on)          { enabled_.store(on, std::memory_order_relaxed); }

    void setLowShelf(float hz, float db)  { lowHz_.store(hz, std::memory_order_relaxed);
                                            lowDb_.store(db, std::memory_order_relaxed); }
    void setPeak(float hz, float db, float q) { peakHz_.store(hz, std::memory_order_relaxed);
                                                peakDb_.store(db, std::memory_order_relaxed);
                                                peakQ_.store(q, std::memory_order_relaxed); }
    void setHighShelf(float hz, float db) { highHz_.store(hz, std::memory_order_relaxed);
                                            highDb_.store(db, std::memory_order_relaxed); }

    void setExciter(float amount, float crossoverHz)
    {
        exciterAmount_.store(amount, std::memory_order_relaxed);
        exciterHz_.store(crossoverHz, std::memory_order_relaxed);
    }

    void setWidth(float width)        { width_.store(width, std::memory_order_relaxed); }
    void setReverb(float amount, float roomSize)
    {
        reverbAmount_.store(amount, std::memory_order_relaxed);
        reverbRoom_.store(roomSize, std::memory_order_relaxed);
    }

    void setMaximizer(float inputDb, float ceilingDb, float releaseMs)
    {
        maxInputDb_.store(inputDb, std::memory_order_relaxed);
        maxCeilingDb_.store(ceilingDb, std::memory_order_relaxed);
        maxReleaseMs_.store(releaseMs, std::memory_order_relaxed);
    }

    void setOutputGainDb(float db)    { outputGainDb_.store(db, std::memory_order_relaxed); }

    /** Gain reduction the maximizer is currently applying, in dB. Lock-free
        readout for the meter. */
    float currentReductionDb() const noexcept
    {
        return reduction_.load(std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
        {
            reduction_.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const float lowHz  = lowHz_.load(std::memory_order_relaxed);
        const float lowDb  = lowDb_.load(std::memory_order_relaxed);
        const float peakHz = peakHz_.load(std::memory_order_relaxed);
        const float peakDb = peakDb_.load(std::memory_order_relaxed);
        const float peakQ  = peakQ_.load(std::memory_order_relaxed);
        const float highHz = highHz_.load(std::memory_order_relaxed);
        const float highDb = highDb_.load(std::memory_order_relaxed);

        for (auto& channel : filters_)
        {
            channel[0].setFrequency(lowHz);  channel[0].setGainDb(lowDb);
            channel[1].setFrequency(peakHz); channel[1].setGainDb(peakDb); channel[1].setQ(peakQ);
            channel[2].setFrequency(highHz); channel[2].setGainDb(highDb);
        }

        exciter_.setAmount(exciterAmount_.load(std::memory_order_relaxed));
        exciter_.setCrossoverHz(exciterHz_.load(std::memory_order_relaxed));

        widener_.setWidth(width_.load(std::memory_order_relaxed));

        maximizer_.setInputGainDb(maxInputDb_.load(std::memory_order_relaxed));
        maximizer_.setCeilingDb(maxCeilingDb_.load(std::memory_order_relaxed));
        maximizer_.setReleaseMs(maxReleaseMs_.load(std::memory_order_relaxed));

        const int numSamples  = buffer.getNumSamples();
        const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
        if (numChannels <= 0)
            return;

        // EQ and exciter are per-sample, per-channel.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* samples = buffer.getWritePointer(ch);
            auto& bank    = filters_[(size_t) ch];

            for (int n = 0; n < numSamples; ++n)
            {
                float x = samples[n];
                x = bank[0].processSample(x);
                x = bank[1].processSample(x);
                x = bank[2].processSample(x);
                samples[n] = exciter_.processSample(ch, x);
            }
        }

        // Width needs both channels at once, so it's its own pass. Mono
        // buffers have no sides to widen, hence the channel check.
        if (numChannels >= 2)
        {
            auto* left  = buffer.getWritePointer(0);
            auto* right = buffer.getWritePointer(1);
            for (int n = 0; n < numSamples; ++n)
                widener_.processFrame(left[n], right[n]);
        }

        // Reverb is a whole-buffer effect and already knows how to mix wet
        // against dry, so it's reused rather than reimplemented.
        const float reverbAmount = reverbAmount_.load(std::memory_order_relaxed);
        if (reverbAmount > 0.0f)
        {
            reverb_.setMix(reverbAmount);
            reverb_.setRoomSize(reverbRoom_.load(std::memory_order_relaxed));
            reverb_.process(buffer);
        }

        // Limiter last: nothing after it may add level, or the ceiling it
        // just guaranteed stops being a guarantee.
        std::array<float, 2> frame {};
        for (int n = 0; n < numSamples; ++n)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                frame[(size_t) ch] = buffer.getWritePointer(ch)[n];

            maximizer_.processFrame(frame.data(), numChannels);

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer(ch)[n] = frame[(size_t) ch];
        }

        reduction_.store(maximizer_.currentReductionDb(), std::memory_order_relaxed);

        const float outputGain = std::pow(10.0f, outputGainDb_.load(std::memory_order_relaxed) / 20.0f);
        if (outputGain != 1.0f)
            buffer.applyGain(0, numSamples, outputGain);
    }

private:
    static constexpr int kBands = 3;

    double sampleRate_ = 48000.0;

    std::array<std::array<ShelfPeakFilter, kBands>, 2> filters_;
    Exciter       exciter_;
    StereoWidener widener_;
    ReverbEffect  reverb_;
    Maximizer     maximizer_;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> lowHz_ { 120.0f }, lowDb_ { 0.0f };
    std::atomic<float> peakHz_ { 1000.0f }, peakDb_ { 0.0f }, peakQ_ { 0.9f };
    std::atomic<float> highHz_ { 8000.0f }, highDb_ { 0.0f };
    std::atomic<float> exciterAmount_ { 0.0f }, exciterHz_ { 3000.0f };
    std::atomic<float> width_ { 1.0f };
    std::atomic<float> reverbAmount_ { 0.0f }, reverbRoom_ { 0.6f };
    std::atomic<float> maxInputDb_ { 0.0f }, maxCeilingDb_ { -0.3f }, maxReleaseMs_ { 100.0f };
    std::atomic<float> outputGainDb_ { 0.0f };
    std::atomic<float> reduction_ { 0.0f };
};

} // namespace looper::engine
