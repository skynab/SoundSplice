#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

#include <juce_graphics/juce_graphics.h>

#include "engine/Spectrogram.h"

namespace soundsplice
{
/**
    Drawing a spectrogram: the colours levels map to and the frequency axis.

    The frequency axis is logarithmic by default, from 20 Hz to the top of the
    audio: an octave takes the same height wherever it is, as it's heard,
    rather than the top octave taking half the view as it would on a linear
    axis. Linear (Audacity's default, best for harmonics, which it spaces
    evenly) and mel (pitch as heard, between the two) can be chosen. Levels
    run from dark (-100 dB and below) through blue and magenta to orange and
    white (0 dB), a scale that keeps quiet detail visible without the loud
    parts saturating.
*/
namespace spectrogramimage
{
    constexpr double kLowestHz = 20.0;
    constexpr float  kFloorDb  = -100.0f;

    /** How frequency is laid out up the view. The values are stored in the
        app's settings, so they are part of that format. */
    enum class Scale
    {
        Logarithmic = 0,
        Linear      = 1,
        Mel         = 2
    };

    inline double melOf(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
    inline double hzOfMel(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

    /** The frequency at @p proportion of the way up the axis (0 bottom, 1 top). */
    inline double frequencyAt(double proportion, double nyquist, Scale scale = Scale::Logarithmic)
    {
        const double p = std::clamp(proportion, 0.0, 1.0);
        switch (scale)
        {
            case Scale::Linear: return p * nyquist;
            case Scale::Mel:    return hzOfMel(p * melOf(nyquist));
            case Scale::Logarithmic:
            default:
            {
                const double top = std::max(kLowestHz * 2.0, nyquist);
                return kLowestHz * std::pow(top / kLowestHz, p);
            }
        }
    }

    /** How far up the axis @p hz sits (0 bottom, 1 top). */
    inline double proportionOf(double hz, double nyquist, Scale scale = Scale::Logarithmic)
    {
        switch (scale)
        {
            case Scale::Linear: return nyquist > 0.0 ? std::clamp(hz / nyquist, 0.0, 1.0) : 0.0;
            case Scale::Mel:
                return nyquist > 0.0 ? std::clamp(melOf(std::max(0.0, hz)) / melOf(nyquist), 0.0, 1.0) : 0.0;
            case Scale::Logarithmic:
            default:
            {
                const double top = std::max(kLowestHz * 2.0, nyquist);
                return std::log(std::max(hz, kLowestHz) / kLowestHz) / std::log(top / kLowestHz);
            }
        }
    }

    /** How levels are coloured: the brightest colour at @p -gainDb and
        below, down to black at @p rangeDb under it (Audacity's "gain" and
        "range"), so a quiet recording can be brought up to be seen. */
    struct Display
    {
        float rangeDb = -kFloorDb;
        float gainDb  = 0.0f;

        bool operator==(const Display& other) const noexcept
        {
            return rangeDb == other.rangeDb && gainDb == other.gainDb;
        }
    };

    inline juce::Colour colourFor(float levelDb, Display display = {})
    {
        static const juce::Colour stops[] {
            juce::Colour(0xff0a0a12), juce::Colour(0xff1b1e5c), juce::Colour(0xff6a1b9a),
            juce::Colour(0xffd84315), juce::Colour(0xffffb300), juce::Colour(0xfffff8e1),
        };
        constexpr int kStops = (int) std::size(stops);

        const float range    = std::max(1.0f, display.rangeDb);
        const float t        = std::clamp((levelDb + display.gainDb + range) / range, 0.0f, 1.0f) * (float) (kStops - 1);
        const int   lower    = std::min((int) t, kStops - 2);
        return stops[lower].interpolatedWith(stops[lower + 1], t - (float) lower);
    }

    /** What the healing brush has painted: round dabs on the spectrogram,
        each as wide in seconds and as tall in the view as the brush was on
        screen when it was painted. Kept in the view's own units (a fraction
        of its height, under the scale showing) so a dab covers the same
        frequencies the eye saw it cover. */
    struct Brush
    {
        struct Dab
        {
            double seconds    = 0.0;
            double proportion = 0.0; // up the view, 0 bottom to 1 top
        };

        static constexpr int kHarmonics = 12;

        std::vector<Dab> dabs;
        std::vector<Dab> outline;        // a lasso: the closed shape's edge, in place of dabs
        int              harmonics        = 1; // the harmonic brush: multiples of what's painted
        double           radiusSeconds    = 0.05;  // also a lasso's soft edge, as a brush's radius
        double           radiusProportion = 0.03;
        Scale            scale            = Scale::Logarithmic;
        double           nyquist          = 24000.0;

        bool isLasso() const noexcept { return outline.size() >= 3; }
        bool isEmpty() const noexcept { return dabs.empty() && ! isLasso(); }

        void clear()
        {
            dabs.clear();
            outline.clear();
        }

        /** How much of the point @p seconds into the clip, at @p hz, is
            covered: 1 well inside, falling to 0 over the outer edge so an
            edit has no hard boundary. The harmonic brush covers each
            multiple of what was painted as well, as a note's overtones follow
            its fundamental. */
        float amountAt(double seconds, double hz) const
        {
            if (isLasso())
                return lassoAmountAt(seconds, hz);

            float amount = 0.0f;
            for (int n = 1; n <= std::max(1, harmonics) && amount < 1.0f; ++n)
            {
                if (n > 1 && hz / n < kLowestHz)
                    break;
                const double p = proportionOf(hz / n, nyquist, scale);
                for (const auto& dab : dabs)
                {
                    const double dt = (seconds - dab.seconds) / radiusSeconds;
                    if (dt < -1.0 || dt > 1.0)
                        continue;
                    const double dp       = (p - dab.proportion) / radiusProportion;
                    const double distance = std::sqrt(dt * dt + dp * dp);
                    if (distance < 1.0)
                        amount = std::max(amount, (float) std::clamp((1.0 - distance) / 0.3, 0.0, 1.0));
                    if (amount >= 1.0f)
                        break;
                }
            }
            return amount;
        }

        /** The seconds the painting spans, a brush's edges included. */
        std::pair<double, double> timeSpan() const
        {
            const auto& points = isLasso() ? outline : dabs;
            const double pad    = isLasso() ? 0.0 : radiusSeconds;
            double from = points.empty() ? 0.0 : points.front().seconds, to = from;
            for (const auto& point : points)
            {
                from = std::min(from, point.seconds);
                to   = std::max(to, point.seconds);
            }
            return { from - pad, to + pad };
        }

    private:
        /** Inside the outline, ramping up from its edge over 0.3 of the
            brush's radius; the view's units scaled by that radius, so the
            ramp is as wide across as up. */
        float lassoAmountAt(double seconds, double hz) const
        {
            const double u = seconds / radiusSeconds;
            const double v = proportionOf(hz, nyquist, scale) / radiusProportion;

            bool   inside  = false;
            double nearest = 1.0e30;
            for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++)
            {
                const double ax = outline[j].seconds / radiusSeconds, ay = outline[j].proportion / radiusProportion;
                const double bx = outline[i].seconds / radiusSeconds, by = outline[i].proportion / radiusProportion;

                if ((by > v) != (ay > v) && u < (ax - bx) * (v - by) / (ay - by) + bx)
                    inside = ! inside;

                const double dx = ax - bx, dy = ay - by;
                const double length = dx * dx + dy * dy;
                const double t      = length > 0.0 ? std::clamp(((u - bx) * dx + (v - by) * dy) / length, 0.0, 1.0) : 0.0;
                nearest = std::min(nearest, std::hypot(u - (bx + t * dx), v - (by + t * dy)));
            }
            return inside ? (float) std::clamp(nearest / 0.3, 0.0, 1.0) : 0.0f;
        }
    };

    /** @p data as an image a column per column and @p rows high, row 0 at the
        top (the highest frequency). Each row shows the loudest bin it spans,
        so a narrow tone high up, where a row covers many bins, isn't lost. */
    inline juce::Image imageOf(const engine::SpectrogramData& data, int rows = 256, Scale scale = Scale::Logarithmic,
                               Display display = {})
    {
        if (data.isEmpty() || rows < 2)
            return {};

        juce::Image          image(juce::Image::RGB, data.columns, rows, false);
        juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
        const double         nyquist = data.sampleRate * 0.5;

        for (int row = 0; row < rows; ++row)
        {
            // The band of frequencies this row covers, as bins.
            const double top    = 1.0 - (double) row / (double) rows;
            const double bottom = 1.0 - (double) (row + 1) / (double) rows;
            const int    binLow = std::clamp((int) std::floor(data.binOfFrequency(frequencyAt(bottom, nyquist, scale))), 0, data.bins - 1);
            const int    binHigh = std::clamp((int) std::ceil(data.binOfFrequency(frequencyAt(top, nyquist, scale))), binLow, data.bins - 1);

            for (int column = 0; column < data.columns; ++column)
            {
                float level = engine::SpectrogramBuilder::kFloorDb;
                for (int bin = binLow; bin <= binHigh; ++bin)
                    level = std::max(level, data.levelDb(column, bin));
                pixels.setPixelColour(column, row, colourFor(level, display));
            }
        }

        return image;
    }
}

} // namespace soundsplice
