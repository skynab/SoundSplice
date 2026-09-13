#pragma once

#include <algorithm>
#include <cmath>

namespace looper::engine
{
/**
    A 2-pole state-variable filter (Cytomic / TPT topology — stable, no
    per-sample division). Single channel; JUCE-free so its frequency response is
    unit-tested headlessly. LP/HP/BP outputs are selectable via the mode.
*/
class StateVariableFilter
{
public:
    enum class Mode { LowPass, HighPass, BandPass };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        reset();
        updateCoefficients();
    }

    void reset() noexcept { ic1eq_ = 0.0f; ic2eq_ = 0.0f; }

    void setMode(Mode mode) noexcept       { mode_ = mode; }
    void setCutoff(float hz)               { cutoff_ = hz; updateCoefficients(); }
    void setResonance(float q)             { resonance_ = q; updateCoefficients(); }

    float processSample(float v0) noexcept
    {
        const float v3 = v0 - ic2eq_;
        const float v1 = a1_ * ic1eq_ + a2_ * v3;
        const float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
        ic1eq_ = 2.0f * v1 - ic1eq_;
        ic2eq_ = 2.0f * v2 - ic2eq_;

        switch (mode_)
        {
            case Mode::LowPass:  return v2;
            case Mode::HighPass: return v0 - k_ * v1 - v2;
            case Mode::BandPass: return v1;
        }
        return v2;
    }

private:
    void updateCoefficients()
    {
        if (sampleRate_ <= 0.0)
            return;

        constexpr float pi = 3.14159265358979f;
        const float clampedCutoff = std::clamp(cutoff_, 20.0f, (float) (sampleRate_ * 0.49));
        const float g             = std::tan(pi * clampedCutoff / (float) sampleRate_);
        k_  = 1.0f / std::max(0.05f, resonance_);
        a1_ = 1.0f / (1.0f + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    double sampleRate_ = 0.0;
    float  cutoff_     = 1000.0f;
    float  resonance_  = 0.707f;
    Mode   mode_       = Mode::LowPass;

    float k_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1eq_ = 0.0f, ic2eq_ = 0.0f;
};

} // namespace looper::engine
