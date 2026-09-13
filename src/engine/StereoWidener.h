#pragma once

#include <algorithm>
#include <cmath>

namespace looper::engine
{
/**
    Mid/side stereo width.

        mid  = (L + R) / 2      side = (L - R) / 2
        L    = mid + side * w   R    = mid - side * w

    Everything both speakers share is the mid; everything they differ by is
    the side. Scaling only the side makes a mix wider or narrower without
    touching what's centred — which on a master bus is the whole point, since
    the kick, bass and lead vocal are usually the centred parts and are
    exactly what you don't want moved.

    **Mono compatibility is the reason this isn't just a multiply.** Summing
    to mono cancels the side signal entirely, so anything pushed into the
    sides gets quieter — or vanishes — on a phone speaker, a club PA's mono
    sub, or a Bluetooth speaker. A widener with no guard is the single easiest
    way to make a master that sounds impressive on headphones and hollow
    everywhere else.

    The guard here is a mid compensation: as the side is boosted, the mid is
    lifted enough that the mono sum (which is just the mid) holds its level
    instead of dropping away. `monoCompatibility()` reports the resulting
    mono-sum gain so a test — and a meter — can check it rather than trust it.

    JUCE-free, like the rest of this engine's DSP.
*/
class StereoWidener
{
public:
    /** 1 = unchanged (and bit-identical), 0 = mono, >1 = wider. */
    void setWidth(float width) noexcept { width_ = std::clamp(width, 0.0f, 2.0f); }

    float width() const noexcept { return width_; }

    /** Processes one stereo frame in place. */
    void processFrame(float& left, float& right) const noexcept
    {
        if (width_ == 1.0f)
            return; // exactly unity: leave the samples untouched, bit for bit

        const float mid  = 0.5f * (left + right);
        const float side = 0.5f * (left - right) * width_;

        const float compensated = mid * midCompensation();

        left  = compensated + side;
        right = compensated - side;
    }

    /** How loud the mono sum (L+R)/2 ends up relative to its input, at the
        current width. 1.0 means folding to mono costs nothing.

        Exposed rather than kept internal because "is this still mono-safe" is
        a claim worth testing and worth showing, not one to assert in a
        comment. */
    float monoCompatibility() const noexcept { return midCompensation(); }

private:
    /** Lifts the mid as the sides are widened, so the mono sum holds level.

        Deliberately mild — a full energy-preserving compensation would make
        wide settings sound centre-heavy in stereo, which is the opposite of
        what the control is for. This trades a little of that for a mono sum
        that stays within a decibel or so of unity across the useful range. */
    float midCompensation() const noexcept
    {
        return width_ <= 1.0f ? 1.0f : 1.0f + (width_ - 1.0f) * 0.5f;
    }

    float width_ = 1.0f;
};

} // namespace looper::engine
