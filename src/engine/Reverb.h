#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace looper::engine
{
/**
    A Freeverb-style algorithmic reverb (8 parallel damped comb filters into 4
    series allpass filters, per channel, with a stereo spread). JUCE-free so the
    tail behaviour is unit-tested headlessly. Tunings are the classic Freeverb
    values, scaled to the running sample rate.
*/
class Reverb
{
public:
    void prepare(double sampleRate)
    {
        const double scale = sampleRate / 44100.0;
        for (int i = 0; i < numCombs; ++i)
        {
            combL_[i].setSize(scaled(combTuning_[i], scale));
            combR_[i].setSize(scaled(combTuning_[i] + stereoSpread, scale));
        }
        for (int i = 0; i < numAllpasses; ++i)
        {
            allpassL_[i].setSize(scaled(allpassTuning_[i], scale));
            allpassR_[i].setSize(scaled(allpassTuning_[i] + stereoSpread, scale));
            allpassL_[i].feedback = 0.5f;
            allpassR_[i].feedback = 0.5f;
        }
        update();
    }

    void setRoomSize(float v) { roomSize_ = v; update(); }
    void setDamping(float v)  { damping_  = v; update(); }
    void setWet(float v)      { wet_      = v; }
    void setDry(float v)      { dry_      = v; }
    void setWidth(float v)    { width_    = v; }

    void processStereo(float& left, float& right) noexcept
    {
        const float input = (left + right) * fixedGain;

        float outL = 0.0f, outR = 0.0f;
        for (int i = 0; i < numCombs; ++i)
        {
            outL += combL_[i].process(input);
            outR += combR_[i].process(input);
        }
        for (int i = 0; i < numAllpasses; ++i)
        {
            outL = allpassL_[i].process(outL);
            outR = allpassR_[i].process(outR);
        }

        const float wet1 = wet_ * (width_ * 0.5f + 0.5f);
        const float wet2 = wet_ * ((1.0f - width_) * 0.5f);

        left  = outL * wet1 + outR * wet2 + left * dry_;
        right = outR * wet1 + outL * wet2 + right * dry_;
    }

private:
    struct Comb
    {
        std::vector<float> buffer;
        int   index = 0;
        float feedback = 0.5f, damp = 0.5f, store = 0.0f;

        void setSize(int n) { buffer.assign((size_t) std::max(1, n), 0.0f); index = 0; store = 0.0f; }

        float process(float in) noexcept
        {
            const float out = buffer[(size_t) index];
            store = out * (1.0f - damp) + store * damp;
            buffer[(size_t) index] = in + store * feedback;
            if (++index >= (int) buffer.size())
                index = 0;
            return out;
        }
    };

    struct Allpass
    {
        std::vector<float> buffer;
        int   index = 0;
        float feedback = 0.5f;

        void setSize(int n) { buffer.assign((size_t) std::max(1, n), 0.0f); index = 0; }

        float process(float in) noexcept
        {
            const float bufout = buffer[(size_t) index];
            const float out    = -in + bufout;
            buffer[(size_t) index] = in + bufout * feedback;
            if (++index >= (int) buffer.size())
                index = 0;
            return out;
        }
    };

    static int scaled(int tuning, double scale) { return std::max(1, (int) std::round(tuning * scale)); }

    void update()
    {
        const float room = roomSize_ * 0.28f + 0.7f;
        const float damp = damping_ * 0.4f;
        for (int i = 0; i < numCombs; ++i)
        {
            combL_[i].feedback = room; combL_[i].damp = damp;
            combR_[i].feedback = room; combR_[i].damp = damp;
        }
    }

    static constexpr int   numCombs     = 8;
    static constexpr int   numAllpasses = 4;
    static constexpr int   stereoSpread = 23;
    static constexpr float fixedGain    = 0.015f;

    static constexpr int combTuning_[numCombs]        = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static constexpr int allpassTuning_[numAllpasses] = { 556, 441, 341, 225 };

    Comb    combL_[numCombs],       combR_[numCombs];
    Allpass allpassL_[numAllpasses], allpassR_[numAllpasses];

    float roomSize_ = 0.5f, damping_ = 0.5f, wet_ = 0.33f, dry_ = 0.7f, width_ = 1.0f;
};

} // namespace looper::engine
