#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace soundsplice::engine
{
/**
    Measures the device's true round trip: sends a short click out of every
    output, listens on every input for a second, and times how long it took
    to come back. Needs a cable (or the interface's own loopback) from an
    output to an input - with none, nothing comes back and it says so.

    The click is a short symmetric pulse, so a converter's filters smearing
    it still leave its peak where its peak was: the measurement is from the
    pulse's peak going out to the loudest sample coming in. This is the
    figure recordings are late by, converters and drivers included, which
    the device's reported latency often isn't.

    start() allocates and runs on the message thread before the audio thread
    sees the probe running; process() only writes into what's there.
*/
class LatencyProbe
{
public:
    /** Starts a measurement listening for @p seconds. Message thread. */
    void start(double sampleRate, double seconds = 1.0)
    {
        running_.store(false, std::memory_order_release);
        capture_.assign((size_t) juce::jmax(1, (int) (sampleRate * seconds)), 0.0f);
        position_ = 0;
        result_.store(kPending, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
    }

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    /** The round trip in samples once a measurement is done, kNoSignal if
        nothing came back, or kPending while it's still listening. */
    int result() const noexcept { return result_.load(std::memory_order_acquire); }

    static constexpr int kPending  = -2;
    static constexpr int kNoSignal = -1;

    /** Audio thread: adds the click to @p output at the start of a
        measurement, and keeps what comes in, loudest input per sample. */
    void process(const float* const* input, int numInputs, float* const* output, int numOutputs,
                 int numSamples) noexcept
    {
        if (! running_.load(std::memory_order_acquire))
            return;

        for (int n = 0; n < numSamples && position_ < (int) capture_.size(); ++n, ++position_)
        {
            if (position_ < (int) kPulse.size())
                for (int ch = 0; ch < numOutputs; ++ch)
                    if (output[ch] != nullptr)
                        output[ch][n] += kPulse[(size_t) position_];

            float loudest = 0.0f;
            for (int ch = 0; ch < numInputs; ++ch)
                if (input != nullptr && input[ch] != nullptr)
                    loudest = juce::jmax(loudest, std::abs(input[ch][n]));
            capture_[(size_t) position_] = loudest;
        }

        if (position_ >= (int) capture_.size())
            finish();
    }

private:
    void finish() noexcept
    {
        int   peakAt = 0;
        float peak   = 0.0f;
        for (int i = 0; i < (int) capture_.size(); ++i)
            if (capture_[(size_t) i] > peak)
            {
                peak   = capture_[(size_t) i];
                peakAt = i;
            }

        // Under this, what came back is noise, not the click.
        result_.store(peak >= 0.05f ? juce::jmax(0, peakAt - kPulsePeak) : kNoSignal, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }

    static constexpr std::array<float, 7> kPulse { 0.15f, 0.4f, 0.7f, 0.9f, 0.7f, 0.4f, 0.15f };
    static constexpr int                  kPulsePeak = 3;

    std::vector<float> capture_;
    int                position_ = 0; // audio thread while running
    std::atomic<int>   result_ { kPending };
    std::atomic<bool>  running_ { false };
};

} // namespace soundsplice::engine
