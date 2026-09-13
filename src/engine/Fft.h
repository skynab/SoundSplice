#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace looper::engine
{
/**
    A small in-place radix-2 Cooley-Tukey FFT.

    Written here rather than linking juce_dsp for the reason every other bit
    of DSP in this engine is written here: it keeps the code JUCE-free, so
    the denoiser built on it stays in the headless test target where its
    claims can actually be measured. juce::dsp::FFT would pull the whole
    module in and push spectral subtraction into the GUI test target, which
    needs a windowing system to run.

    Iterative rather than recursive — bit-reversal permutation followed by
    log2(n) butterfly passes — so there's no per-call recursion depth or
    allocation beyond the twiddle it computes inline.

    Length must be a power of two. That's the only real constraint of radix-2,
    and the STFT that uses this picks its own frame size, so it's never an
    imposition on a caller who'd have preferred otherwise.
*/
namespace fft
{
    inline constexpr double kPi = 3.14159265358979323846;

    inline bool isPowerOfTwo(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

    /** Transforms @p real / @p imaginary in place.

        @p inverse runs the conjugate transform *and* divides by n, so
        `transform(x, true)` after `transform(x, false)` returns the original
        signal rather than the original scaled by n. Making the round trip an
        identity is worth more than matching any particular convention: it's
        the property every caller here relies on and the one the tests pin. */
    inline void transform(std::vector<float>& real, std::vector<float>& imaginary, bool inverse)
    {
        const size_t n = real.size();
        if (n < 2 || imaginary.size() != n || ! isPowerOfTwo(n))
            return;

        // Bit-reversal permutation.
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;

            if (i < j)
            {
                std::swap(real[i], real[j]);
                std::swap(imaginary[i], imaginary[j]);
            }
        }

        // Butterflies, doubling the block length each pass.
        for (size_t length = 2; length <= n; length <<= 1)
        {
            const double angle = 2.0 * kPi / (double) length * (inverse ? 1.0 : -1.0);
            const double wRe   = std::cos(angle);
            const double wIm   = std::sin(angle);

            for (size_t start = 0; start < n; start += length)
            {
                // The twiddle is stepped by repeated multiplication rather
                // than a cos/sin per butterfly: far fewer transcendentals,
                // and at these lengths the drift is well under float
                // precision anyway.
                double curRe = 1.0, curIm = 0.0;

                for (size_t k = 0; k < length / 2; ++k)
                {
                    const size_t a = start + k;
                    const size_t b = a + length / 2;

                    const double evenRe = real[a];
                    const double evenIm = imaginary[a];
                    const double oddRe  = real[b] * curRe - imaginary[b] * curIm;
                    const double oddIm  = real[b] * curIm + imaginary[b] * curRe;

                    real[a]      = (float) (evenRe + oddRe);
                    imaginary[a] = (float) (evenIm + oddIm);
                    real[b]      = (float) (evenRe - oddRe);
                    imaginary[b] = (float) (evenIm - oddIm);

                    const double nextRe = curRe * wRe - curIm * wIm;
                    curIm = curRe * wIm + curIm * wRe;
                    curRe = nextRe;
                }
            }
        }

        if (inverse)
        {
            const float scale = 1.0f / (float) n;
            for (size_t i = 0; i < n; ++i)
            {
                real[i]      *= scale;
                imaginary[i] *= scale;
            }
        }
    }
}

} // namespace looper::engine
