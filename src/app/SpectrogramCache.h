#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <set>

#include "engine/SequenceAudioFormat.h"
#include "engine/Spectrogram.h"
#include "SpectrogramImage.h"

namespace soundsplice
{
/**
    Spectrograms of the audio files on the timeline, one per path, for
    drawing clips in the arrangement's lanes as spectrograms (Audacity's
    spectrogram track view).

    As with WaveformCache, nothing is read while painting: a file is analysed
    on a background thread when it's asked for, and onUpdated fires once its
    picture is ready. Only the picture is kept, 128 rows by at most a few
    thousand columns, not the analysis, so a project with many files stays
    small; a change of scale or colouring analyses them again.
*/
class SpectrogramCache : private juce::Thread
{
public:
    SpectrogramCache() : juce::Thread("Lane spectrograms")
    {
        engine::sequencefile::registerFormats(formats_);
    }

    ~SpectrogramCache() override
    {
        *alive_ = false;
        signalThreadShouldExit();
        notify();
        stopThread(10000);
    }

    /** Fired on the message thread as each file's picture is ready. */
    std::function<void()> onUpdated;

    struct Picture
    {
        juce::Image image;             // invalid while it's being made
        double      secondsPerColumn = 0.0;
        double      windowSeconds    = 0.0;

        bool isReady() const noexcept { return image.isValid() && secondsPerColumn > 0.0; }
    };

    /** Asks for @p file's picture, if it hasn't been already. */
    void ensure(const juce::File& file)
    {
        const auto key = file.getFullPathName();
        if (! file.existsAsFile() || pictures_.count(key) > 0)
            return;

        pictures_[key] = {};
        request(file);
    }

    /** @p file's picture, or nullptr if it was never asked for. */
    const Picture* find(const juce::File& file) const
    {
        const auto it = pictures_.find(file.getFullPathName());
        return it != pictures_.end() ? &it->second : nullptr;
    }

    /** Forgets every file but @p keep, so pictures of audio no longer in the
        project (an edit writes a new file) don't pile up. */
    void keepOnly(const std::set<juce::String>& keep)
    {
        for (auto it = pictures_.begin(); it != pictures_.end();)
            it = keep.count(it->first) > 0 ? std::next(it) : pictures_.erase(it);
    }

    /** How frequency is laid out and levels coloured, as in the audio
        editor; a change makes every picture again. */
    void setStyle(spectrogramimage::Scale scale, spectrogramimage::Display display)
    {
        if (scale == scale_ && display == display_)
            return;
        scale_   = scale;
        display_ = display;
        ++generation_;

        for (auto& [key, picture] : pictures_)
        {
            picture = {};
            request(juce::File(key));
        }
    }

    static constexpr int kRows = 128;

private:
    void request(const juce::File& file)
    {
        {
            const juce::ScopedLock lock(queueLock_);
            queue_.push_back(file);
        }
        if (! isThreadRunning())
            startThread(juce::Thread::Priority::low);
        notify();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            juce::File next;
            {
                const juce::ScopedLock lock(queueLock_);
                if (! queue_.empty())
                {
                    next = queue_.front();
                    queue_.pop_front();
                }
            }

            if (next == juce::File())
            {
                wait(-1);
                continue;
            }

            auto data = analyse(next);
            if (threadShouldExit())
                return;

            // The picture is made on the message thread: an image there is of
            // the kind the renderer draws, which one made here mightn't be.
            juce::MessageManager::callAsync(
                [this, alive = alive_, generation = generation_.load(), key = next.getFullPathName(),
                 data = std::move(data)]
                {
                    if (! *alive || generation != generation_.load())
                        return;
                    const auto it = pictures_.find(key);
                    if (it == pictures_.end())
                        return;

                    it->second.image            = spectrogramimage::imageOf(data, kRows, scale_, display_);
                    it->second.secondsPerColumn = data.secondsPerColumn;
                    it->second.windowSeconds    = data.windowSeconds;
                    if (onUpdated)
                        onUpdated();
                });
        }
    }

    engine::SpectrogramData analyse(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return {};

        // A shorter window than the editor's: a lane is a few dozen pixels
        // tall, and quick sounds matter more there than close pitches.
        engine::SpectrogramSettings settings;
        settings.fftSize = 1024;
        engine::SpectrogramBuilder builder(reader->sampleRate, reader->lengthInSamples, settings);

        constexpr int            kChunk   = 1 << 16;
        const int                channels = (int) juce::jlimit(1u, 2u, reader->numChannels);
        juce::AudioBuffer<float> buffer(channels, kChunk);
        for (juce::int64 at = 0; at < reader->lengthInSamples; at += kChunk)
        {
            if (threadShouldExit())
                return {};
            const int frames = (int) std::min<juce::int64>(kChunk, reader->lengthInSamples - at);
            reader->read(&buffer, 0, frames, at, true, channels > 1);
            builder.append(buffer.getArrayOfReadPointers(), channels, frames);
        }
        return builder.finish();
    }

    juce::AudioFormatManager                formats_; // the worker's alone, once made
    std::map<juce::String, Picture>         pictures_; // message thread only
    spectrogramimage::Scale                 scale_   = spectrogramimage::Scale::Logarithmic;
    spectrogramimage::Display               display_;
    std::atomic<int>                        generation_ { 0 };
    std::shared_ptr<std::atomic<bool>>      alive_ = std::make_shared<std::atomic<bool>>(true);

    juce::CriticalSection  queueLock_;
    std::deque<juce::File> queue_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrogramCache)
};

} // namespace soundsplice
