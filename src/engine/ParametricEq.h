#pragma once

#include <array>
#include <cmath>

#include "engine/ShelfPeakFilter.h"

namespace soundsplice::engine
{
/**
    A parametric EQ (Audacity's Filter Curve, REAPER's ReaEQ): six bands in
    series, each a bell, a shelf, a notch or a cut at its own frequency, with
    its own gain and width. A band that's Off costs nothing, so the six cover
    anything from a single notch to a full tone-shaping curve. JUCE-free, so
    its curve is tested headless; ParametricEqEffect.h runs it in a chain.
*/
struct ParametricBand
{
    /** The values are stored in projects, so they are part of that format. */
    enum class Type
    {
        Off       = 0,
        Bell      = 1,
        LowShelf  = 2,
        HighShelf = 3,
        Notch     = 4,
        LowCut    = 5,
        HighCut   = 6
    };

    Type  type   = Type::Off;
    float hz     = 1000.0f;
    float gainDb = 0.0f; // bells and shelves only
    float q      = 1.0f; // bells, notches and cuts

    /** Whether the band's gain means anything: a notch or a cut has none. */
    static bool hasGain(Type type) noexcept
    {
        return type == Type::Bell || type == Type::LowShelf || type == Type::HighShelf;
    }

    bool operator==(const ParametricBand&) const = default;
};

class ParametricEq
{
public:
    static constexpr int kBands = 6;
    using Bands                 = std::array<ParametricBand, kBands>;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int b = 0; b < kBands; ++b)
        {
            filters_[(size_t) b].prepare(sampleRate_);
            configure(b);
        }
    }

    void reset()
    {
        for (auto& filter : filters_)
            filter.reset();
    }

    void setBand(int index, const ParametricBand& band)
    {
        if (index < 0 || index >= kBands || band == bands_[(size_t) index])
            return;
        const bool wasOff       = bands_[(size_t) index].type == ParametricBand::Type::Off;
        bands_[(size_t) index] = band;
        if (wasOff)
            filters_[(size_t) index].reset(); // no stale state from before it was off
        configure(index);
    }

    const Bands& bands() const noexcept { return bands_; }

    float processSample(float x) noexcept
    {
        for (int b = 0; b < kBands; ++b)
            if (bands_[(size_t) b].type != ParametricBand::Type::Off)
                x = filters_[(size_t) b].processSample(x);
        return x;
    }

    /** The whole curve's gain at @p hz, in dB, from the filters themselves. */
    float magnitudeDbAt(float hz) const
    {
        float db = 0.0f;
        for (int b = 0; b < kBands; ++b)
            if (bands_[(size_t) b].type != ParametricBand::Type::Off)
                db += filters_[(size_t) b].magnitudeDbAt(hz);
        return db;
    }

private:
    void configure(int index)
    {
        const auto& band   = bands_[(size_t) index];
        auto&       filter = filters_[(size_t) index];
        using Shape        = ShelfPeakFilter::Shape;

        switch (band.type)
        {
            case ParametricBand::Type::Bell:      filter.setShape(Shape::Peaking);   break;
            case ParametricBand::Type::LowShelf:  filter.setShape(Shape::LowShelf);  break;
            case ParametricBand::Type::HighShelf: filter.setShape(Shape::HighShelf); break;
            case ParametricBand::Type::Notch:     filter.setShape(Shape::Notch);     break;
            case ParametricBand::Type::LowCut:    filter.setShape(Shape::HighPass);  break;
            case ParametricBand::Type::HighCut:   filter.setShape(Shape::LowPass);   break;
            case ParametricBand::Type::Off:       return;
        }
        filter.setFrequency(band.hz);
        filter.setGainDb(ParametricBand::hasGain(band.type) ? band.gainDb : 0.0f);
        filter.setQ(band.q);
    }

    std::array<ShelfPeakFilter, kBands> filters_;
    Bands                               bands_ {};
    double                              sampleRate_ = 48000.0;
};

/** The curve @p bands make, at @p hz, in dB: for drawing without audio. */
inline float parametricMagnitudeDb(const ParametricEq::Bands& bands, float hz, double sampleRate = 48000.0)
{
    ParametricEq eq;
    eq.prepare(sampleRate);
    for (int b = 0; b < ParametricEq::kBands; ++b)
        eq.setBand(b, bands[(size_t) b]);
    return eq.magnitudeDbAt(hz);
}

} // namespace soundsplice::engine
