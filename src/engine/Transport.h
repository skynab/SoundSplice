#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "engine/ProcessContext.h"
#include "engine/TempoMap.h"

namespace looper::engine
{
/**
    The playhead and its state: playing/stopped, position, loop region, tempo.

    Ownership model:
      - The authoritative state (position, playing, loop) is owned by the audio
        thread. Only the audio thread calls the mutators and advance().
      - The audio thread publishes a snapshot into atomics after every block; the
        UI thread reads those via the *ForUI() accessors. This is the same
        message-passing discipline the whole engine follows.

    No JUCE dependency — the loop/position math is unit-tested headless.
*/
class Transport
{
public:
    void prepare(double sampleRate) noexcept
    {
        tempoMap_.setSampleRate(sampleRate);
        publish();
    }

    // ---- audio thread only ----
    void setPlaying(bool p) noexcept                          { playing_ = p; }
    void setLooping(bool l) noexcept                          { looping_ = l; }
    void setLoopRegion(int64_t start, int64_t end) noexcept   { loopStart_ = start; loopEnd_ = end; }
    void setTempo(double bpm) noexcept                        { tempoMap_.setTempo(bpm); }
    void seek(int64_t s) noexcept                             { playhead_ = s < 0 ? 0 : s; publish(); }

    bool     isPlaying() const noexcept  { return playing_; }
    bool     isLooping() const noexcept  { return looping_; }
    int64_t  playhead() const noexcept   { return playhead_; }
    TempoMap&       tempoMap() noexcept        { return tempoMap_; }
    const TempoMap& tempoMap() const noexcept  { return tempoMap_; }

    /** Build the snapshot handed to nodes for a block of @p numSamples.

        Takes the block length because the musical span of a block is not a
        constant once the tempo map can hold more than one tempo - the map is
        evaluated at both edges here, once, rather than by every node. */
    TransportSnapshot snapshot(int numSamples) const noexcept
    {
        TransportSnapshot s;
        s.playing            = playing_;
        s.playheadSamples    = playhead_;
        s.timeSigNumerator   = tempoMap_.timeSigNumerator();
        s.timeSigDenominator = tempoMap_.timeSigDenominator();
        s.ppqPosition        = tempoMap_.ppqFromSamples(playhead_);
        s.ppqAtBlockEnd      = tempoMap_.ppqFromSamples(playhead_ + (int64_t) std::max(0, numSamples));

        // The tempo *in force at this block*, not "the" tempo — what the
        // things that legitimately want a scalar (tempo-synced effects,
        // automation stepping) should be following.
        s.bpm = tempoMap_.tempoAtBeat(s.ppqPosition);
        return s;
    }

    /** Advance the playhead by a rendered block, wrapping inside the loop region. */
    void advance(int numSamples) noexcept
    {
        if (playing_)
        {
            playhead_ += numSamples;

            if (looping_ && loopEnd_ > loopStart_ && playhead_ >= loopEnd_)
            {
                const int64_t len = loopEnd_ - loopStart_;
                playhead_ = loopStart_ + (playhead_ - loopStart_) % len;
            }
        }

        publish();
    }

    // ---- UI thread only ----
    int64_t playheadForUI() const noexcept { return uiPlayhead_.load(std::memory_order_relaxed); }
    bool    playingForUI() const noexcept  { return uiPlaying_.load(std::memory_order_relaxed); }

private:
    void publish() noexcept
    {
        uiPlayhead_.store(playhead_, std::memory_order_relaxed);
        uiPlaying_.store(playing_, std::memory_order_relaxed);
    }

    TempoMap tempoMap_;
    bool     playing_   = false;
    bool     looping_   = false;
    int64_t  playhead_  = 0;
    int64_t  loopStart_ = 0;
    int64_t  loopEnd_   = 0;

    std::atomic<int64_t> uiPlayhead_ { 0 };
    std::atomic<bool>    uiPlaying_  { false };
};

} // namespace looper::engine
