#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Vocoder.h"

namespace soundsplice::engine
{
/** The vocoder as a chain node: the left channel speaks through the carrier,
    and the result, mixed with the dry signal, goes to both channels. */
class VocoderEffect
{
public:
    void prepare(double sampleRate, int) { vocoder_.prepare(sampleRate); }

    void setEnabled(bool enabled)   { enabled_.store(enabled, std::memory_order_relaxed); }
    void setCarrier(int carrier)    { carrier_.store(juce::jlimit(0, 2, carrier), std::memory_order_relaxed); }
    void setPitchHz(float hz)       { pitchHz_.store(hz, std::memory_order_relaxed); }
    void setBands(int bands)        { bands_.store(bands, std::memory_order_relaxed); }
    void setResponseMs(float ms)    { responseMs_.store(ms, std::memory_order_relaxed); }
    void setMix(float mix)          { mix_.store(juce::jlimit(0.0f, 1.0f, mix), std::memory_order_relaxed); }
    void setGainDb(float db)        { gainDb_.store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed) || buffer.getNumChannels() == 0)
            return;

        vocoder_.setCarrier((Vocoder::Carrier) carrier_.load(std::memory_order_relaxed));
        vocoder_.setPitchHz(pitchHz_.load(std::memory_order_relaxed));
        vocoder_.setBands(bands_.load(std::memory_order_relaxed));
        vocoder_.setResponseMs(responseMs_.load(std::memory_order_relaxed));

        const float mix      = mix_.load(std::memory_order_relaxed);
        const float wetGain  = mix * juce::Decibels::decibelsToGain(gainDb_.load(std::memory_order_relaxed));
        const int   channels = juce::jmin(buffer.getNumChannels(), 2);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float left  = buffer.getSample(0, n);
            const float right = channels > 1 ? buffer.getSample(1, n) : left;
            const float wet   = vocoder_.process(left, right);
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample(ch, n, buffer.getSample(ch, n) * (1.0f - mix) + wet * wetGain);
        }
    }

private:
    Vocoder vocoder_;

    std::atomic<bool>  enabled_ { false };
    std::atomic<int>   carrier_ { 1 };
    std::atomic<float> pitchHz_ { 110.0f };
    std::atomic<int>   bands_ { 16 };
    std::atomic<float> responseMs_ { 30.0f };
    std::atomic<float> mix_ { 1.0f };
    std::atomic<float> gainDb_ { 0.0f };
};

} // namespace soundsplice::engine
