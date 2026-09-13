#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"
#include "engine/NoiseReduction.h"

namespace looper::engine
{
/**
    An averaged magnitude spectrum of a passage of audio, in dBFS.

    What it's for: seeing what's actually in a recording before deciding what
    to do about it. Mains hum, a room resonance, a whistle from a bad mic
    cable — all of them are obvious as a spike here and nearly impossible to
    place by ear, and all of them are things you'd then reach for the
    mastering EQ to remove.

    Averaged over overlapping frames rather than taken from one: a single
    frame of a real recording is dominated by whatever happened to be in that
    23ms, so the peaks move every time you look. Averaging gives the stable
    picture that's actually useful for finding a resonance.

    Calibrated so a full-scale sine reads 0dBFS at its own bin, which is what
    makes the numbers on the axis mean something — see the normalisation in
    analyse(), and the test that pins it.

    JUCE-free, so the calibration is measured headlessly rather than trusted.
*/
struct Spectrum
{
    /** One entry per bin, `fftSize / 2 + 1` of them, in dBFS. */
    std::vector<float> magnitudesDb;
    double             sampleRate = 0.0;
    int                fftSize    = 0;

    bool isEmpty() const { return magnitudesDb.empty() || sampleRate <= 0.0 || fftSize <= 0; }

    /** The frequency bin @p index is centred on. */
    double frequencyForBin(int index) const
    {
        if (fftSize <= 0)
            return 0.0;
        return (double) index * sampleRate / (double) fftSize;
    }

    /** The level at @p hz, interpolated between the two neighbouring bins so
        a curve drawn from this is smooth rather than stepped. */
    float magnitudeDbAtHz(double hz) const
    {
        if (isEmpty() || hz < 0.0)
            return kFloorDb;

        const double bin = hz * (double) fftSize / sampleRate;
        const int    low = (int) bin;
        if (low < 0 || low >= (int) magnitudesDb.size())
            return kFloorDb;

        const int high = std::min(low + 1, (int) magnitudesDb.size() - 1);
        const auto fraction = (float) (bin - (double) low);

        return magnitudesDb[(size_t) low]
             + (magnitudesDb[(size_t) high] - magnitudesDb[(size_t) low]) * fraction;
    }

    /** The loudest frequency present — what a hum or a resonance shows up as. */
    double dominantFrequency() const
    {
        if (isEmpty())
            return 0.0;

        int best = 0;
        for (int k = 1; k < (int) magnitudesDb.size(); ++k)
            if (magnitudesDb[(size_t) k] > magnitudesDb[(size_t) best])
                best = k;

        return frequencyForBin(best);
    }

    /** Anything quieter than this reads as silence. Deep enough to show a
        noise floor, shallow enough that log(0) never reaches the display. */
    static constexpr float kFloorDb = -120.0f;
};

namespace spectrum
{
    inline constexpr int kDefaultFftSize = 2048;

    /** The averaged spectrum of @p samples.

        Returns an empty result when there isn't a full frame to analyse —
        the honest answer for a selection too short to measure, rather than a
        spectrum of mostly zero padding. */
    inline Spectrum analyse(const std::vector<float>& samples, double sampleRate,
                            int fftSize = kDefaultFftSize)
    {
        Spectrum result;
        if (! fft::isPowerOfTwo((size_t) fftSize) || sampleRate <= 0.0
            || (int) samples.size() < fftSize)
            return result;

        const int  bins   = fftSize / 2 + 1;
        const int  hop    = fftSize / 4;
        const auto window = noisereduction::hannWindow(fftSize);

        std::vector<double> accumulated((size_t) bins, 0.0);
        int                 frames = 0;

        std::vector<float> re((size_t) fftSize), im((size_t) fftSize);

        for (int start = 0; start + fftSize <= (int) samples.size(); start += hop)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                re[(size_t) i] = samples[(size_t) (start + i)] * window[(size_t) i];
                im[(size_t) i] = 0.0f;
            }

            fft::transform(re, im, false);

            for (int k = 0; k < bins; ++k)
                accumulated[(size_t) k] += std::hypot((double) re[(size_t) k], (double) im[(size_t) k]);

            ++frames;
        }

        if (frames == 0)
            return result;

        // A Hann window has a coherent gain of 0.5, so a full-scale sine
        // sitting on a bin centre produces a magnitude of fftSize/4 there.
        // Dividing by that is what makes the axis read in dBFS rather than in
        // arbitrary units that change with the frame size.
        const double reference = (double) fftSize * 0.25;

        result.sampleRate = sampleRate;
        result.fftSize    = fftSize;
        result.magnitudesDb.resize((size_t) bins);

        for (int k = 0; k < bins; ++k)
        {
            const double mean = accumulated[(size_t) k] / (double) frames;
            const double db   = 20.0 * std::log10(std::max(mean / reference, 1.0e-12));
            result.magnitudesDb[(size_t) k] = (float) std::max(db, (double) Spectrum::kFloorDb);
        }

        return result;
    }
}

} // namespace looper::engine
