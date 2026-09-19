#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

namespace soundsplice::engine
{
/**
    A channel mixer (Audition's Channel Mixer, REAPER's routing matrix, and
    the mid/side half of Stereo Tools): each output channel as a mix of the
    two inputs, each amount from -2 to +2 (below 0 inverted), so it can swap sides,
    fold to mono, keep one side, or flip one's polarity. Mid/side conversion
    wraps the matrix: encoding turns left and right into mid (L+R)/2 and side
    (L-R)/2 before it, decoding turns mid and side back into left and right
    after it, so with the matrix scaling mid and side a mix can be made wider,
    narrower or mono in the bass-safe way mastering engineers do.
*/
namespace channelmixer
{
    enum class MidSide
    {
        Off    = 0,
        Encode = 1, // left/right in, mid (left out) and side (right out) out
        Decode = 2, // mid (left in) and side (right in) in, left/right out
        Around = 3  // encoded, mixed as mid and side, decoded: the matrix works on M/S
    };

    struct Matrix
    {
        float leftToLeft   = 1.0f;
        float rightToLeft  = 0.0f;
        float leftToRight  = 0.0f;
        float rightToRight = 1.0f;
        MidSide midSide    = MidSide::Off;
    };

    /** One frame, in place. */
    inline void process(float& left, float& right, const Matrix& m) noexcept
    {
        float a = left, b = right;
        if (m.midSide == MidSide::Encode || m.midSide == MidSide::Around)
        {
            const float mid = 0.5f * (a + b), side = 0.5f * (a - b);
            a = mid;
            b = side;
        }

        const float outA = m.leftToLeft * a + m.rightToLeft * b;
        const float outB = m.leftToRight * a + m.rightToRight * b;
        a = outA;
        b = outB;

        if (m.midSide == MidSide::Decode || m.midSide == MidSide::Around)
        {
            const float l = a + b, r = a - b;
            a = l;
            b = r;
        }
        left  = a;
        right = b;
    }
}

/** The channel mixer as a chain node. A mono buffer is left alone. */
class ChannelMixerEffect
{
public:
    void prepare(double, int) {}

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

    void setMatrix(const channelmixer::Matrix& m)
    {
        ll_.store(m.leftToLeft, std::memory_order_relaxed);
        rl_.store(m.rightToLeft, std::memory_order_relaxed);
        lr_.store(m.leftToRight, std::memory_order_relaxed);
        rr_.store(m.rightToRight, std::memory_order_relaxed);
        midSide_.store((int) m.midSide, std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed) || buffer.getNumChannels() < 2)
            return;

        channelmixer::Matrix m;
        m.leftToLeft   = ll_.load(std::memory_order_relaxed);
        m.rightToLeft  = rl_.load(std::memory_order_relaxed);
        m.leftToRight  = lr_.load(std::memory_order_relaxed);
        m.rightToRight = rr_.load(std::memory_order_relaxed);
        m.midSide      = (channelmixer::MidSide) juce::jlimit(0, 3, midSide_.load(std::memory_order_relaxed));

        auto* left  = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int n = 0; n < buffer.getNumSamples(); ++n)
            channelmixer::process(left[n], right[n], m);
    }

private:
    std::atomic<bool>  enabled_ { false };
    std::atomic<float> ll_ { 1.0f }, rl_ { 0.0f }, lr_ { 0.0f }, rr_ { 1.0f };
    std::atomic<int>   midSide_ { 0 };
};

} // namespace soundsplice::engine
