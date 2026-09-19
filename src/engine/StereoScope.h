#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace soundsplice::engine
{
/**
    How the two channels of the master relate, for the phase correlation
    meter, the vectorscope and the oscilloscope in the Master pane (as
    Audition and every mastering suite show them).

    Correlation is the running average of L*R over the running averages of
    L^2 and R^2, about 300 ms long: +1 when the two sides are the same (mono),
    0 when they have nothing in common (wide, or one side silent), and -1
    when one is the other upside down, which would cancel when summed to mono.
    That last is the reason to watch it.

    The recent sample pairs are kept in a ring the audio thread writes and
    the UI copies from, each value an atomic so a copy taken mid-write is
    only ever a slightly stale picture, never a torn number. JUCE-free, so
    the meter's readings are tested headless.
*/
class PhaseCorrelation
{
public:
    void prepare(double sampleRate, double seconds = 0.3)
    {
        coefficient_ = 1.0 - std::exp(-1.0 / (std::max(1.0, sampleRate) * seconds));
        lr_ = ll_ = rr_ = 0.0;
    }

    void process(const float* left, const float* right, int frames) noexcept
    {
        for (int i = 0; i < frames; ++i)
        {
            const double l = left[i], r = right[i];
            lr_ += coefficient_ * (l * r - lr_);
            ll_ += coefficient_ * (l * l - ll_);
            rr_ += coefficient_ * (r * r - rr_);
        }
    }

    /** -1 to +1; 0 while either side is silent. */
    double correlation() const noexcept
    {
        const double power = std::sqrt(ll_ * rr_);
        return power > 1.0e-12 ? std::clamp(lr_ / power, -1.0, 1.0) : 0.0;
    }

private:
    double coefficient_ = 0.0;
    double lr_ = 0.0, ll_ = 0.0, rr_ = 0.0;
};

/** The last kSize (left, right) pairs, written by one thread and copied by
    another. */
class ScopeRing
{
public:
    static constexpr int kSize = 2048;

    void push(const float* left, const float* right, int frames) noexcept
    {
        int at = written_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i)
        {
            left_[(size_t) at].store(left[i], std::memory_order_relaxed);
            right_[(size_t) at].store(right[i], std::memory_order_relaxed);
            at = (at + 1) % kSize;
        }
        written_.store(at, std::memory_order_release);
    }

    /** The pairs, oldest first. */
    void copy(std::vector<std::pair<float, float>>& out) const
    {
        out.resize((size_t) kSize);
        const int newest = written_.load(std::memory_order_acquire);
        for (int i = 0; i < kSize; ++i)
        {
            const auto at = (size_t) ((newest + i) % kSize);
            out[(size_t) i] = { left_[at].load(std::memory_order_relaxed), right_[at].load(std::memory_order_relaxed) };
        }
    }

private:
    std::array<std::atomic<float>, kSize> left_ {}, right_ {};
    std::atomic<int>                       written_ { 0 };
};

} // namespace soundsplice::engine
