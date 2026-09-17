#pragma once

#include <array>
#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DynamicsDsp.h"

namespace soundsplice::engine
{
/** The Graphic EQ as a chain node. */
class GraphicEqEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (auto& channel : channels_)
            channel.prepare(sampleRate);
    }

    void setEnabled(bool enabled)      { enabled_.store(enabled, std::memory_order_relaxed); }
    void setBandDb(int band, float db) { if (band >= 0 && band < GraphicEq::kBands) gains_[(size_t) band].store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& eq = channels_[(size_t) ch];
            for (int b = 0; b < GraphicEq::kBands; ++b)
                eq.setGainDb(b, gains_[(size_t) b].load(std::memory_order_relaxed));

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = eq.processSample(samples[n]);
        }
    }

private:
    std::array<GraphicEq, 2>                            channels_;
    std::array<std::atomic<float>, GraphicEq::kBands>   gains_ {};
    std::atomic<bool>                                   enabled_ { false };
};

/** Runs a frame processor (DeEsser, Expander) over a buffer, a frame at a time. */
template <typename Processor>
void processFrames(Processor& processor, juce::AudioBuffer<float>& buffer)
{
    const int channels = juce::jmin(buffer.getNumChannels(), 2);
    float     frame[2] {};
    for (int n = 0; n < buffer.getNumSamples(); ++n)
    {
        for (int ch = 0; ch < channels; ++ch)
            frame[ch] = buffer.getSample(ch, n);
        processor.processFrame(frame, channels);
        for (int ch = 0; ch < channels; ++ch)
            buffer.setSample(ch, n, frame[ch]);
    }
}

class DeEsserEffect
{
public:
    void prepare(double sampleRate, int) { deEsser_.prepare(sampleRate); }

    void setEnabled(bool enabled)     { enabled_.store(enabled, std::memory_order_relaxed); }
    void setFrequencyHz(float hz)     { frequencyHz_.store(hz, std::memory_order_relaxed); }
    void setThresholdDb(float db)     { thresholdDb_.store(db, std::memory_order_relaxed); }
    void setMaxReductionDb(float db)  { maxReductionDb_.store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        deEsser_.setFrequencyHz(frequencyHz_.load(std::memory_order_relaxed));
        deEsser_.setThresholdDb(thresholdDb_.load(std::memory_order_relaxed));
        deEsser_.setMaxReductionDb(maxReductionDb_.load(std::memory_order_relaxed));
        processFrames(deEsser_, buffer);
    }

private:
    DeEsser            deEsser_;
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> frequencyHz_ { 5500.0f };
    std::atomic<float> thresholdDb_ { -30.0f };
    std::atomic<float> maxReductionDb_ { 12.0f };
};

class ExpanderEffect
{
public:
    void prepare(double sampleRate, int) { expander_.prepare(sampleRate); }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setThresholdDb(float db)  { thresholdDb_.store(db, std::memory_order_relaxed); }
    void setRatio(float ratio)     { ratio_.store(ratio, std::memory_order_relaxed); }
    void setRangeDb(float db)      { rangeDb_.store(db, std::memory_order_relaxed); }
    void setAttackMs(float ms)     { attackMs_.store(ms, std::memory_order_relaxed); }
    void setReleaseMs(float ms)    { releaseMs_.store(ms, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        expander_.setThresholdDb(thresholdDb_.load(std::memory_order_relaxed));
        expander_.setRatio(ratio_.load(std::memory_order_relaxed));
        expander_.setRangeDb(rangeDb_.load(std::memory_order_relaxed));
        expander_.setAttackMs(attackMs_.load(std::memory_order_relaxed));
        expander_.setReleaseMs(releaseMs_.load(std::memory_order_relaxed));
        processFrames(expander_, buffer);
    }

private:
    Expander           expander_;
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> thresholdDb_ { -40.0f };
    std::atomic<float> ratio_ { 2.0f };
    std::atomic<float> rangeDb_ { 40.0f };
    std::atomic<float> attackMs_ { 5.0f };
    std::atomic<float> releaseMs_ { 100.0f };
};

class RingModulatorEffect
{
public:
    void prepare(double sampleRate, int) { ring_.prepare(sampleRate); }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setFrequencyHz(float hz)  { frequencyHz_.store(hz, std::memory_order_relaxed); }
    void setMix(float mix)         { mix_.store(mix, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        ring_.setFrequencyHz(frequencyHz_.load(std::memory_order_relaxed));
        ring_.setMix(mix_.load(std::memory_order_relaxed));

        const int channels = buffer.getNumChannels();
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float carrier = ring_.nextCarrier();
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample(ch, n, ring_.apply(buffer.getSample(ch, n), carrier));
        }
    }

private:
    RingModulator      ring_;
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> frequencyHz_ { 440.0f };
    std::atomic<float> mix_ { 1.0f };
};

/** The Wah-wah as a chain node, the right channel's sweep a quarter cycle behind. */
class WahEffect
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
    void setResonance(float q)     { resonance_.store(q, std::memory_order_relaxed); }
    void setMix(float mix)         { mix_.store(mix, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& wah = channels_[(size_t) ch];
            wah.setRateHz(rateHz_.load(std::memory_order_relaxed));
            wah.setDepth(depth_.load(std::memory_order_relaxed));
            wah.setResonance(resonance_.load(std::memory_order_relaxed));
            wah.setMix(mix_.load(std::memory_order_relaxed));

            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = wah.processSample(samples[n]);
        }
    }

private:
    std::array<Wah, 2> channels_;
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> rateHz_ { 1.5f };
    std::atomic<float> depth_ { 0.8f };
    std::atomic<float> resonance_ { 4.0f };
    std::atomic<float> mix_ { 1.0f };
};

class EchoEffect
{
public:
    void prepare(double sampleRate, int) { echo_.prepare(sampleRate); }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setTimeMs(float ms)       { timeMs_.store(ms, std::memory_order_relaxed); }
    void setTaps(int taps)         { taps_.store(taps, std::memory_order_relaxed); }
    void setDecay(float decay)     { decay_.store(decay, std::memory_order_relaxed); }
    void setMix(float mix)         { mix_.store(mix, std::memory_order_relaxed); }
    void setPingPong(bool on)      { pingPong_.store(on, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        echo_.setTimeMs(timeMs_.load(std::memory_order_relaxed));
        echo_.setTaps(taps_.load(std::memory_order_relaxed));
        echo_.setDecay(decay_.load(std::memory_order_relaxed));
        echo_.setMix(mix_.load(std::memory_order_relaxed));
        echo_.setPingPong(pingPong_.load(std::memory_order_relaxed));
        processFrames(echo_, buffer);
    }

private:
    MultitapEcho       echo_;
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> timeMs_ { 250.0f };
    std::atomic<int>   taps_ { 3 };
    std::atomic<float> decay_ { 0.5f };
    std::atomic<float> mix_ { 0.35f };
    std::atomic<bool>  pingPong_ { false };
};

} // namespace soundsplice::engine
