#pragma once

#include <cmath>

#include "engine/Node.h"

namespace looper::engine
{
/**
    A sine source used as the Phase 1 stand-in for real clips: it lets us hear the
    graph + transport working before file playback exists. It only sounds while it
    is armed *and* the transport is playing, so Play/Stop actually start and stop
    audio. Control fields are touched only on the audio thread (set from the
    drained command queue), so they need no synchronisation.
*/
class OscillatorNode final : public Node
{
public:
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sampleRate_ = sampleRate;
        phase_      = 0.0;
        gain_.reset(sampleRate, 0.02);
    }

    void setEnabled(bool e) noexcept    { enabled_ = e; }
    void setFrequency(float hz) noexcept { if (hz > 0.0f) frequency_ = hz; }
    void setGainDb(float db) noexcept    { gainDb_ = db; }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/, const ProcessContext& context) override
    {
        const bool sounding = enabled_ && context.transport.playing;
        gain_.setTargetValue(sounding ? juce::Decibels::decibelsToGain(gainDb_) : 0.0f);

        if (sampleRate_ <= 0.0)
            return;

        const int    numSamples  = buffer.getNumSamples();
        const int    numChannels = buffer.getNumChannels();
        const double increment   = juce::MathConstants<double>::twoPi * frequency_ / sampleRate_;

        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = std::sin((float) phase_) * gain_.getNextValue();

            phase_ += increment;
            if (phase_ >= juce::MathConstants<double>::twoPi)
                phase_ -= juce::MathConstants<double>::twoPi;

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer(ch)[i] += sample; // sources sum into the buffer
        }
    }

private:
    double sampleRate_ = 0.0;
    double phase_      = 0.0;
    bool   enabled_    = false;
    float  frequency_  = 440.0f;
    float  gainDb_     = -12.0f;
    juce::LinearSmoothedValue<float> gain_ { 0.0f };
};

} // namespace looper::engine
