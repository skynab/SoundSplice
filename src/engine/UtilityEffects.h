#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DcBlocker.h"
#include "engine/Maximizer.h"

namespace soundsplice::engine
{
/** Gain, as Audacity's Amplify. Ramped across a block when it changes, so
    moving the control doesn't click. */
class AmplifyEffect
{
public:
    void prepare(double, int) { current_ = target(); }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setGainDb(float db)      { gainDb_.store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float wanted = target();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.applyGainRamp(ch, 0, buffer.getNumSamples(), current_, wanted);
        current_ = wanted;
    }

private:
    float target() const { return juce::Decibels::decibelsToGain(gainDb_.load(std::memory_order_relaxed), -120.0f); }

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> gainDb_ { 0.0f };
    float              current_ = 1.0f;
};

/** Polarity inversion, of either channel or both. On a mono track, the left
    setting is the one that counts. */
class InvertEffect
{
public:
    void prepare(double, int) {}

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setLeft(bool invert)     { left_.store(invert, std::memory_order_relaxed); }
    void setRight(bool invert)    { right_.store(invert, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            if ((ch == 0 ? left_ : right_).load(std::memory_order_relaxed))
                buffer.applyGain(ch, 0, buffer.getNumSamples(), -1.0f);
    }

private:
    std::atomic<bool> enabled_ { false };
    std::atomic<bool> left_ { true };
    std::atomic<bool> right_ { true };
};

/** DC offset removal: a DcBlocker per channel. */
class DcOffsetEffect
{
public:
    void prepare(double sampleRate, int)
    {
        for (auto& blocker : blockers_)
            blocker.prepare(sampleRate);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setCutoffHz(float hz)    { cutoffHz_.store(hz, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float cutoff   = cutoffHz_.load(std::memory_order_relaxed);
        const int   channels = juce::jmin(buffer.getNumChannels(), (int) blockers_.size());
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& blocker = blockers_[(size_t) ch];
            blocker.setCutoffHz(cutoff);
            auto* samples = buffer.getWritePointer(ch);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = blocker.process(samples[n]);
        }
    }

private:
    std::array<DcBlocker, 2> blockers_;
    std::atomic<bool>        enabled_ { false };
    std::atomic<float>       cutoffHz_ { 5.0f };
};

/** A brickwall limiter: the mastering rack's Maximizer as a chain effect, so
    one track or one selection can be held under a ceiling. Its 1.5 ms
    lookahead delays what passes through by that much. */
class LimiterEffect
{
public:
    void prepare(double sampleRate, int) { limiter_.prepare(sampleRate, 2); }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setInputGainDb(float db)  { inputGainDb_.store(db, std::memory_order_relaxed); }
    void setCeilingDb(float db)    { ceilingDb_.store(db, std::memory_order_relaxed); }
    void setReleaseMs(float ms)    { releaseMs_.store(ms, std::memory_order_relaxed); }

    int latencySamples() const noexcept { return limiter_.latencySamples(); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        limiter_.setInputGainDb(inputGainDb_.load(std::memory_order_relaxed));
        limiter_.setCeilingDb(ceilingDb_.load(std::memory_order_relaxed));
        const float release = releaseMs_.load(std::memory_order_relaxed);
        if (release != lastReleaseMs_)
        {
            limiter_.setReleaseMs(release);
            lastReleaseMs_ = release;
        }

        const int channels = juce::jmin(buffer.getNumChannels(), 2);
        if (channels == 0)
            return;

        float frame[2] {};
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            for (int ch = 0; ch < channels; ++ch)
                frame[ch] = buffer.getSample(ch, n);
            limiter_.processFrame(frame, channels);
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample(ch, n, frame[ch]);
        }
    }

private:
    Maximizer          limiter_;
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> inputGainDb_ { 0.0f };
    std::atomic<float> ceilingDb_ { -1.0f };
    std::atomic<float> releaseMs_ { 100.0f };
    float              lastReleaseMs_ = -1.0f;
};

} // namespace soundsplice::engine
