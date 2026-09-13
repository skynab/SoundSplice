#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/MetronomeMath.h"
#include "engine/ProcessContext.h"

namespace looper::engine
{
/**
    The click. A short decaying sine on every beat, pitched higher on the
    downbeat, generated rather than loaded from a sample so it adds no assets
    and no new dependency.

    Deliberately *not* a track and not part of the processing chain: the
    engine sums it in after the master bus, so it never passes through the
    master effects or gain and never reaches the meter — and, because
    OfflineRenderer has no metronome at all, it can't end up in a bounce. A
    click you can accidentally export is a bug waiting to happen.

    Parameters are atomics set from the message thread, read once per block,
    matching the convention the master effects already use.
*/
class Metronome
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_       = sampleRate;
        samplesRemaining_ = 0;
        phase_            = 0.0;
    }

    // ---- message thread ----
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const noexcept        { return enabled_.load(std::memory_order_relaxed); }
    void setLevel(float level) noexcept    { level_.store(level, std::memory_order_relaxed); }

    /** Adds the click into @p buffer for this block. @p force sounds it even
        when switched off, which is what a count-in needs — counting you in
        silently would be useless (see AudioEngine::beginRecording). */
    void process(juce::AudioBuffer<float>& buffer, const ProcessContext& context, bool force)
    {
        if (! enabled_.load(std::memory_order_relaxed) && ! force)
        {
            samplesRemaining_ = 0; // drop any click still ringing when switched off
            return;
        }

        if (sampleRate_ <= 0.0 || context.transport.bpm <= 0.0)
            return;

        const int numSamples = context.numSamples;

        // A stopped transport shouldn't tick, but a click already sounding is
        // allowed to ring out rather than being cut off mid-tick.
        if (context.transport.playing)
        {
            const double quartersPerBar = context.transport.timeSigNumerator * 4.0
                                        / juce::jmax(1, context.transport.timeSigDenominator);

            // Walked in beats rather than by stepping a fixed samples-per-beat.
            // Beats are no longer evenly spaced in samples once tempo can
            // change, so the click has to follow musical positions and convert
            // each one — otherwise it drifts away from the music it exists to
            // count.
            const double startBeats = context.transport.ppqPosition;
            const double endBeats   = context.transport.ppqAtBlockEnd;

            if (endBeats > startBeats)
            {
                for (int64_t beat = (int64_t) std::ceil(startBeats - 1.0e-9); ; ++beat)
                {
                    if ((double) beat >= endBeats)
                        break;

                    const int offset = (int) context.transport.sampleOffsetForPpq((double) beat, numSamples);
                    if (offset >= 0 && offset < numSamples)
                        trigger(MetronomeMath::isDownbeat(beat, quartersPerBar), offset);
                }
            }
        }

        renderInto(buffer, numSamples);
    }

private:
    static constexpr double kClickSeconds  = 0.035;
    static constexpr double kBeatHz        = 1000.0;
    static constexpr double kDownbeatHz    = 1600.0;

    /** Restarts the click voice. One voice is enough: two beats can't overlap
        within a 35ms click at any sane tempo, and if they somehow did, the
        newer beat is the one worth hearing. */
    void trigger(bool downbeat, int offsetInBlock) noexcept
    {
        frequency_        = downbeat ? kDownbeatHz : kBeatHz;
        phase_            = 0.0;
        samplesRemaining_ = (int) (kClickSeconds * sampleRate_);
        startOffset_      = offsetInBlock;
    }

    void renderInto(juce::AudioBuffer<float>& buffer, int numSamples)
    {
        if (samplesRemaining_ <= 0)
            return;

        const float  level     = level_.load(std::memory_order_relaxed);
        const double increment = juce::MathConstants<double>::twoPi * frequency_ / sampleRate_;
        const int    total     = (int) (kClickSeconds * sampleRate_);
        const int    channels  = buffer.getNumChannels();

        for (int i = juce::jmax(0, startOffset_); i < numSamples && samplesRemaining_ > 0; ++i)
        {
            // Exponential-ish decay over the click's length, so it reads as a
            // tick rather than a beep with a hard edge.
            const float envelope = (float) samplesRemaining_ / (float) juce::jmax(1, total);
            const float sample   = (float) std::sin(phase_) * envelope * envelope * level;

            for (int ch = 0; ch < channels; ++ch)
                buffer.addSample(ch, i, sample);

            phase_ += increment;
            if (phase_ >= juce::MathConstants<double>::twoPi)
                phase_ -= juce::MathConstants<double>::twoPi;
            --samplesRemaining_;
        }

        startOffset_ = 0; // only the block it was triggered in starts late
    }

    double sampleRate_       = 0.0;
    double phase_            = 0.0;
    double frequency_        = kBeatHz;
    int    samplesRemaining_ = 0;
    int    startOffset_      = 0;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> level_   { 0.5f };
};

} // namespace looper::engine
