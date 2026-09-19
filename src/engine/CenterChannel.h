#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"

namespace soundsplice::engine
{
/**
    Centre channel extraction (Audition's Center Channel Extractor, Audacity's
    Vocal Reduction and Isolation): what's panned dead centre in a stereo mix
    (usually the lead vocal, often the bass and kick too) taken out, for a
    backing track, or kept alone, for an a cappella.

    Frame by frame and bin by bin, the two channels are compared: the more
    alike they are, in level and in phase, the more of that bin is centre.
    2 Re(L R*) / (|L|^2 + |R|^2) is 1 for a sound identical in both, 0 for one
    in only one side, and below 0 for one out of phase between them. That
    likeness, sharpened, is the share of the bin's mid (L + R) / 2 taken as
    centre. Only bins between the given frequencies are touched, so the kick
    and the cymbals can be left where they are while the voice goes.

    JUCE-free, so what it removes and what it leaves is tested headless.
*/
namespace centre
{
    enum class Mode
    {
        Remove  = 0, // everything but the centre
        Isolate = 1  // the centre alone, in both channels
    };

    struct Settings
    {
        Mode   mode     = Mode::Remove;
        double strength = 1.0;     // 0 nothing, 1 all of it
        double lowHz    = 120.0;   // the band worked on
        double highHz   = 9000.0;
    };

    constexpr int kSize = 4096;
    constexpr int kHop  = kSize / 4;

    /** Processes @p left and @p right (the same length) in place. */
    inline void process(std::vector<float>& left, std::vector<float>& right, double sampleRate, const Settings& settings)
    {
        const size_t length = std::min(left.size(), right.size());
        if (length == 0 || sampleRate <= 0.0)
            return;

        // Padded a frame either side, so every sample is covered by the same
        // number of frames and the ends need no special case.
        const size_t       padded = length + 2 * (size_t) kSize;
        std::vector<float> inL(padded, 0.0f), inR(padded, 0.0f);
        std::copy(left.begin(), left.begin() + (long) length, inL.begin() + kSize);
        std::copy(right.begin(), right.begin() + (long) length, inR.begin() + kSize);
        std::vector<double> outL(padded, 0.0), outR(padded, 0.0);

        // Square-root Hann both ways: their product, a Hann, sums to 2 at a
        // quarter hop.
        std::vector<float> window((size_t) kSize);
        for (int i = 0; i < kSize; ++i)
            window[(size_t) i] = (float) std::sqrt(0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / kSize));

        const double binHz    = sampleRate / kSize;
        const double strength = std::clamp(settings.strength, 0.0, 1.0);
        std::vector<float> lr((size_t) kSize), li((size_t) kSize), rr((size_t) kSize), ri((size_t) kSize);

        for (size_t start = 0; start + (size_t) kSize <= padded; start += (size_t) kHop)
        {
            for (int i = 0; i < kSize; ++i)
            {
                lr[(size_t) i] = inL[start + (size_t) i] * window[(size_t) i];
                rr[(size_t) i] = inR[start + (size_t) i] * window[(size_t) i];
                li[(size_t) i] = ri[(size_t) i] = 0.0f;
            }
            fft::transform(lr, li, false);
            fft::transform(rr, ri, false);

            for (int k = 0; k <= kSize / 2; ++k)
            {
                const double hz     = k * binHz;
                const bool   inBand = hz >= settings.lowHz && hz <= settings.highHz;
                const double aRe = lr[(size_t) k], aIm = li[(size_t) k];
                const double bRe = rr[(size_t) k], bIm = ri[(size_t) k];

                const double energy = aRe * aRe + aIm * aIm + bRe * bRe + bIm * bIm;
                const double alike  = energy > 1.0e-18 ? std::max(0.0, 2.0 * (aRe * bRe + aIm * bIm) / energy) : 0.0;
                // Sharpened, so a bin has to be close to centre to count: a
                // wide stereo sound shares a little of each side by chance.
                // Out of the band, nothing is centre.
                const double share = inBand ? std::pow(alike, 4.0) : 0.0;
                const double cRe   = share * 0.5 * (aRe + bRe);
                const double cIm   = share * 0.5 * (aIm + bIm);

                double nlRe, nlIm, nrRe, nrIm;
                if (settings.mode == Mode::Isolate)
                {
                    // From the mix as it was (0) to the centre alone (1).
                    nlRe = strength * cRe + (1.0 - strength) * aRe;
                    nlIm = strength * cIm + (1.0 - strength) * aIm;
                    nrRe = strength * cRe + (1.0 - strength) * bRe;
                    nrIm = strength * cIm + (1.0 - strength) * bIm;
                }
                else
                {
                    nlRe = aRe - strength * cRe; nlIm = aIm - strength * cIm;
                    nrRe = bRe - strength * cRe; nrIm = bIm - strength * cIm;
                }

                lr[(size_t) k] = (float) nlRe; li[(size_t) k] = (float) nlIm;
                rr[(size_t) k] = (float) nrRe; ri[(size_t) k] = (float) nrIm;
                if (k > 0 && k < kSize / 2)
                {
                    lr[(size_t) (kSize - k)] = (float) nlRe; li[(size_t) (kSize - k)] = (float) -nlIm;
                    rr[(size_t) (kSize - k)] = (float) nrRe; ri[(size_t) (kSize - k)] = (float) -nrIm;
                }
            }

            fft::transform(lr, li, true);
            fft::transform(rr, ri, true);
            for (int i = 0; i < kSize; ++i)
            {
                outL[start + (size_t) i] += lr[(size_t) i] * window[(size_t) i] * 0.5;
                outR[start + (size_t) i] += rr[(size_t) i] * window[(size_t) i] * 0.5;
            }
        }

        for (size_t i = 0; i < length; ++i)
        {
            left[i]  = (float) outL[i + (size_t) kSize];
            right[i] = (float) outR[i + (size_t) kSize];
        }
    }
}

} // namespace soundsplice::engine
