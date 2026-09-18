#pragma once

#include <array>
#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DynamicsProcessor.h"

namespace soundsplice::engine
{
/** The dynamics processor as a chain node: the curve and settings come in
    from the message thread through atomics and are read once per block. */
class DynamicsProcessorEffect
{
public:
    void prepare(double sampleRate, int) { processor_.prepare(sampleRate); }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void setCurve(const TransferCurve& curve)
    {
        count_.store(curve.count, std::memory_order_relaxed);
        for (int i = 0; i < TransferCurve::kMaxPoints; ++i)
        {
            inDb_[(size_t) i].store(curve.points[(size_t) i].inDb, std::memory_order_relaxed);
            outDb_[(size_t) i].store(curve.points[(size_t) i].outDb, std::memory_order_relaxed);
        }
    }

    void setDetector(DynamicsProcessor::Detector detector) { detector_.store((int) detector, std::memory_order_relaxed); }
    void setMakeUpDb(float db)   { makeUpDb_.store(db, std::memory_order_relaxed); }
    void setAttackMs(float ms)   { attackMs_.store(ms, std::memory_order_relaxed); }
    void setReleaseMs(float ms)  { releaseMs_.store(ms, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        TransferCurve curve;
        curve.count = juce::jlimit(2, TransferCurve::kMaxPoints, count_.load(std::memory_order_relaxed));
        for (int i = 0; i < TransferCurve::kMaxPoints; ++i)
            curve.points[(size_t) i] = { inDb_[(size_t) i].load(std::memory_order_relaxed),
                                         outDb_[(size_t) i].load(std::memory_order_relaxed) };
        processor_.setCurve(curve);
        processor_.setDetector((DynamicsProcessor::Detector) juce::jlimit(0, 1, detector_.load(std::memory_order_relaxed)));
        processor_.setMakeUpDb(makeUpDb_.load(std::memory_order_relaxed));
        processor_.setAttackMs(attackMs_.load(std::memory_order_relaxed));
        processor_.setReleaseMs(releaseMs_.load(std::memory_order_relaxed));

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        float     frame[2] {};
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            for (int ch = 0; ch < channels; ++ch)
                frame[ch] = buffer.getSample(ch, n);
            processor_.processFrame(frame, channels);
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample(ch, n, frame[ch]);
        }
    }

private:
    DynamicsProcessor processor_;

    std::atomic<int>                                         count_ { 2 };
    std::array<std::atomic<float>, TransferCurve::kMaxPoints> inDb_ {};
    std::array<std::atomic<float>, TransferCurve::kMaxPoints> outDb_ {};
    std::atomic<int>   detector_ { 0 };
    std::atomic<float> makeUpDb_ { 0.0f };
    std::atomic<float> attackMs_ { 5.0f };
    std::atomic<float> releaseMs_ { 150.0f };
    std::atomic<bool>  enabled_ { false };
};

} // namespace soundsplice::engine
