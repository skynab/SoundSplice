#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <vector>

namespace soundsplice::engine
{
/**
    Play-at-speed: the song rendered at its own rate and read back faster or
    slower, pitch and all, as a tape machine or Audacity's Play-at-Speed does.

    The engine renders the arrangement into this in ordinary blocks, as much as
    the next output block needs (framesNeeded), and pulls the output from it by
    linear interpolation. A fractional read position carries across blocks, so
    the output is continuous however the speed and the device's block size
    line up.

    Sized once in prepare and never allocating afterwards, since it runs on the
    audio thread. JUCE-free, so the arithmetic is tested on its own.
*/
class Varispeed
{
public:
    static constexpr double kMinSpeed = 0.25;
    static constexpr double kMaxSpeed = 4.0;

    static double clampSpeed(double speed)
    {
        return std::isfinite(speed) ? std::clamp(speed, kMinSpeed, kMaxSpeed) : 1.0;
    }

    /** The next of the speeds Play Faster and Play Slower step through from
        @p current, in @p direction (+1 faster, -1 slower); @p current itself
        at either end. A speed between steps goes to the nearest step that way. */
    static double steppedSpeed(double current, int direction)
    {
        static constexpr double steps[] { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0 };
        constexpr double        tolerance = 1.0e-6;

        if (direction > 0)
        {
            for (double step : steps)
                if (step > current + tolerance)
                    return step;
        }
        else if (direction < 0)
        {
            for (auto it = std::rbegin(steps); it != std::rend(steps); ++it)
                if (*it < current - tolerance)
                    return *it;
        }
        return clampSpeed(current);
    }

    /** Sizes the buffers for output blocks up to @p maxOutputFrames long, fed
        in input blocks up to @p maxInputBlock long. */
    void prepare(int channels, int maxOutputFrames, int maxInputBlock)
    {
        channels_  = std::max(1, channels);
        maxOutput_ = std::max(1, maxOutputFrames);
        capacity_  = (std::size_t) std::ceil(maxOutput_ * kMaxSpeed) + (std::size_t) std::max(1, maxInputBlock) + 4;
        buffer_.assign((std::size_t) channels_, std::vector<float>(capacity_, 0.0f));
        reset();
    }

    /** Drops everything buffered: after a seek, a stop, or a change back to
        normal speed, what's held is from somewhere the song no longer is. */
    void reset() noexcept
    {
        available_ = 0;
        position_  = 0.0;
    }

    int  channels() const noexcept { return channels_; }
    int  maxOutputFrames() const noexcept { return maxOutput_; }
    bool isPrepared() const noexcept { return capacity_ > 0; }

    /** How many more input frames must be pushed before @p outputFrames can be
        pulled at @p speed: every position read, and the frame after the last
        for interpolating towards. */
    int framesNeeded(int outputFrames, double speed) const noexcept
    {
        if (outputFrames <= 0)
            return 0;

        const double last  = position_ + (double) (outputFrames - 1) * clampSpeed(speed);
        const auto   wanted = (std::size_t) std::floor(last) + 2;
        return wanted > available_ ? (int) (wanted - available_) : 0;
    }

    /** Appends @p frames frames, one pointer per channel (a missing channel
        repeats the last one given). Returns how many fitted. */
    int push(const float* const* data, int dataChannels, int frames) noexcept
    {
        if (data == nullptr || dataChannels <= 0 || frames <= 0 || capacity_ == 0)
            return 0;

        const int room  = (int) (capacity_ - available_);
        const int count = std::min(frames, room);

        for (int ch = 0; ch < channels_; ++ch)
        {
            const auto* source = data[std::min(ch, dataChannels - 1)];
            auto&       target = buffer_[(std::size_t) ch];
            if (source == nullptr)
                std::fill_n(target.begin() + (std::ptrdiff_t) available_, count, 0.0f);
            else
                std::copy_n(source, count, target.begin() + (std::ptrdiff_t) available_);
        }

        available_ += (std::size_t) count;
        return count;
    }

    /** Writes @p outputFrames frames at @p speed into @p out (one pointer per
        channel), consuming what they read. Frames there isn't input for yet
        are written as silence rather than read past what's held. */
    void pull(float* const* out, int outChannels, int outputFrames, double speed) noexcept
    {
        if (out == nullptr || outChannels <= 0 || outputFrames <= 0)
            return;

        const double step = clampSpeed(speed);

        for (int ch = 0; ch < outChannels; ++ch)
        {
            auto* target = out[ch];
            if (target == nullptr)
                continue;

            const auto& source = buffer_[(std::size_t) std::min(ch, channels_ - 1)];
            double      at     = position_;

            for (int n = 0; n < outputFrames; ++n, at += step)
            {
                const auto index = (std::size_t) at;
                if (index + 1 >= available_)
                {
                    target[n] = 0.0f;
                    continue;
                }

                const auto fraction = (float) (at - (double) index);
                target[n] = source[index] + (source[index + 1] - source[index]) * fraction;
            }
        }

        // Drop the frames every later read is past.
        const double end      = position_ + (double) outputFrames * step;
        const auto   consumed = std::min((std::size_t) end, available_);

        if (consumed > 0)
        {
            for (auto& channel : buffer_)
                std::copy(channel.begin() + (std::ptrdiff_t) consumed,
                          channel.begin() + (std::ptrdiff_t) available_, channel.begin());
            available_ -= consumed;
        }

        position_ = std::max(0.0, end - (double) consumed);
    }

    std::size_t available() const noexcept { return available_; }

private:
    int                             channels_  = 0;
    int                             maxOutput_ = 0;
    std::size_t                     capacity_  = 0;
    std::size_t                     available_ = 0;
    double                          position_  = 0.0; // read position into buffer_, in frames
    std::vector<std::vector<float>> buffer_;
};

} // namespace soundsplice::engine
