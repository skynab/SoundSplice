#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ShelfPeakFilter.h"
#include "model/Effects.h"

namespace looper::engine
{
/**
    The master bus's 3-band EQ — bass (low shelf), mid (peaking), treble (high
    shelf) — the "regular mastering controls" a whole-song bus gets. Crossover
    frequencies are fixed (model::EqSettings::bassHz/trebleHz); only the three
    gains are adjustable, same shape as every other master effect here:
    atomics set from the message thread, coefficients refreshed once per
    block, passthrough when disabled.
*/
class EqEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/)
    {
        for (auto* band : { &bassLeft_, &bassRight_ })
        {
            band->setShape(ShelfPeakFilter::Shape::LowShelf);
            band->setFrequency(model::EqSettings::bassHz);
            band->prepare(sampleRate);
        }
        for (auto* band : { &midLeft_, &midRight_ })
        {
            band->setShape(ShelfPeakFilter::Shape::Peaking);
            band->setFrequency(model::EqSettings::midHz());
            band->setQ(0.7f);
            band->prepare(sampleRate);
        }
        for (auto* band : { &trebleLeft_, &trebleRight_ })
        {
            band->setShape(ShelfPeakFilter::Shape::HighShelf);
            band->setFrequency(model::EqSettings::trebleHz);
            band->prepare(sampleRate);
        }
    }

    void setEnabled(bool enabled)  { enabled_.store(enabled, std::memory_order_relaxed); }
    void setBassDb(float db)       { bassDb_.store(db, std::memory_order_relaxed); }
    void setMidDb(float db)        { midDb_.store(db, std::memory_order_relaxed); }
    void setTrebleDb(float db)     { trebleDb_.store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float bassDb   = bassDb_.load(std::memory_order_relaxed);
        const float midDb    = midDb_.load(std::memory_order_relaxed);
        const float trebleDb = trebleDb_.load(std::memory_order_relaxed);

        bassLeft_.setGainDb(bassDb);     bassRight_.setGainDb(bassDb);
        midLeft_.setGainDb(midDb);       midRight_.setGainDb(midDb);
        trebleLeft_.setGainDb(trebleDb); trebleRight_.setGainDb(trebleDb);

        const int numSamples  = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        for (int i = 0; i < numSamples; ++i)
        {
            if (numChannels > 0)
            {
                float s = buffer.getSample(0, i);
                s = bassLeft_.processSample(s);
                s = midLeft_.processSample(s);
                s = trebleLeft_.processSample(s);
                buffer.setSample(0, i, s);
            }
            if (numChannels > 1)
            {
                float s = buffer.getSample(1, i);
                s = bassRight_.processSample(s);
                s = midRight_.processSample(s);
                s = trebleRight_.processSample(s);
                buffer.setSample(1, i, s);
            }
        }
    }

private:
    ShelfPeakFilter bassLeft_, bassRight_;
    ShelfPeakFilter midLeft_, midRight_;
    ShelfPeakFilter trebleLeft_, trebleRight_;

    std::atomic<bool>  enabled_  { false };
    std::atomic<float> bassDb_   { 0.0f };
    std::atomic<float> midDb_    { 0.0f };
    std::atomic<float> trebleDb_ { 0.0f };
};

} // namespace looper::engine
