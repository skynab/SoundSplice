#pragma once

#include <array>
#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ToneDsp.h"

namespace soundsplice::engine
{
/** The Phaser as a chain node. The right channel's LFO runs a quarter cycle
    behind the left's, so the sweep moves across the stereo image. */
class PhaserEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            channels_[(size_t) ch].prepare(sampleRate);
            channels_[(size_t) ch].setPhase(ch * 0.25);
        }
    }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setRateHz(float hz)       { rateHz_.store(hz, std::memory_order_relaxed); }
    void setDepth(float depth)     { depth_.store(depth, std::memory_order_relaxed); }
    void setFeedback(float f)      { feedback_.store(f, std::memory_order_relaxed); }
    void setStages(int stages)     { stages_.store(stages, std::memory_order_relaxed); }
    void setMix(float mix)         { mix_.store(mix, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& phaser = channels_[(size_t) ch];
            phaser.setRateHz(rateHz_.load(std::memory_order_relaxed));
            phaser.setDepth(depth_.load(std::memory_order_relaxed));
            phaser.setFeedback(feedback_.load(std::memory_order_relaxed));
            phaser.setStages(stages_.load(std::memory_order_relaxed));
            phaser.setMix(mix_.load(std::memory_order_relaxed));

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = phaser.processSample(samples[n]);
        }
    }

private:
    std::array<Phaser, 2> channels_;
    std::atomic<bool>     enabled_ { false };
    std::atomic<float>    rateHz_ { 0.5f };
    std::atomic<float>    depth_ { 0.7f };
    std::atomic<float>    feedback_ { 0.5f };
    std::atomic<int>      stages_ { 6 };
    std::atomic<float>    mix_ { 0.5f };
};

/** The Flanger as a chain node, with the same quarter-cycle stereo offset. */
class FlangerEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            channels_[(size_t) ch].prepare(sampleRate);
            channels_[(size_t) ch].setPhase(ch * 0.25);
        }
    }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setRateHz(float hz)       { rateHz_.store(hz, std::memory_order_relaxed); }
    void setDepth(float depth)     { depth_.store(depth, std::memory_order_relaxed); }
    void setDelayMs(float ms)      { delayMs_.store(ms, std::memory_order_relaxed); }
    void setFeedback(float f)      { feedback_.store(f, std::memory_order_relaxed); }
    void setMix(float mix)         { mix_.store(mix, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& flanger = channels_[(size_t) ch];
            flanger.setRateHz(rateHz_.load(std::memory_order_relaxed));
            flanger.setDepth(depth_.load(std::memory_order_relaxed));
            flanger.setDelayMs(delayMs_.load(std::memory_order_relaxed));
            flanger.setFeedback(feedback_.load(std::memory_order_relaxed));
            flanger.setMix(mix_.load(std::memory_order_relaxed));

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = flanger.processSample(samples[n]);
        }
    }

private:
    std::array<Flanger, 2> channels_;
    std::atomic<bool>      enabled_ { false };
    std::atomic<float>     rateHz_ { 0.25f };
    std::atomic<float>     depth_ { 0.7f };
    std::atomic<float>     delayMs_ { 1.0f };
    std::atomic<float>     feedback_ { 0.5f };
    std::atomic<float>     mix_ { 0.5f };
};

class BassTrebleEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (auto& channel : channels_)
            channel.prepare(sampleRate);
    }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setBassDb(float db)       { bassDb_.store(db, std::memory_order_relaxed); }
    void setTrebleDb(float db)     { trebleDb_.store(db, std::memory_order_relaxed); }
    void setVolumeDb(float db)     { volumeDb_.store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& tone = channels_[(size_t) ch];
            tone.setBassDb(bassDb_.load(std::memory_order_relaxed));
            tone.setTrebleDb(trebleDb_.load(std::memory_order_relaxed));
            tone.setVolumeDb(volumeDb_.load(std::memory_order_relaxed));

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = tone.processSample(samples[n]);
        }
    }

private:
    std::array<BassTreble, 2> channels_;
    std::atomic<bool>         enabled_ { false };
    std::atomic<float>        bassDb_ { 0.0f };
    std::atomic<float>        trebleDb_ { 0.0f };
    std::atomic<float>        volumeDb_ { 0.0f };
};

/** Stereo Tools as a chain node. A mono track has nothing to mix, and passes
    through. */
class StereoToolEffect
{
public:
    void prepare(double, int) {}

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setWidth(float width)     { width_.store(width, std::memory_order_relaxed); }
    void setBalance(float balance) { balance_.store(balance, std::memory_order_relaxed); }
    void setMono(bool mono)        { mono_.store(mono, std::memory_order_relaxed); }
    void setSwap(bool swap)        { swap_.store(swap, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed) || buffer.getNumChannels() < 2)
            return;

        StereoTool tool;
        tool.width   = width_.load(std::memory_order_relaxed);
        tool.balance = balance_.load(std::memory_order_relaxed);
        tool.mono    = mono_.load(std::memory_order_relaxed);
        tool.swap    = swap_.load(std::memory_order_relaxed);

        auto* left  = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int n = 0; n < buffer.getNumSamples(); ++n)
            tool.processFrame(left[n], right[n]);
    }

private:
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> width_ { 1.0f };
    std::atomic<float> balance_ { 0.0f };
    std::atomic<bool>  mono_ { false };
    std::atomic<bool>  swap_ { false };
};

} // namespace soundsplice::engine
