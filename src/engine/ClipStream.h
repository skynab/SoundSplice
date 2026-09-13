#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace soundsplice::engine
{
/**
    A long audio clip played from disk rather than decoded into memory.

    Every clip used to be decoded whole into RAM when a track was loaded, so a
    two-hour recording cost around 2.5 GB before a note had played. A stream
    keeps only the audio near where it's being played: the file is split into
    fixed-size pages, a loader thread reads the pages ahead of every position
    that's playing, and pages far from all of them are let go again.

    The audio thread never waits for any of that. It reads through atomic page
    pointers and hears silence where a page isn't loaded yet, which only
    happens for a moment after a jump or at the very start of playback. An
    offline render isn't in real time, so it loads a missing page on the spot
    (pageNow) and an export never has a gap.

    Freeing a page is the one delicate part, since the audio thread may be
    reading it at that instant. A page is unlinked first and only deleted once
    the block epoch (advanced at the start of every audio block, see
    ClipStreamer::beginBlock) has moved past the value it had when it was
    unlinked: the audio thread is one thread running one block at a time, so a
    newer epoch means the block that could have seen the page has finished.

    Threads: construct and destroy on the message thread; noteReading,
    loadedPage and Cursor on the audio thread; service and freeRetired on the
    loader thread; pageNow on a thread that may block (an offline render).
*/
class ClipStream
{
public:
    static constexpr int          kPageShift  = 16;
    static constexpr std::int64_t kPageFrames = std::int64_t { 1 } << kPageShift; // about 1.4 s at 48 kHz

    /** Readers that can report where they're playing at once: every track's
        player and the preview player. */
    static constexpr int kMaxReaders = 64;

    static constexpr double kReadAheadSeconds = 15.0;
    /** Pages within this of any playing position are kept, so a short jump
        back or a loop doesn't have to wait for the disk. */
    static constexpr double kKeepSeconds = 30.0;

    /** Blocks after which a reported position no longer counts as playing:
        about ten seconds at the usual block sizes. */
    static constexpr std::uint64_t kFreshEpochs = 1000;
    /** Blocks with no playing position at all after which every page is let
        go: about two minutes. Until then, stopping and starting again finds
        the audio still loaded. */
    static constexpr std::uint64_t kIdleEpochs = 12000;

    struct Page
    {
        juce::AudioBuffer<float> audio;
    };

    ClipStream(std::unique_ptr<juce::AudioFormatReader> reader, std::shared_ptr<std::atomic<std::uint64_t>> epoch)
        : reader_(std::move(reader)), epoch_(std::move(epoch))
    {
        jassert(reader_ != nullptr && epoch_ != nullptr);

        sampleRate_  = reader_->sampleRate;
        numChannels_ = juce::jmax(1, (int) reader_->numChannels);
        length_      = juce::jmax<std::int64_t>(0, reader_->lengthInSamples);
        numPages_    = (int) ((length_ + kPageFrames - 1) >> kPageShift);

        pages_ = std::make_unique<std::atomic<Page*>[]>((size_t) numPages_);
        for (int i = 0; i < numPages_; ++i)
            pages_[(size_t) i].store(nullptr);

        for (auto& hint : hints_)
            hint.store(-1);
        for (auto& epoch : hintEpochs_)
            epoch.store(0);
    }

    ~ClipStream()
    {
        for (int i = 0; i < numPages_; ++i)
            delete pages_[(size_t) i].load();
        for (const auto& retired : retired_)
            delete retired.first;
    }

    double       sampleRate() const noexcept { return sampleRate_; }
    int          numChannels() const noexcept { return numChannels_; }
    std::int64_t lengthFrames() const noexcept { return length_; }
    int          numPages() const noexcept { return numPages_; }

    // ---- audio thread ----------------------------------------------------

    /** Reader @p reader is playing around @p frame in the block with epoch
        @p epoch. What the loader reads ahead of. */
    void noteReading(int reader, std::int64_t frame, std::uint64_t epoch) noexcept
    {
        if (reader < 0 || reader >= kMaxReaders)
            return;

        hints_[(size_t) reader].store(juce::jmax<std::int64_t>(0, frame), std::memory_order_relaxed);
        hintEpochs_[(size_t) reader].store(epoch, std::memory_order_relaxed);
    }

    /** Page @p index if it's loaded, or nullptr. Never waits. */
    const Page* loadedPage(int index) const noexcept
    {
        return index >= 0 && index < numPages_ ? pages_[(size_t) index].load(std::memory_order_acquire) : nullptr;
    }

    /** Reads samples from a stream within one block, remembering the last two
        pages it looked up (a linear read's two samples straddle a page edge
        once per page). @p blocking loads missing pages instead of reading
        silence: offline renders only. */
    class Cursor
    {
    public:
        Cursor(ClipStream& stream, bool blocking) noexcept : stream_(stream), blocking_(blocking) {}

        float sample(int channel, std::int64_t frame)
        {
            const int   index = (int) (frame >> kPageShift);
            const Page* page  = nullptr;

            if (index == index0_)
                page = page0_;
            else if (index == index1_)
                page = page1_;
            else
            {
                page    = blocking_ ? stream_.pageNow(index) : stream_.loadedPage(index);
                index1_ = index0_;
                page1_  = page0_;
                index0_ = index;
                page0_  = page;
            }

            if (page == nullptr)
                return 0.0f;

            const auto& audio = page->audio;
            const int   offset = (int) (frame & (kPageFrames - 1));
            if (offset >= audio.getNumSamples())
                return 0.0f;

            return audio.getReadPointer(juce::jmin(channel, audio.getNumChannels() - 1))[offset];
        }

        /** Exactly engine::sampleLinear over the whole file, so a streamed
            clip sounds bit for bit like the same clip decoded into memory. */
        float sampleLinear(int channel, double position, std::int64_t length)
        {
            if (length <= 0 || position < 0.0 || position >= (double) length)
                return 0.0f;

            const auto   i0   = (std::int64_t) position;
            const double frac = position - (double) i0;
            const float  s0   = sample(channel, i0);
            const float  s1   = i0 + 1 < length ? sample(channel, i0 + 1) : s0;

            return (float) (s0 + (s1 - s0) * frac);
        }

    private:
        ClipStream& stream_;
        bool        blocking_;
        int         index0_ = -1, index1_ = -1;
        const Page* page0_ = nullptr;
        const Page* page1_ = nullptr;
    };

    // ---- a thread that may block ------------------------------------------

    /** Page @p index, loading it now if it isn't loaded. For an offline
        render, which must not skip audio. nullptr only past the end. */
    const Page* pageNow(int index)
    {
        if (const auto* page = loadedPage(index))
            return page;

        if (index < 0 || index >= numPages_)
            return nullptr;

        const std::lock_guard<std::mutex> lock(readerMutex_);
        if (const auto* page = loadedPage(index))
            return page;

        return load(index);
    }

    // ---- loader thread -----------------------------------------------------

    /** Loads up to @p maxPages missing pages ahead of every recent playing
        position, nearest first, and lets go of pages far from all of them.
        Returns how many pages it loaded. */
    int service(int maxPages = 4)
    {
        freeRetired();

        const auto now = epoch_->load(std::memory_order_acquire);

        std::array<std::int64_t, kMaxReaders> fresh {};
        int  freshCount = 0;
        bool idle       = true;

        for (int r = 0; r < kMaxReaders; ++r)
        {
            const auto hint = hints_[(size_t) r].load(std::memory_order_relaxed);
            if (hint < 0)
                continue;

            const auto when = hintEpochs_[(size_t) r].load(std::memory_order_relaxed);
            const auto age  = now > when ? now - when : 0;

            if (age <= kFreshEpochs)
                fresh[(size_t) freshCount++] = hint;
            if (age <= kIdleEpochs)
                idle = false;
        }

        if (freshCount == 0)
        {
            // Nothing has played this stream for a long while: none of it
            // needs to stay in memory.
            if (idle)
                retireWhere([](std::int64_t, std::int64_t) { return true; });
            return 0;
        }

        const int aheadPages = (int) ((std::int64_t) (kReadAheadSeconds * sampleRate_) >> kPageShift) + 1;
        int       loaded     = 0;

        // The page being played first, then the one before it (a read that
        // starts a sample early, or interpolation), then onwards.
        for (int step = 0; step <= aheadPages + 1 && loaded < maxPages; ++step)
        {
            const int offset = step == 0 ? 0 : step == 1 ? -1 : step - 1;

            for (int h = 0; h < freshCount && loaded < maxPages; ++h)
            {
                const int index = (int) (fresh[(size_t) h] >> kPageShift) + offset;
                if (index < 0 || index >= numPages_ || loadedPage(index) != nullptr)
                    continue;

                const std::lock_guard<std::mutex> lock(readerMutex_);
                if (loadedPage(index) == nullptr)
                {
                    load(index);
                    ++loaded;
                }
            }
        }

        const auto keep = (std::int64_t) (kKeepSeconds * sampleRate_);
        retireWhere([&fresh, freshCount, keep](std::int64_t pageStart, std::int64_t pageEnd)
        {
            for (int h = 0; h < freshCount; ++h)
                if (pageEnd > fresh[(size_t) h] - keep && pageStart < fresh[(size_t) h] + keep)
                    return false;
            return true;
        });

        return loaded;
    }

    /** Deletes the pages let go of whose last possible reader has finished. */
    void freeRetired()
    {
        const auto now = epoch_->load(std::memory_order_acquire);
        const auto end = std::remove_if(retired_.begin(), retired_.end(), [now](const auto& retired)
        {
            if (now <= retired.second)
                return false;
            delete retired.first;
            return true;
        });
        retired_.erase(end, retired_.end());
    }

    int loadedPageCount() const noexcept
    {
        int count = 0;
        for (int i = 0; i < numPages_; ++i)
            count += loadedPage(i) != nullptr ? 1 : 0;
        return count;
    }

    int retiredPageCount() const noexcept { return (int) retired_.size(); }

private:
    /** Reads page @p index from the file and publishes it. Reader mutex held. */
    const Page* load(int index)
    {
        const auto start  = (std::int64_t) index << kPageShift;
        const int  frames = (int) juce::jmin(kPageFrames, length_ - start);

        auto page = std::make_unique<Page>();
        page->audio.setSize(numChannels_, frames);
        page->audio.clear();
        reader_->read(page->audio.getArrayOfWritePointers(), numChannels_, start, frames);

        auto* published = page.release();
        pages_[(size_t) index].store(published, std::memory_order_release);
        return published;
    }

    template <typename ShouldRetire>
    void retireWhere(ShouldRetire&& shouldRetire)
    {
        for (int i = 0; i < numPages_; ++i)
        {
            auto* page = pages_[(size_t) i].load(std::memory_order_acquire);
            if (page == nullptr)
                continue;

            const auto pageStart = (std::int64_t) i << kPageShift;
            if (! shouldRetire(pageStart, pageStart + kPageFrames))
                continue;

            if (pages_[(size_t) i].compare_exchange_strong(page, nullptr, std::memory_order_acquire))
            {
                // Stamped after unlinking: only a block that began before this
                // point can have seen the page.
                retired_.emplace_back(page, epoch_->load(std::memory_order_acquire));
            }
        }
    }

    std::unique_ptr<juce::AudioFormatReader>    reader_;
    std::mutex                                  readerMutex_;
    std::shared_ptr<std::atomic<std::uint64_t>> epoch_;

    double       sampleRate_  = 0.0;
    int          numChannels_ = 1;
    std::int64_t length_      = 0;
    int          numPages_    = 0;

    std::unique_ptr<std::atomic<Page*>[]>            pages_;
    std::array<std::atomic<std::int64_t>, kMaxReaders>  hints_;
    std::array<std::atomic<std::uint64_t>, kMaxReaders> hintEpochs_;

    std::vector<std::pair<Page*, std::uint64_t>> retired_; // loader thread only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipStream)
};

/**
    Owns the block epoch every stream shares and keeps all of them serviced
    from one loader thread.
*/
class ClipStreamer : private juce::TimeSliceClient
{
public:
    /** Loads on @p thread, which must outlive this. */
    explicit ClipStreamer(juce::TimeSliceThread& thread) : thread_(thread)
    {
        thread_.addTimeSliceClient(this);
    }

    ~ClipStreamer() override
    {
        thread_.removeTimeSliceClient(this);
    }

    /** Audio thread, or an offline render standing in for it: at the start of
        every block, before any page is read. Returns the block's epoch. */
    std::uint64_t beginBlock() noexcept
    {
        return epoch_->fetch_add(1, std::memory_order_acquire) + 1;
    }

    /** Message thread: a stream over @p reader, serviced from now on for as
        long as anything holds it. */
    std::shared_ptr<ClipStream> open(std::unique_ptr<juce::AudioFormatReader> reader)
    {
        auto stream = std::make_shared<ClipStream>(std::move(reader), epoch_);

        // Nothing has played it yet, so nothing says where to read: its start
        // is the likeliest place, and loading it now saves a gap there.
        stream->noteReading(ClipStream::kMaxReaders - 1, 0, epoch_->load());

        const std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.push_back(stream);
        return stream;
    }

private:
    int useTimeSlice() override
    {
        std::vector<std::shared_ptr<ClipStream>> live;
        {
            const std::lock_guard<std::mutex> lock(streamsMutex_);
            streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                          [](const auto& weak) { return weak.expired(); }),
                           streams_.end());

            for (const auto& weak : streams_)
                if (auto stream = weak.lock())
                    live.push_back(std::move(stream));
        }

        int loaded = 0;
        for (const auto& stream : live)
            loaded += stream->service();

        // Straight back while there's reading to do; otherwise often enough
        // that a jump is heard within a block or two.
        return loaded > 0 ? 1 : 5;
    }

    juce::TimeSliceThread&                       thread_;
    std::shared_ptr<std::atomic<std::uint64_t>>  epoch_ = std::make_shared<std::atomic<std::uint64_t>>(1);
    std::mutex                                   streamsMutex_;
    std::vector<std::weak_ptr<ClipStream>>       streams_;
};

} // namespace soundsplice::engine
