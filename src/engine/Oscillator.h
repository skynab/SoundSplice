#pragma once

#include <cmath>

namespace looper::engine
{
/**
    Waveform generation for the synth's oscillator.

    JUCE-free so the anti-aliasing can be measured headlessly (see
    OscillatorTests, which correlates the output against frequencies that
    *shouldn't* be there) — the same reason SequencerMath, MetronomeMath and
    NoteOps are separated out. Aliasing is exactly the kind of defect that is
    hard to hear deliberately and easy to ship by accident.

    Saw and square use PolyBLEP: a naive ramp or step has a discontinuity once
    per cycle, whose infinite harmonic series folds back around Nyquist as
    inharmonic hash. PolyBLEP subtracts a polynomial approximation of a
    band-limited step across the two samples either side of each
    discontinuity, which removes most of that energy for a couple of
    multiplies per sample.

    Sine and triangle stay naive on purpose: a sine has no harmonics to alias
    at all, and a triangle's fall off as 1/n² (against a saw's 1/n), so its
    aliasing sits roughly 20 dB lower and isn't worth the extra work here.

    Phase is a normalised 0..1 position through the cycle, and `increment` is
    how far one sample advances it — i.e. frequency / sampleRate.
*/
struct Oscillator
{
    enum class Waveform { Sine, Saw, Square, Triangle };

    /** The PolyBLEP correction at normalised phase @p t, for a step
        discontinuity at t = 0. Zero except within one sample either side,
        which is why this costs almost nothing away from the edges. */
    static double polyBlep(double t, double increment) noexcept
    {
        if (increment <= 0.0)
            return 0.0;

        if (t < increment)                 // just after the step
        {
            const double x = t / increment;
            return x + x - x * x - 1.0;
        }
        if (t > 1.0 - increment)           // just before the next one
        {
            const double x = (t - 1.0) / increment;
            return x * x + x + x + 1.0;
        }
        return 0.0;
    }

    static float sample(Waveform waveform, double phase, double increment) noexcept
    {
        constexpr double twoPi = 6.283185307179586;

        switch (waveform)
        {
            case Waveform::Sine:
                return (float) std::sin(twoPi * phase);

            case Waveform::Saw:
                // A naive ramp minus the band-limited-step correction.
                return (float) (2.0 * phase - 1.0 - polyBlep(phase, increment));

            case Waveform::Square:
            {
                // Two discontinuities per cycle — one at 0, one at the
                // half-way point — so two corrections, with opposite signs.
                const double naive = phase < 0.5 ? 1.0 : -1.0;
                const double wrapped = phase < 0.5 ? phase + 0.5 : phase - 0.5;
                return (float) (naive + polyBlep(phase, increment) - polyBlep(wrapped, increment));
            }

            case Waveform::Triangle:
                return (float) (4.0 * std::abs(phase - 0.5) - 1.0);
        }
        return 0.0f;
    }
};

} // namespace looper::engine
