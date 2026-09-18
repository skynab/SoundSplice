#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/DynamicsDsp.h"

namespace soundsplice::engine
{
/**
    Audition's Dynamics Processing: the output level for each input level
    drawn as a curve, so one effect is a compressor, an expander, a gate, a
    limiter or any mix of them. The curve is a few points joined by straight
    lines in dB, level with the input (slope 1) beyond the first and last.
    JUCE-free, so the curve and what it does to audio are tested headless;
    DynamicsProcessorEffect.h runs it in a chain.
*/
struct TransferCurve
{
    static constexpr int   kMaxPoints = 6;
    static constexpr float kFloorDb   = -100.0f;

    struct Point
    {
        float inDb  = 0.0f;
        float outDb = 0.0f;
        bool operator==(const Point&) const = default;
    };

    std::array<Point, kMaxPoints> points { Point { -80.0f, -80.0f }, Point { 0.0f, 0.0f } };
    int                           count = 2;

    /** The points in use, in order of input level. */
    std::array<Point, kMaxPoints> sorted() const
    {
        auto result = points;
        std::sort(result.begin(), result.begin() + std::clamp(count, 1, kMaxPoints),
                  [](const Point& a, const Point& b) { return a.inDb < b.inDb; });
        return result;
    }

    float outputDb(float inDb) const
    {
        const int n  = std::clamp(count, 1, kMaxPoints);
        const auto p = sorted();
        if (inDb <= p[0].inDb)
            return p[0].outDb + (inDb - p[0].inDb);
        for (int i = 1; i < n; ++i)
            if (inDb <= p[(size_t) i].inDb)
            {
                const auto& a = p[(size_t) i - 1];
                const auto& b = p[(size_t) i];
                const float t = b.inDb > a.inDb ? (inDb - a.inDb) / (b.inDb - a.inDb) : 1.0f;
                return a.outDb + (b.outDb - a.outDb) * t;
            }
        return p[(size_t) n - 1].outDb + (inDb - p[(size_t) n - 1].inDb);
    }

    bool operator==(const TransferCurve&) const = default;
};

class DynamicsProcessor
{
public:
    enum class Detector
    {
        Peak = 0,
        Rms  = 1
    };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        update();
        envelope_ = 0.0f;
        gainDb_   = 0.0f;
    }

    void setCurve(const TransferCurve& curve) { curve_ = curve; }
    void setDetector(Detector detector) noexcept { detector_ = detector; }
    void setMakeUpDb(float db) noexcept { makeUpDb_ = db; }
    void setAttackMs(float ms)  { if (ms != attackMs_) { attackMs_ = ms; update(); } }
    void setReleaseMs(float ms) { if (ms != releaseMs_) { releaseMs_ = ms; update(); } }

    float currentGainDb() const noexcept { return gainDb_; }

    /** One frame of @p channels samples; one detector for all of them, so
        the stereo image stays put. */
    void processFrame(float* samples, int channels) noexcept
    {
        float level = 0.0f;
        if (detector_ == Detector::Rms)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                sum += samples[ch] * samples[ch];
            // A mean square followed over about 10 ms, then its root.
            meanSquare_ += rmsCoeff_ * (sum / (float) std::max(1, channels) - meanSquare_);
            level = std::sqrt(std::max(0.0f, meanSquare_));
        }
        else
        {
            for (int ch = 0; ch < channels; ++ch)
                level = std::max(level, std::abs(samples[ch]));
            envelope_ = std::max(level, envelope_ * envelopeFall_);
            level     = envelope_;
        }

        const float inDb   = std::max(TransferCurve::kFloorDb, dsp::toDb(level));
        const float target = std::max(TransferCurve::kFloorDb, curve_.outputDb(inDb)) - inDb;

        // Turning down is the attack; coming back up is the release.
        gainDb_ += (target < gainDb_ ? attack_ : release_) * (target - gainDb_);

        const float gain = dsp::toGain(gainDb_ + makeUpDb_);
        for (int ch = 0; ch < channels; ++ch)
            samples[ch] *= gain;
    }

private:
    void update()
    {
        attack_       = dsp::timeCoeff(std::max(0.05f, attackMs_), sampleRate_);
        release_      = dsp::timeCoeff(std::max(1.0f, releaseMs_), sampleRate_);
        envelopeFall_ = 1.0f - dsp::timeCoeff(std::max(1.0f, releaseMs_), sampleRate_);
        rmsCoeff_     = dsp::timeCoeff(10.0f, sampleRate_);
    }

    TransferCurve curve_;
    Detector      detector_   = Detector::Peak;
    double        sampleRate_ = 48000.0;
    float         makeUpDb_   = 0.0f;
    float         attackMs_   = 5.0f;
    float         releaseMs_  = 150.0f;
    float         attack_ = 0.0f, release_ = 0.0f, envelopeFall_ = 0.0f, rmsCoeff_ = 0.0f;
    float         envelope_   = 0.0f;
    float         meanSquare_ = 0.0f;
    float         gainDb_     = 0.0f;
};

} // namespace soundsplice::engine
