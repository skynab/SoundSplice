#pragma once

#include <array>

#include "engine/ShelfPeakFilter.h"

namespace soundsplice::engine
{
/**
    A 31-band graphic EQ: a third-octave-wide bell at each of the standard
    ISO centres from 20 Hz to 20 kHz, as Audacity's and every hardware 31-band
    offers. A band at 0 dB costs nothing. JUCE-free, like the 10-band one in
    DynamicsDsp.h; ThirdOctaveEqEffect.h runs it in a chain.
*/
class ThirdOctaveEq
{
public:
    static constexpr int                        kBands = 31;
    static constexpr std::array<float, kBands> kCentres {
        20.0f,   25.0f,   31.5f,   40.0f,   50.0f,   63.0f,   80.0f,   100.0f,  125.0f,  160.0f,  200.0f,
        250.0f,  315.0f,  400.0f,  500.0f,  630.0f,  800.0f,  1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f,
        3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f, 20000.0f
    };

    /** A third of an octave between the -3 dB points of a full boost. */
    static constexpr float kQ = 4.32f;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int b = 0; b < kBands; ++b)
        {
            auto& band = bands_[(size_t) b];
            band.prepare(sampleRate_);
            band.setShape(ShelfPeakFilter::Shape::Peaking);
            band.setFrequency(kCentres[(size_t) b]);
            band.setQ(kQ);
            band.setGainDb(gains_[(size_t) b]);
        }
    }

    void setGainDb(int band, float db)
    {
        if (band < 0 || band >= kBands || db == gains_[(size_t) band])
            return;
        if (gains_[(size_t) band] == 0.0f)
            bands_[(size_t) band].reset(); // no stale state from while it was skipped
        gains_[(size_t) band] = db;
        bands_[(size_t) band].setGainDb(db);
    }

    float processSample(float x) noexcept
    {
        for (int b = 0; b < kBands; ++b)
            if (active(b))
                x = bands_[(size_t) b].processSample(x);
        return x;
    }

    float magnitudeDbAt(float hz) const
    {
        float db = 0.0f;
        for (int b = 0; b < kBands; ++b)
            if (active(b))
                db += bands_[(size_t) b].magnitudeDbAt(hz);
        return db;
    }

private:
    /** Set away from 0 dB, and below where the sample rate can carry it. */
    bool active(int b) const noexcept
    {
        return gains_[(size_t) b] != 0.0f && kCentres[(size_t) b] < sampleRate_ * 0.45;
    }

    std::array<ShelfPeakFilter, kBands> bands_;
    std::array<float, kBands>           gains_ {};
    double                              sampleRate_ = 48000.0;
};

} // namespace soundsplice::engine
