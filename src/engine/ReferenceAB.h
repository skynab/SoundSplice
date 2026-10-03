#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rt/SpscRingBuffer.h"

namespace soundsplice::engine
{
/**
    Loudness-matched A/B against a reference track: switch what's heard
    between the mix (A) and a finished record (B), each turned down to the
    quieter's loudness so louder can't pass for better.

    The reference follows the transport - B plays the reference from where
    the playhead is, so flipping back and forth compares the same moment -
    and loops if the song runs longer. Monitoring only: it's applied in the
    device callback after the mix, so it never reaches an export (which
    only ever calls processBlock), and the mix's own level is untouched.

    The reference's audio is handed to the audio thread through a single
    slot and retired through a queue, so loading one never blocks playback.
    The slot holds only the latest: loading several while no device runs
    (nothing takes them) keeps the newest, the ones it replaced deleted
    here, since the audio thread never saw them.
*/
struct ReferenceAudio
{
    std::vector<std::vector<float>> channels; // one or two
    double                          sampleRate = 48000.0;

    int64_t length() const noexcept { return channels.empty() ? 0 : (int64_t) channels[0].size(); }
};

class ReferenceAB
{
public:
    enum Mode
    {
        Off = 0, // the mix, as it is
        A   = 1, // the mix, at its matched level
        B   = 2, // the reference, at its matched level
    };

    ~ReferenceAB()
    {
        collectRetired();
        if (auto* pending = pending_.load(); pending != nothing())
            delete pending;
        delete current_;
    }

    // ---- message thread

    /** Hands @p audio (or nothing, to clear it) to the audio thread. */
    void setAudio(ReferenceAudio* audio)
    {
        collectRetired();
        // Whatever this replaces was never taken, so it's still ours.
        if (auto* replaced = pending_.exchange(audio, std::memory_order_acq_rel); replaced != nothing())
            delete replaced;
    }

    void setMode(Mode mode) noexcept { mode_.store(mode, std::memory_order_relaxed); }
    Mode mode() const noexcept { return (Mode) mode_.load(std::memory_order_relaxed); }

    /** The matched levels, linear: the louder of the two turned down. */
    void setGains(float mixGain, float referenceGain) noexcept
    {
        mixGain_.store(mixGain, std::memory_order_relaxed);
        referenceGain_.store(referenceGain, std::memory_order_relaxed);
    }

    void collectRetired()
    {
        ReferenceAudio* old = nullptr;
        while (reclaim_.pop(old))
            delete old;
    }

    // ---- audio thread

    /** After the mix is in @p output (@p channels of @p frames): leaves it,
        turns it to A's level, or replaces it with the reference at the
        playhead. */
    void process(float* const* output, int channels, int frames, int64_t playheadSamples, double deviceRate,
                 bool playing) noexcept
    {
        if (auto* incoming = pending_.exchange(nothing(), std::memory_order_acq_rel); incoming != nothing())
        {
            // A full reclaim queue leaks the old one until the destructor
            // rather than ever blocking here.
            if (current_ != nullptr)
                (void) reclaim_.push(current_);
            current_ = incoming;
        }

        const auto mode = (Mode) mode_.load(std::memory_order_relaxed);
        if (mode == Off)
            return;

        if (mode == A || current_ == nullptr || current_->length() == 0)
        {
            const float gain = mode == A ? mixGain_.load(std::memory_order_relaxed) : 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < frames; ++i)
                    output[ch][i] *= gain;
            return;
        }

        // B: the reference, from where the playhead is.
        const float  gain   = referenceGain_.load(std::memory_order_relaxed);
        const double ratio  = current_->sampleRate / (deviceRate > 0.0 ? deviceRate : 48000.0);
        const auto   length = current_->length();
        const int    refChannels = (int) current_->channels.size();
        for (int i = 0; i < frames; ++i)
        {
            const double position = std::fmod((double) (playheadSamples + i) * ratio, (double) length);
            const auto   at       = (int64_t) position;
            const float  frac     = (float) (position - (double) at);
            const auto   next     = at + 1 < length ? at + 1 : 0;
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto& source = current_->channels[(size_t) std::min(ch, refChannels - 1)];
                const float value  = source[(size_t) at] + frac * (source[(size_t) next] - source[(size_t) at]);
                output[ch][i] = playing ? value * gain : 0.0f;
            }
        }
    }

private:
    /** "Nothing waiting" in the slot: nullptr is a real hand-off (clear). */
    ReferenceAudio* nothing() noexcept { return &nothing_; }

    ReferenceAudio                      nothing_;
    std::atomic<ReferenceAudio*>        pending_ { &nothing_ };
    rt::SpscRingBuffer<ReferenceAudio*> reclaim_ { 8 };
    ReferenceAudio*                     current_ = nullptr; // the audio thread's
    std::atomic<int>                    mode_ { Off };
    std::atomic<float>                  mixGain_ { 1.0f }, referenceGain_ { 1.0f };
};

/** The matched gains for a mix at @p mixLufs and a reference at
    @p referenceLufs: the louder turned down to the quieter, never up. */
inline void matchedGains(double mixLufs, double referenceLufs, float& mixGain, float& referenceGain)
{
    mixGain = referenceGain = 1.0f;
    if (! std::isfinite(mixLufs) || ! std::isfinite(referenceLufs))
        return;
    const double quieter = std::min(mixLufs, referenceLufs);
    mixGain       = (float) std::pow(10.0, (quieter - mixLufs) / 20.0);
    referenceGain = (float) std::pow(10.0, (quieter - referenceLufs) / 20.0);
}

} // namespace soundsplice::engine
