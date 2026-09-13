#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Fft.h"

namespace looper::engine
{
/**
    Spectral-subtraction noise reduction — the "capture a noise print from a
    silent passage, then remove that from the whole recording" of a mastering
    workflow.

    The idea: steady background noise (hiss, hum, room tone, a computer fan)
    has a roughly constant spectrum, while speech and music do not. Measure
    that spectrum where nothing else is happening, then subtract it from
    every frame of the recording. What's left is what wasn't noise.

    Two decisions do most of the work in whether this sounds usable:

    **The spectral floor.** Subtracting all the way to zero is what produces
    "musical noise" — the warbling, underwater artefact that makes naive
    denoisers unusable. It happens because the noise estimate is an *average*,
    so in any given frame a bin's actual noise is above or below it; bins that
    subtract to nothing leave isolated surviving bins that flit around
    between frames and are heard as tones. Attenuating toward a floor instead
    of nulling keeps a quiet noise bed that masks them, which sounds far
    better than the "cleaner" alternative.

    **Weighted overlap-add.** Analysis and synthesis both window with Hann at
    75% overlap, and the output is divided by the accumulated squared window.
    That makes reconstruction exact when nothing is modified — so `reduceNoise`
    with an empty profile returns its input, which is both the right behaviour
    and a property worth testing.

    JUCE-free, so all of the above is measured headlessly rather than asserted
    in a comment.
*/
struct NoiseProfile
{
    /** Mean magnitude per FFT bin over the captured passage. Size is
        fftSize/2 + 1 — the real spectrum's unique half. */
    std::vector<float> magnitude;
    int                fftSize = 0;

    bool isEmpty() const { return magnitude.empty() || fftSize <= 0; }
};

namespace noisereduction
{
    inline constexpr int kDefaultFftSize = 1024;

    /** Hann window of length @p size, periodic (not symmetric) — the form
        that satisfies the overlap-add constancy the reconstruction relies
        on. */
    inline std::vector<float> hannWindow(int size)
    {
        std::vector<float> window((size_t) std::max(0, size));
        for (size_t i = 0; i < window.size(); ++i)
            window[i] = 0.5f - 0.5f * (float) std::cos(2.0 * fft::kPi * (double) i / (double) size);
        return window;
    }

    /**
        Averages the spectrum of @p samples — a passage the caller has
        identified as noise only.

        Mean rather than minimum or median: the mean is what spectral
        subtraction's arithmetic assumes, and using a lower statistic leaves
        a residue that the oversubtraction below then has to guess at.

        Returns an empty profile if there isn't at least one full frame, which
        is the honest answer for a selection too short to measure — better
        than a profile built from one zero-padded frame, which would describe
        the padding as much as the noise.
    */
    inline NoiseProfile captureNoiseProfile(const std::vector<float>& samples,
                                            int fftSize = kDefaultFftSize)
    {
        NoiseProfile profile;
        if (! fft::isPowerOfTwo((size_t) fftSize) || (int) samples.size() < fftSize)
            return profile;

        const int  bins   = fftSize / 2 + 1;
        const int  hop    = fftSize / 4;
        const auto window = hannWindow(fftSize);

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
            return profile;

        profile.fftSize = fftSize;
        profile.magnitude.resize((size_t) bins);
        for (int k = 0; k < bins; ++k)
            profile.magnitude[(size_t) k] = (float) (accumulated[(size_t) k] / (double) frames);

        return profile;
    }

    /**
        Subtracts @p profile from @p samples.

        @p reductionDb  how much more than the measured noise to subtract.
                        Higher removes more noise and more of the signal with
                        it; this is the control a user actually turns.
        @p floorDb      the most any bin may be attenuated (negative). This is
                        the musical-noise guard described above — at 0 the
                        effect is off, and at very negative values artefacts
                        return.

        Returns a buffer the same length as the input. An empty profile is a
        no-op rather than an error: the UI can only reach here after a
        capture, but a project reloaded mid-workflow shouldn't be able to
        silently destroy audio.
    */
    inline std::vector<float> reduceNoise(const std::vector<float>& samples,
                                          const NoiseProfile& profile,
                                          float reductionDb = 12.0f,
                                          float floorDb     = -18.0f)
    {
        if (profile.isEmpty() || samples.empty())
            return samples;

        const int fftSize = profile.fftSize;
        const int bins    = fftSize / 2 + 1;
        if (! fft::isPowerOfTwo((size_t) fftSize) || (int) profile.magnitude.size() != bins)
            return samples;

        const int   hop            = fftSize / 4;
        const auto  window         = hannWindow(fftSize);
        const float oversubtract   = std::pow(10.0f, std::max(0.0f, reductionDb) / 20.0f);
        const float floorGain      = std::pow(10.0f, std::min(0.0f, floorDb) / 20.0f);

        std::vector<float> output(samples.size(), 0.0f);
        std::vector<float> windowSum(samples.size(), 0.0f);
        std::vector<float> re((size_t) fftSize), im((size_t) fftSize);

        // Frames run past the end and are simply clipped when written back,
        // so the tail gets the same treatment as the body rather than being
        // left un-processed.
        for (int start = 0; start < (int) samples.size(); start += hop)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const int index = start + i;
                const float sample = index < (int) samples.size() ? samples[(size_t) index] : 0.0f;
                re[(size_t) i] = sample * window[(size_t) i];
                im[(size_t) i] = 0.0f;
            }

            fft::transform(re, im, false);

            for (int k = 0; k < bins; ++k)
            {
                const float real = re[(size_t) k];
                const float imag = im[(size_t) k];
                const float mag  = std::hypot(real, imag);
                if (mag <= 0.0f)
                    continue;

                const float noise = profile.magnitude[(size_t) k] * oversubtract;
                const float gain  = std::clamp((mag - noise) / mag, floorGain, 1.0f);

                re[(size_t) k] = real * gain;
                im[(size_t) k] = imag * gain;

                // The upper half mirrors the lower for a real signal. Keeping
                // it conjugate-symmetric is what makes the inverse come back
                // real; letting the two halves disagree produces an imaginary
                // residue that shows up as a harsh, metallic edge.
                if (k > 0 && k < fftSize - k)
                {
                    re[(size_t) (fftSize - k)] =  re[(size_t) k];
                    im[(size_t) (fftSize - k)] = -im[(size_t) k];
                }
            }

            fft::transform(re, im, true);

            for (int i = 0; i < fftSize; ++i)
            {
                const int index = start + i;
                if (index >= (int) samples.size())
                    break;

                const float w = window[(size_t) i];
                output[(size_t) index]    += re[(size_t) i] * w;
                windowSum[(size_t) index] += w * w;
            }
        }

        // Normalising by the accumulated squared window is what makes this
        // reconstruct exactly when no bin was changed. The floor is the other
        // half of getting it right: at the first and last hops only a
        // window's taper contributes, so windowSum is near zero there and
        // dividing by it amplifies rather than corrects. Flooring at a
        // fraction of the steady-state sum bounds that and lets the edges
        // taper, which is what partial data should do. (Same fix, and the
        // same reasoning, as TimeStretch's overlap-add.)
        const float steadyState = *std::max_element(windowSum.begin(), windowSum.end());
        const float floorValue  = std::max(1.0e-6f, steadyState * 0.1f);

        for (size_t i = 0; i < output.size(); ++i)
            output[i] /= std::max(windowSum[i], floorValue);

        return output;
    }
}

} // namespace looper::engine
