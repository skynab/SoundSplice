#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

namespace soundsplice::engine
{
/**
    Captures audio input to a file while armed and the transport is playing.

    **Streams to disk rather than to RAM.** It used to capture into a buffer
    sized once up front for a maximum take length, and drop everything past it:
    a four-minute take came back three minutes long, with no error and nothing
    to say why. A cap that silently discards the user's performance is not a
    limit, it is data loss, so there is no cap here at all - the take is as long
    as you play.

    The mechanism is `juce::AudioFormatWriter::ThreadedWriter`: a pre-allocated
    lock-free FIFO the audio thread pushes into, drained to disk by a background
    `TimeSliceThread`. Nothing here allocates or blocks on the audio thread, and
    the one case that could still lose audio - the disk failing to keep up, so
    the FIFO fills - is *counted* and reported rather than swallowed. See
    droppedSampleCount().

    **Hand-off is RT-safe without a lock**, by time-slicing rather than by
    sharing. arm() (message thread) is the only thing that creates the writer;
    process() (audio thread) is the only thing that writes to it; and the
    message thread only destroys it after observing isFinished() == true, which
    process() sets on its own thread the instant it notices recording has
    stopped. So there is no window where both sides touch the writer. That was
    the discipline when this held a RAM buffer, and it is unchanged - only what
    is being protected is different.
*/
class AudioRecorder
{
public:
    /** What a take is written as, and which of the device's inputs it takes. */
    struct Format
    {
        int  bitsPerSample = 24;    // 16, 24, or 32 (as floating point)
        int  channels      = 2;     // 1 for mono, 2 for stereo
        int  firstInput    = 0;     // the device input the take starts from
        bool operator==(const Format&) const = default;
    };

    ~AudioRecorder() { discardWriter(); }

    void prepare(double sampleRate, int numChannels)
    {
        sampleRate_  = sampleRate > 0.0 ? sampleRate : 48000.0;
        numChannels_ = juce::jmax(1, numChannels);
    }

    /** For the next take: arm() reads it. Message thread. */
    void setFormat(const Format& format)
    {
        format_      = format;
        numChannels_ = juce::jlimit(1, 2, format.channels);
        firstInput_.store(juce::jmax(0, format.firstInput), std::memory_order_relaxed);
    }

    const Format& format() const noexcept { return format_; }

    /** Sound-activated recording, from the next take: an armed take waits
        until the inputs it records reach @p thresholdGain (linear; 0 for
        off) and starts there, and, with @p stopAfterSilenceSamples, stops by
        itself once they've stayed under it that long. Message thread. */
    void setSoundTrigger(float thresholdGain, int64_t stopAfterSilenceSamples)
    {
        triggerGain_.store(juce::jmax(0.0f, thresholdGain), std::memory_order_relaxed);
        silenceStop_.store(juce::jmax((int64_t) 0, stopAfterSilenceSamples), std::memory_order_relaxed);
    }

    float   soundTriggerGain() const noexcept { return triggerGain_.load(std::memory_order_relaxed); }
    int64_t soundTriggerStopSamples() const noexcept { return silenceStop_.load(std::memory_order_relaxed); }

    /** True while an armed take is waiting for sound to start it. */
    bool isWaitingForSound() const noexcept
    {
        return armed_.load(std::memory_order_relaxed) && waitingForSound_.load(std::memory_order_relaxed);
    }

    /** True if the last take ended itself, on silence, rather than being
        stopped. */
    bool stoppedOnSilence() const noexcept { return stoppedOnSilence_.load(std::memory_order_relaxed); }

    // ---- message thread ----
    /**
        Starts a new take, written to @p destination.

        @p leadInSamples rolls the transport for that long before capture
        begins - a count-in. The lead-in is *skipped*, not recorded, so the take
        still starts at the first sample you actually played; only the wait is
        counted.

        Returns false if the file could not be opened, in which case nothing is
        armed - better to say so now than to let someone play a take into a
        writer that was never going to exist.
    */
    bool arm(const juce::File& destination, juce::TimeSliceThread& writerThread,
             int64_t leadInSamples = 0)
    {
        discardWriter(); // a previous take's writer, if it was never collected

        destination.deleteFile();

        std::unique_ptr<juce::OutputStream> stream(destination.createOutputStream());
        if (stream == nullptr)
            return false;

        // A WAV, which JUCE writes as RF64 once it passes 4 GB, so a long
        // take at a high rate isn't cut off by the format.
        const bool floating = format_.bitsPerSample >= 32;
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor(stream, // consumed on success
            juce::AudioFormatWriterOptions{}
                .withSampleRate(sampleRate_)
                .withNumChannels(numChannels_)
                .withBitsPerSample(floating ? 32 : (format_.bitsPerSample <= 16 ? 16 : 24))
                .withSampleFormat(floating ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                           : juce::AudioFormatWriterOptions::SampleFormat::integral));

        if (writer == nullptr)
            return false;

        // Buffered samples per channel. Big enough that a slow disk or a
        // momentarily busy background thread can't stall a take, small enough
        // that it isn't a hidden RAM cap by another name - this is about half a
        // second, and it drains continuously rather than filling up.
        constexpr int kFifoSamples = 32768;

        file_ = destination;
        threadedWriter_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            writer.release(), writerThread, kFifoSamples);

        writePosition_.store(0, std::memory_order_relaxed);
        droppedSamples_.store(0, std::memory_order_relaxed);
        startPlayhead_.store(-1, std::memory_order_relaxed);
        leadInRemaining_.store(juce::jmax((int64_t) 0, leadInSamples), std::memory_order_relaxed);
        waitingForSound_.store(triggerGain_.load(std::memory_order_relaxed) > 0.0f, std::memory_order_relaxed);
        stoppedOnSilence_.store(false, std::memory_order_relaxed);
        silentRun_ = 0;
        finished_.store(false, std::memory_order_relaxed);

        // Released last: it is what makes the writer visible to the audio
        // thread, so everything above must already be in place.
        activeWriter_.store(threadedWriter_.get(), std::memory_order_release);
        armed_.store(true, std::memory_order_release);
        return true;
    }

    /** Samples of count-in still to elapse before capture starts. */
    int64_t leadInRemaining() const noexcept { return leadInRemaining_.load(std::memory_order_relaxed); }

    /** Signals the audio thread to stop capturing; the take finishes on the
        next block it processes (or immediately if the transport already isn't
        playing). */
    void disarm() { armed_.store(false, std::memory_order_release); }

    bool isArmed() const noexcept { return armed_.load(std::memory_order_relaxed); }

    /** True once the audio thread has confirmed it will no longer touch the
        writer - only then is it safe to finalise the file. */
    bool isFinished() const noexcept { return finished_.load(std::memory_order_acquire); }

    /** Samples captured so far - safe to poll live for a recording-time
        readout. */
    int64_t recordedSampleCount() const noexcept { return writePosition_.load(std::memory_order_relaxed); }

    /**
        Samples that could not be written because the FIFO was full, i.e. the
        disk could not keep up.

        Exists because the failure it describes used to be invisible. Any
        non-zero value here means the recording has a gap in it, and the user
        needs to be told - a take that is quietly missing a bar is worse than
        one that failed outright, because it looks fine until it is played.
    */
    int64_t droppedSampleCount() const noexcept { return droppedSamples_.load(std::memory_order_relaxed); }

    /**
        Where the transport was when capture actually began, in samples, or -1
        if nothing was captured.

        Latched on the audio thread on the first captured block rather than
        computed on the message thread, because the count-in means capture
        starts later than arming - and re-deriving that offset from bars and
        tempo is arithmetic that can disagree with what really happened.
    */
    int64_t startPlayheadSamples() const noexcept { return startPlayhead_.load(std::memory_order_relaxed); }

    // ---- message thread, only after isFinished() ----
    /** Closes the file and returns it. Empty if nothing was captured. */
    juce::File finishTake()
    {
        const bool captured = writePosition_.load(std::memory_order_relaxed) > 0;

        // Destroying the ThreadedWriter is what flushes the FIFO and closes the
        // file, so this has to happen before anyone reads it.
        discardWriter();

        if (! captured)
        {
            file_.deleteFile(); // an empty WAV header helps nobody
            return {};
        }

        return file_;
    }

    double sampleRate() const noexcept { return sampleRate_; }

    // ---- audio thread ----
    /** @p inputChannelData may be nullptr (no input device) or have fewer
        channels than the writer expects - handled gracefully either way. */
    void process(const float* const* inputChannelData, int numInputChannels,
                 int numSamples, bool transportPlaying, int64_t playheadSamples) noexcept
    {
        const bool armedNow = armed_.load(std::memory_order_acquire);

        // Count-in: the transport is rolling and we're armed, but capture
        // hasn't started yet. Deliberately not treated as "recording", so a
        // take that is stopped during its own count-in finishes empty rather
        // than being reported as a zero-length recording that already began.
        int64_t leadIn = leadInRemaining_.load(std::memory_order_relaxed);
        if (armedNow && transportPlaying && leadIn > 0)
        {
            leadIn = juce::jmax((int64_t) 0, leadIn - (int64_t) numSamples);
            leadInRemaining_.store(leadIn, std::memory_order_relaxed);
            return;
        }

        // Sound-activated: still waiting, and nothing has started, until the
        // inputs it records reach the threshold - then this block starts it.
        if (armedNow && transportPlaying && waitingForSound_.load(std::memory_order_relaxed))
        {
            if (inputPeak(inputChannelData, numInputChannels, numSamples) < triggerGain_.load(std::memory_order_relaxed))
                return;
            waitingForSound_.store(false, std::memory_order_relaxed);
        }

        const bool recordingNow = armedNow && transportPlaying;

        // The take ends when capture stops...
        if (wasRecording_ && ! recordingNow)
            finished_.store(true, std::memory_order_release);
        wasRecording_ = recordingNow;

        // ...and also whenever we're disarmed without ever having captured
        // anything - stopped during a count-in, or armed and stopped before
        // the transport rolled. Without this, finished_ is never published,
        // isFinished() stays false forever, and the owner waits on a take that
        // will never arrive (which leaves the UI stuck mid-record and unable to
        // start another one). Cheap to check: finished_ starts true, so this
        // only fires on the transition out of an armed take.
        if (! armedNow && ! finished_.load(std::memory_order_relaxed))
            finished_.store(true, std::memory_order_release);

        if (! recordingNow || inputChannelData == nullptr || numSamples <= 0)
            return;

        auto* writer = activeWriter_.load(std::memory_order_acquire);
        if (writer == nullptr)
            return;

        if (startPlayhead_.load(std::memory_order_relaxed) < 0)
            startPlayhead_.store(playheadSamples, std::memory_order_relaxed);

        // The writer wants exactly the channel count it was created with. A
        // device offering fewer means duplicating what there is rather than
        // handing it a null pointer, which it documents as not allowed - and a
        // mono source recorded to a silent right channel would be a bug that
        // only shows up on playback.
        // From the chosen input on; an input the device doesn't have falls
        // back to its last, so a take still has sound in it.
        const int    first = firstInput_.load(std::memory_order_relaxed);
        const float* channels[kMaxChannels] {};
        for (int ch = 0; ch < numChannels_ && ch < kMaxChannels; ++ch)
        {
            const int source = juce::jmin(first + ch, numInputChannels - 1);
            channels[ch] = source >= 0 ? inputChannelData[source] : nullptr;
        }

        if (channels[0] == nullptr)
            return;

        if (writer->write(channels, numSamples))
            writePosition_.fetch_add(numSamples, std::memory_order_relaxed);
        else
            droppedSamples_.fetch_add(numSamples, std::memory_order_relaxed);

        // Stopping on silence: disarmed here, so the take finishes on the
        // next block exactly as if Stop had been pressed.
        const auto stopAfter = silenceStop_.load(std::memory_order_relaxed);
        const auto threshold = triggerGain_.load(std::memory_order_relaxed);
        if (stopAfter > 0 && threshold > 0.0f)
        {
            silentRun_ = inputPeak(inputChannelData, numInputChannels, numSamples) < threshold ? silentRun_ + numSamples : 0;
            if (silentRun_ >= stopAfter)
            {
                stoppedOnSilence_.store(true, std::memory_order_relaxed);
                armed_.store(false, std::memory_order_release);
            }
        }
    }

private:
    static constexpr int kMaxChannels = 8;

    /** The loudest sample this block on the inputs a take records. */
    float inputPeak(const float* const* input, int numInputChannels, int numSamples) const noexcept
    {
        if (input == nullptr || numInputChannels <= 0)
            return 0.0f;

        const int first = firstInput_.load(std::memory_order_relaxed);
        float     peak  = 0.0f;
        for (int ch = 0; ch < numChannels_; ++ch)
            if (const float* samples = input[juce::jmin(first + ch, numInputChannels - 1)])
                for (int n = 0; n < numSamples; ++n)
                    peak = juce::jmax(peak, std::abs(samples[n]));
        return peak;
    }

    /** Message thread. Stops the audio thread seeing the writer first, then
        destroys it - which flushes the FIFO and closes the file. */
    void discardWriter()
    {
        activeWriter_.store(nullptr, std::memory_order_release);
        threadedWriter_.reset();
    }

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter_;
    juce::File                                              file_;

    // What the audio thread sees. Separate from the owning pointer above so
    // that publishing and revoking it are single atomic operations.
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter_ { nullptr };

    std::atomic<int64_t> writePosition_   { 0 };
    std::atomic<int64_t> droppedSamples_  { 0 };
    std::atomic<int64_t> startPlayhead_   { -1 };
    std::atomic<int64_t> leadInRemaining_ { 0 }; // count-in still to elapse
    std::atomic<bool>    armed_    { false };
    std::atomic<bool>    finished_ { true };     // true initially: no take pending

    Format           format_;
    std::atomic<int> firstInput_ { 0 };

    std::atomic<float>   triggerGain_      { 0.0f };
    std::atomic<int64_t> silenceStop_      { 0 };
    std::atomic<bool>    waitingForSound_  { false };
    std::atomic<bool>    stoppedOnSilence_ { false };
    int64_t              silentRun_ = 0; // audio-thread only
    bool   wasRecording_ = false;                // audio-thread only
    double sampleRate_   = 48000.0;
    int    numChannels_  = 2;
};

} // namespace soundsplice::engine
