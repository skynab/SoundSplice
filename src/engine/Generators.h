#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "engine/Oscillator.h"

namespace soundsplice::engine
{
/**
    Audio made from nothing, as Audacity's Generate menu makes it: a tone, a
    chirp sweeping from one frequency and level to another, white, pink or
    brown noise, silence, and DTMF (telephone keypad) tones.

    A Generator renders one channel a chunk at a time, in order, so a long
    generation is written to disk without being held in memory; the same spec
    at the same rate always gives the same samples, noise included. JUCE-free,
    so each is tested headless.
*/
enum class GeneratorKind
{
    Tone,
    Chirp,
    Noise,
    Silence,
    Dtmf
};

enum class NoiseColour
{
    White,
    Pink,
    Brown
};

struct GeneratorSpec
{
    GeneratorKind        kind     = GeneratorKind::Tone;
    Oscillator::Waveform waveform = Oscillator::Waveform::Sine;

    double startHz        = 440.0; // a tone uses only the start values
    double endHz          = 1320.0;
    double startAmplitude = 0.8;   // 0..1
    double endAmplitude   = 0.1;
    bool   logarithmic    = false; // a chirp's sweep in octaves rather than hertz

    NoiseColour noise = NoiseColour::White;

    std::string dtmf      = "0123456789";
    double      dtmfDuty  = 0.55; // how much of each digit's slot is tone rather than the gap after it

    double seconds = 30.0;
};

class Generator
{
public:
    Generator(const GeneratorSpec& spec, double sampleRate)
        : spec_(spec),
          rate_(sampleRate > 0.0 ? sampleRate : 48000.0),
          total_((std::int64_t) std::llround(std::max(0.0, spec.seconds) * rate_))
    {
        if (spec_.kind == GeneratorKind::Dtmf)
            layOutDtmf();
    }

    /** How many frames the whole generation is. */
    std::int64_t totalFrames() const noexcept { return total_; }

    /** The next @p count frames into @p out; past the end, silence. */
    void render(float* out, int count) noexcept
    {
        for (int i = 0; i < count; ++i, ++frame_)
            out[i] = frame_ < total_ ? next() : 0.0f;
    }

    /** The DTMF keys this renders, in order: the spec's, with anything that
        isn't one left out. */
    static std::string dtmfKeys(const std::string& text)
    {
        std::string keys;
        for (char c : text)
            if (dtmfFrequencies((char) std::toupper((unsigned char) c)).first > 0.0)
                keys += (char) std::toupper((unsigned char) c);
        return keys;
    }

    /** The low and high frequencies of DTMF key @p key; zeros if it isn't one. */
    static std::pair<double, double> dtmfFrequencies(char key) noexcept
    {
        static constexpr const char* rows[] { "123A", "456B", "789C", "*0#D" };
        static constexpr double      low[]  { 697.0, 770.0, 852.0, 941.0 };
        static constexpr double      high[] { 1209.0, 1336.0, 1477.0, 1633.0 };

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (rows[r][c] == key)
                    return { low[r], high[c] };
        return { 0.0, 0.0 };
    }

private:
    static constexpr double kTwoPi = 6.283185307179586;

    float next() noexcept
    {
        const double t = total_ > 1 ? (double) frame_ / (double) (total_ - 1) : 0.0;

        switch (spec_.kind)
        {
            case GeneratorKind::Tone:
                return oscillator(spec_.startHz, spec_.startAmplitude);

            case GeneratorKind::Chirp:
            {
                const double hz = spec_.logarithmic && spec_.startHz > 0.0 && spec_.endHz > 0.0
                                    ? spec_.startHz * std::pow(spec_.endHz / spec_.startHz, t)
                                    : spec_.startHz + (spec_.endHz - spec_.startHz) * t;
                return oscillator(hz, spec_.startAmplitude + (spec_.endAmplitude - spec_.startAmplitude) * t);
            }

            case GeneratorKind::Noise:
                return (float) (clampAmplitude(spec_.startAmplitude) * colouredNoise());

            case GeneratorKind::Dtmf:
                return dtmfSample();

            case GeneratorKind::Silence:
                break;
        }
        return 0.0f;
    }

    static double clampAmplitude(double amplitude) noexcept { return std::clamp(amplitude, 0.0, 1.0); }

    float oscillator(double hz, double amplitude) noexcept
    {
        const double increment = std::clamp(hz, 0.0, rate_ * 0.5) / rate_;
        const float  value     = Oscillator::sample(spec_.waveform, phase_, increment);
        phase_ += increment;
        phase_ -= std::floor(phase_);
        return (float) (clampAmplitude(amplitude) * value);
    }

    /** Uniform in [-1, 1), from xorshift64*: fixed seed, so a generation repeats. */
    double white() noexcept
    {
        seed_ ^= seed_ >> 12;
        seed_ ^= seed_ << 25;
        seed_ ^= seed_ >> 27;
        const auto bits = (seed_ * 0x2545F4914F6CDD1DULL) >> 11;
        return (double) bits / (double) (1ULL << 52) - 1.0;
    }

    double colouredNoise() noexcept
    {
        const double w = white();
        switch (spec_.noise)
        {
            case NoiseColour::Pink:
            {
                // Paul Kellet's refined pink filter: within 0.05 dB of -3 dB
                // an octave above 9 Hz at 44.1 kHz. Scaled so its peaks sit
                // near those of white noise at the same amplitude.
                b_[0] = 0.99886 * b_[0] + w * 0.0555179;
                b_[1] = 0.99332 * b_[1] + w * 0.0750759;
                b_[2] = 0.96900 * b_[2] + w * 0.1538520;
                b_[3] = 0.86650 * b_[3] + w * 0.3104856;
                b_[4] = 0.55000 * b_[4] + w * 0.5329522;
                b_[5] = -0.7616 * b_[5] - w * 0.0168980;
                const double pink = b_[0] + b_[1] + b_[2] + b_[3] + b_[4] + b_[5] + b_[6] + w * 0.5362;
                b_[6] = w * 0.115926;
                return std::clamp(pink * 0.2, -1.0, 1.0);
            }

            case NoiseColour::Brown:
            {
                // Integrated white noise, leaking back towards zero so it
                // can't wander off: -6 dB an octave above a few hertz.
                brown_ = (brown_ + 0.02 * w) / 1.02;
                return std::clamp(brown_ * 3.5, -1.0, 1.0);
            }

            case NoiseColour::White:
                break;
        }
        return w;
    }

    /** Each key's tone, then a gap before the next: n tones and n - 1 gaps
        fill the duration, each tone dtmfDuty of a tone and its gap. */
    void layOutDtmf()
    {
        keys_ = dtmfKeys(spec_.dtmf);
        const auto   n    = (double) keys_.size();
        const double duty = std::clamp(spec_.dtmfDuty, 0.01, 1.0);
        if (n <= 0.0 || total_ <= 0)
            return;

        const double tone = (double) total_ * duty / (n * duty + (n - 1.0) * (1.0 - duty));
        toneFrames_       = std::max<std::int64_t>(1, (std::int64_t) std::floor(tone));
        slotFrames_       = std::max<std::int64_t>(toneFrames_, (std::int64_t) std::floor(tone / duty));
    }

    float dtmfSample() noexcept
    {
        if (keys_.empty() || slotFrames_ <= 0)
            return 0.0f;

        const auto key = (size_t) (frame_ / slotFrames_);
        const auto at  = frame_ % slotFrames_;
        if (key >= keys_.size() || at >= toneFrames_)
            return 0.0f;

        // A few milliseconds of raised-cosine fade at each end, so a key
        // starts and stops without a click.
        const auto   fade     = std::max<std::int64_t>(1, std::min<std::int64_t>(toneFrames_ / 4, (std::int64_t) (rate_ * 0.003)));
        const auto   edge     = std::min(at, toneFrames_ - 1 - at);
        const double envelope = edge >= fade ? 1.0 : 0.5 - 0.5 * std::cos(3.14159265358979323846 * (double) edge / (double) fade);

        const auto [low, high] = dtmfFrequencies(keys_[key]);
        const double seconds   = (double) at / rate_;
        const double value     = 0.5 * (std::sin(kTwoPi * low * seconds) + std::sin(kTwoPi * high * seconds));
        return (float) (clampAmplitude(spec_.startAmplitude) * envelope * value);
    }

    GeneratorSpec spec_;
    double        rate_;
    std::int64_t  total_;
    std::int64_t  frame_ = 0;

    double        phase_ = 0.0;
    std::uint64_t seed_  = 0x9E3779B97F4A7C15ULL;
    double        b_[7] {};
    double        brown_ = 0.0;

    std::string  keys_;
    std::int64_t toneFrames_ = 0;
    std::int64_t slotFrames_ = 0;
};

} // namespace soundsplice::engine
