#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Oscillator.h"

namespace soundsplice::engine
{
/**
    A channel vocoder (Audacity's Vocoder): a voice, the modulator, speaks
    through another sound, the carrier. Both are split into the same bank of
    bands; in each, the voice's level is followed and applied to the
    carrier's, and the bands are summed. The carrier's pitch and tone come
    out with the voice's words.

    The modulator is the left channel. The carrier is the right channel, as
    Audacity's is, or one made here: a bright sawtooth at a set pitch (the
    classic robot) or noise (a whisper). Bands are log-spaced from 80 Hz to
    10 kHz, each two band-passes deep so neighbours don't bleed into each
    other. Each carrier band is flattened to a steady level first, so the
    output follows the voice's level whatever the carrier's. JUCE-free, so
    which band follows which is tested headless.
*/
class Vocoder
{
public:
    enum class Carrier
    {
        RightChannel = 0,
        Sawtooth     = 1,
        Noise        = 2
    };

    static constexpr int    kMaxBands = 32;
    static constexpr double kLowestHz = 80.0;
    static constexpr double kHighestHz = 10000.0;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        configure();
    }

    void setBands(int count)           { count = std::clamp(count, 4, kMaxBands); if (count != bands_) { bands_ = count; configure(); } }
    void setCarrier(Carrier carrier)  { carrier_ = carrier; }
    void setPitchHz(double hz)        { pitchHz_ = std::clamp(hz, 20.0, 2000.0); }
    void setResponseMs(double ms)     { if (ms != responseMs_) { responseMs_ = std::max(1.0, ms); configure(); } }

    int bands() const noexcept { return bands_; }

    /** The centre of band @p band, in Hz. */
    double centreHz(int band) const noexcept
    {
        return kLowestHz * std::pow(kHighestHz / kLowestHz, (band + 0.5) / bands_);
    }

    /** One frame: @p modulator in, and @p carrierIn (the right channel, used
        only when that's the carrier); the vocoded sample out. */
    float process(float modulator, float carrierIn) noexcept
    {
        float carrier = carrierIn;
        if (carrier_ == Carrier::Sawtooth)
        {
            const double increment = pitchHz_ / sampleRate_;
            carrier = Oscillator::sample(Oscillator::Waveform::Saw, phase_, increment);
            phase_ += increment;
            phase_ -= std::floor(phase_);
        }
        else if (carrier_ == Carrier::Noise)
        {
            carrier = white();
        }

        double out = 0.0;
        for (int b = 0; b < bands_; ++b)
        {
            auto& band = bank_[(size_t) b];
            const double m = band.modulator[1].process(band.modulator[0].process(modulator));
            const double c = band.carrier[1].process(band.carrier[0].process(carrier));

            // Rises fast, falls at the response time: a consonant's attack
            // comes through, and the band doesn't flutter at its own pitch.
            const double level = std::abs(m);
            band.envelope += (level > band.envelope ? attack_ : release_) * (level - band.envelope);

            // The carrier's band flattened to a steady level, so what comes
            // out follows the voice's level alone, not the voice's times the
            // carrier's: a quiet carrier doesn't make a quiet vocoder.
            band.carrierLevel += carrierFollow_ * (std::abs(c) - band.carrierLevel);
            out += c / (band.carrierLevel + 1.0e-4) * band.envelope;
        }
        return (float) (out * gain_);
    }

private:
    struct Biquad
    {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

        double process(double x) noexcept
        {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return y;
        }

        /** The cookbook's band-pass, 0 dB at its centre. */
        void bandPass(double hz, double q, double rate)
        {
            const double w0    = 2.0 * 3.14159265358979323846 * std::min(hz, rate * 0.45) / rate;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0    = 1.0 + alpha;
            b0 = alpha / a0;
            b1 = 0.0;
            b2 = -alpha / a0;
            a1 = -2.0 * std::cos(w0) / a0;
            a2 = (1.0 - alpha) / a0;
        }
    };

    struct Band
    {
        std::array<Biquad, 2> modulator, carrier;
        double                envelope     = 0.0;
        double                carrierLevel = 0.0;
    };

    void configure()
    {
        // Each band an equal share of the octaves, its Q set so neighbours
        // meet near their -3 dB points.
        const double octaves = std::log2(kHighestHz / kLowestHz) / bands_;
        const double q       = std::sqrt(std::pow(2.0, octaves)) / (std::pow(2.0, octaves) - 1.0);
        for (int b = 0; b < bands_; ++b)
        {
            auto& band = bank_[(size_t) b];
            for (auto* chain : { &band.modulator, &band.carrier })
                for (auto& filter : *chain)
                    filter.bandPass(centreHz(b), q, sampleRate_);
        }

        attack_  = 1.0 - std::exp(-1.0 / (0.002 * sampleRate_));
        release_ = 1.0 - std::exp(-1.0 / (responseMs_ * 0.001 * sampleRate_));

        carrierFollow_ = 1.0 - std::exp(-1.0 / (0.01 * sampleRate_));

        // A flattened band carries its envelope's level times 1.25 (a
        // rectified signal averages 0.8 of its RMS), which about makes up
        // what the dips between neighbouring bands lose: so unity.
        gain_ = 1.0;
    }

    float white() noexcept
    {
        seed_ ^= seed_ >> 12;
        seed_ ^= seed_ << 25;
        seed_ ^= seed_ >> 27;
        return (float) ((double) ((seed_ * 0x2545F4914F6CDD1DULL) >> 11) / (double) (1ULL << 52) - 1.0);
    }

    std::array<Band, kMaxBands> bank_ {};
    int                         bands_      = 16;
    Carrier                     carrier_    = Carrier::Sawtooth;
    double                      sampleRate_ = 48000.0;
    double                      pitchHz_    = 110.0;
    double                      responseMs_ = 30.0;
    double                      attack_ = 0.0, release_ = 0.0, gain_ = 1.0, carrierFollow_ = 0.0;
    double                      phase_ = 0.0;
    std::uint64_t               seed_  = 0x9E3779B97F4A7C15ULL;
};

} // namespace soundsplice::engine
