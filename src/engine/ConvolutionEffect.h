#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/Convolver.h"
#include "engine/Resample.h"
#include "engine/SequenceAudioFormat.h"

namespace soundsplice::engine
{
/**
    The convolution reverb as a chain node: each channel through its own
    Convolver (a stereo response gives each side its own; a mono one is used
    for both), after a pre-delay, mixed with the dry signal.

    A new response is built on the message thread, whole — resampled to the
    engine's rate, normalised, partitioned and transformed — and handed over
    under a lock the audio thread only ever tries, never waits on. The one it
    replaces is handed back the same way and freed on the message thread, so
    the audio thread neither allocates nor frees.
*/
class ConvolutionReverbEffect
{
public:
    static constexpr double kLongestSeconds  = 8.0;   // a response longer is cut here
    static constexpr float  kLongestPreDelay = 500.0f;

    ConvolutionReverbEffect() = default;

    void prepare(double sampleRate, int)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        const int delay = (int) std::ceil(sampleRate_ * kLongestPreDelay / 1000.0) + 1;
        for (auto& line : preDelay_)
            line.assign((size_t) delay, 0.0f);
        writeAt_ = 0;

        // Prepared with the audio stopped, so this can build in place.
        const juce::SpinLock::ScopedLockType lock(lock_);
        active_ = build();
        pending_.reset();
        retired_.reset();
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setMix(float mix)        { mix_.store(juce::jlimit(0.0f, 1.0f, mix), std::memory_order_relaxed); }
    void setPreDelayMs(float ms)  { preDelayMs_.store(juce::jlimit(0.0f, kLongestPreDelay, ms), std::memory_order_relaxed); }
    void setGainDb(float db)      { gainDb_.store(db, std::memory_order_relaxed); }

    /** Message thread: the response, as channels at @p rate, or none for the
        built-in hall. */
    void setImpulse(std::vector<std::vector<float>> channels, double rate)
    {
        source_     = std::move(channels);
        sourceRate_ = rate;
        if (sampleRate_ <= 0.0)
            return; // built when prepared

        auto built = build();
        const juce::SpinLock::ScopedLockType lock(lock_);
        retired_.reset();
        pending_ = std::move(built);
    }

    /** Message thread: frees whatever the audio thread has finished with. */
    void collectRetired()
    {
        const juce::SpinLock::ScopedLockType lock(lock_);
        retired_.reset();
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        {
            const juce::SpinLock::ScopedTryLockType lock(lock_);
            if (lock.isLocked() && pending_ != nullptr && retired_ == nullptr)
            {
                retired_ = std::move(active_);
                active_  = std::move(pending_);
            }
        }

        if (! enabled_.load(std::memory_order_relaxed) || active_ == nullptr || preDelay_[0].empty())
            return;

        const float mix    = mix_.load(std::memory_order_relaxed);
        const float wetMix = mix * juce::Decibels::decibelsToGain(gainDb_.load(std::memory_order_relaxed));
        const int   size   = (int) preDelay_[0].size();
        const int   delay  = juce::jlimit(0, size - 1,
                                          (int) std::lround(preDelayMs_.load(std::memory_order_relaxed) * 0.001 * sampleRate_));
        const int   channels = juce::jmin(buffer.getNumChannels(), 2);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const int readAt = (writeAt_ - delay + size) % size;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float dry = buffer.getSample(ch, n);
                auto&       line = preDelay_[(size_t) ch];
                line[(size_t) writeAt_] = dry;
                const float wet = active_->channels[(size_t) ch].processSample(line[(size_t) readAt]);
                buffer.setSample(ch, n, dry * (1.0f - mix) + wet * wetMix);
            }
            writeAt_ = (writeAt_ + 1) % size;
        }
    }

private:
    struct Kernel
    {
        std::array<Convolver, 2> channels;
    };

    std::unique_ptr<Kernel> build() const
    {
        auto channels = source_.empty() ? convolution::syntheticHall(sampleRate_) : source_;
        if (! source_.empty())
        {
            if (sourceRate_ > 0.0 && std::abs(sourceRate_ - sampleRate_) > 0.5)
            {
                const Resampler resampler(sourceRate_, sampleRate_);
                for (auto& channel : channels)
                    channel = resampler.processAll(channel);
            }
            const auto longest = (size_t) (kLongestSeconds * sampleRate_);
            for (auto& channel : channels)
                if (channel.size() > longest)
                    channel.resize(longest);
            convolution::normalise(channels);
        }

        auto kernel = std::make_unique<Kernel>();
        for (int ch = 0; ch < 2; ++ch)
            kernel->channels[(size_t) ch].setImpulse(channels.empty() ? std::vector<float> {}
                                                     : channels[(size_t) std::min(ch, (int) channels.size() - 1)]);
        return kernel;
    }

    double                              sampleRate_ = 0.0;
    std::vector<std::vector<float>>     source_;     // message thread only
    double                              sourceRate_ = 0.0;

    juce::SpinLock                      lock_;
    std::unique_ptr<Kernel>             active_;     // the audio thread's
    std::unique_ptr<Kernel>             pending_;    // message thread -> audio thread
    std::unique_ptr<Kernel>             retired_;    // audio thread -> message thread

    std::array<std::vector<float>, 2>   preDelay_;
    int                                 writeAt_ = 0;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> mix_ { 0.3f };
    std::atomic<float> preDelayMs_ { 0.0f };
    std::atomic<float> gainDb_ { 0.0f };
};

/** Reads an impulse response from @p path, any format the app plays. False,
    leaving @p channels empty, if it can't be read. */
inline bool loadImpulseFile(const std::string& path, std::vector<std::vector<float>>& channels, double& sampleRate)
{
    channels.clear();
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    if (path.empty() || ! file.existsAsFile())
        return false;

    juce::AudioFormatManager formats;
    sequencefile::registerFormats(formats);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    // Never more than the longest response kept, at the file's own rate.
    const auto frames = (int) std::min<juce::int64>(reader->lengthInSamples,
                                                    (juce::int64) (reader->sampleRate * ConvolutionReverbEffect::kLongestSeconds) + 1);
    const int  count  = (int) juce::jlimit(1u, 2u, reader->numChannels);
    juce::AudioBuffer<float> buffer(count, frames);
    reader->read(&buffer, 0, frames, 0, true, count > 1);

    for (int ch = 0; ch < count; ++ch)
        channels.emplace_back(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + frames);
    sampleRate = reader->sampleRate;
    return true;
}

} // namespace soundsplice::engine
