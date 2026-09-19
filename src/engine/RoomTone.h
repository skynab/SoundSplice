#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine
{
/**
    Room tone: the quiet a room makes when nobody's talking, captured from a
    passage of it and synthesized to any length, to fill the gaps an edit
    leaves so they don't drop to dead digital silence.

    The capture is the passage's average power at each frequency. The
    synthesis is noise with exactly that spectrum, built a frame at a time
    from those levels with random phases and overlap-added, so it has the
    room's colour and level but never repeats the way a looped recording
    would. JUCE-free, so its level and colour are tested headless.
*/
struct RoomToneProfile
{
    static constexpr int kSize = 2048;
    static constexpr int kHop  = kSize / 4;

    std::vector<double> power; // mean |X|^2 per bin of a Hann-windowed frame, kSize / 2 + 1 of them

    bool isEmpty() const noexcept { return power.empty(); }

    /** The profile of @p samples, or an empty one if they're shorter than a
        frame. */
    static RoomToneProfile capture(const std::vector<float>& samples)
    {
        RoomToneProfile profile;
        if ((int) samples.size() < kSize)
            return profile;

        std::vector<float> window((size_t) kSize), re((size_t) kSize), im((size_t) kSize);
        for (int i = 0; i < kSize; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / kSize));

        profile.power.assign((size_t) kSize / 2 + 1, 0.0);
        int frames = 0;
        for (size_t start = 0; start + (size_t) kSize <= samples.size(); start += (size_t) kHop, ++frames)
        {
            for (int i = 0; i < kSize; ++i)
            {
                re[(size_t) i] = samples[start + (size_t) i] * window[(size_t) i];
                im[(size_t) i] = 0.0f;
            }
            fft::transform(re, im, false);
            for (size_t k = 0; k < profile.power.size(); ++k)
                profile.power[k] += (double) re[k] * re[k] + (double) im[k] * im[k];
        }
        for (auto& p : profile.power)
            p /= std::max(1, frames);
        profile.power[0] = 0.0; // no DC: a room has none, and it would thump at the edges
        return profile;
    }
};

/** Streams room tone with @p profile's spectrum, a sample at a time. */
class RoomToneSynth
{
public:
    static constexpr int kSize = RoomToneProfile::kSize;
    static constexpr int kHop  = RoomToneProfile::kHop;

    RoomToneSynth(const RoomToneProfile& profile, std::uint64_t seed = 0x2545F4914F6CDD1DULL)
        : seed_(seed | 1u),
          window_((size_t) kSize),
          re_((size_t) kSize),
          im_((size_t) kSize),
          output_((size_t) kSize, 0.0f)
    {
        for (int i = 0; i < kSize; ++i)
            window_[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / kSize));

        // A bin's measured power is the noise's power times the analysis
        // window's energy (3N/8); a frame built from it and overlap-added
        // under the same window at a quarter hop carries 1.5/N of it per
        // sample: so the room's level needs 4/3 in amplitude.
        magnitude_.resize(profile.power.size());
        for (size_t k = 0; k < profile.power.size(); ++k)
            magnitude_[k] = (float) std::sqrt(std::max(0.0, profile.power[k]));

        // Primed, so the first sample out is already at full level rather
        // than fading up over the first frame.
        for (int i = 0; i < kSize / kHop; ++i)
            addFrame();
        position_ = 0;
    }

    float next() noexcept
    {
        if (magnitude_.empty())
            return 0.0f;
        const float out = output_[(size_t) position_] * kScale;
        if (++position_ == kHop)
        {
            position_ = 0;
            addFrame();
        }
        return out;
    }

private:
    static constexpr float kScale = 4.0f / 3.0f;

    double random() noexcept // [0, 1)
    {
        seed_ ^= seed_ >> 12;
        seed_ ^= seed_ << 25;
        seed_ ^= seed_ >> 27;
        return (double) ((seed_ * 0x2545F4914F6CDD1DULL) >> 11) / (double) (1ULL << 53);
    }

    /** Shifts a hop out of the output and overlap-adds a new frame. */
    void addFrame() noexcept
    {
        std::copy(output_.begin() + kHop, output_.end(), output_.begin());
        std::fill(output_.end() - kHop, output_.end(), 0.0f);

        const int bins = (int) magnitude_.size();
        for (int k = 0; k < bins; ++k)
        {
            const double phase = 2.0 * fft::kPi * random();
            re_[(size_t) k]    = (float) (magnitude_[(size_t) k] * std::cos(phase));
            im_[(size_t) k]    = (float) (magnitude_[(size_t) k] * std::sin(phase));
        }
        im_[0]                  = 0.0f;
        im_[(size_t) kSize / 2] = 0.0f;
        for (int k = 1; k < kSize / 2; ++k)
        {
            re_[(size_t) (kSize - k)] = re_[(size_t) k];
            im_[(size_t) (kSize - k)] = -im_[(size_t) k];
        }
        fft::transform(re_, im_, true);

        for (int i = 0; i < kSize; ++i)
            output_[(size_t) i] += re_[(size_t) i] * window_[(size_t) i];
    }

    std::uint64_t      seed_;
    std::vector<float> window_, re_, im_, output_, magnitude_;
    int                position_ = 0;
};

} // namespace soundsplice::engine
