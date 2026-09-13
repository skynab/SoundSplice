#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <map>
#include <memory>

namespace looper
{
/**
    Waveform thumbnails for the audio files on the timeline, one per path.

    A clip's rectangle should show what's in it, and for an audio clip that
    means the waveform. Reading the file to draw it is out of the question —
    paint runs on every scroll, resize and playhead tick, and decoding audio
    there would stall the UI on the first frame and every frame after.

    juce::AudioThumbnail exists for exactly this: it scans the file on a
    background thread, keeps a low-resolution peak summary, and draws that
    summary at any width. This owns one per file path and the reader thread
    they share.

    Thumbnails are created when the song is set, not when a clip is painted.
    Creating one starts a file scan and allocates; doing that inside paint
    would put both on the path that has to stay cheap.

    Loading is asynchronous, so a thumbnail is empty for the first frames
    after a file is added. onUpdated fires as the scan progresses, and the
    view repaints — otherwise the clip would stay blank until something else
    happened to invalidate it.
*/
class WaveformCache : private juce::ChangeListener
{
public:
    WaveformCache()
    {
        formats_.registerBasicFormats();
    }

    ~WaveformCache() override
    {
        // Each thumbnail is a broadcaster this is listening to; they must stop
        // pointing at a listener that is being destroyed.
        for (auto& [path, thumbnail] : thumbnails_)
        {
            juce::ignoreUnused(path);
            thumbnail->removeChangeListener(this);
        }
    }

    /** Fired as a scan progresses, so the owner can repaint. */
    std::function<void()> onUpdated;

    /** Makes sure a thumbnail exists for @p file and has been asked to load.
        Cheap and idempotent after the first call for a given path. */
    void ensure(const juce::File& file)
    {
        if (! file.existsAsFile())
            return;

        const auto key = file.getFullPathName();
        if (thumbnails_.find(key) != thumbnails_.end())
            return;

        auto thumbnail = std::make_unique<juce::AudioThumbnail>(kSamplesPerThumbnailSample,
                                                                formats_, thumbnailCache_);
        thumbnail->addChangeListener(this);
        thumbnail->setSource(new juce::FileInputSource(file));

        thumbnails_.emplace(key, std::move(thumbnail));
    }

    /** The thumbnail for @p file, or nullptr if none has been made — which is
        the normal state for a file that has just been added, and for one that
        no longer exists on disk. */
    juce::AudioThumbnail* find(const juce::File& file) const
    {
        const auto it = thumbnails_.find(file.getFullPathName());
        return it != thumbnails_.end() ? it->second.get() : nullptr;
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (onUpdated)
            onUpdated();
    }

    // 512 samples per summary point is JUCE's own default for this: fine
    // enough that a drum hit is visible at clip size, coarse enough that a
    // long file summarises quickly.
    static constexpr int kSamplesPerThumbnailSample = 512;

    juce::AudioFormatManager  formats_;
    juce::AudioThumbnailCache thumbnailCache_ { 64 };

    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformCache)
};

} // namespace looper
