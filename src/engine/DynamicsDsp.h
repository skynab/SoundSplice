#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/DelayLine.h"
#include "engine/PedalDsp.h"
#include "engine/ShelfPeakFilter.h"
#include "engine/StateVariableFilter.h"

namespace soundsplice::engine
{
/**
    The DSP of the Graphic EQ, De-esser, Expander, Ring Modulator and Wah-wah
    effects. JUCE-free, so each is tested headless; engine/DynamicsEffects.h
    wraps them as chain effects.
*/

namespace dsp
{
    /** One-pole coefficient reaching ~63% of a step in @p ms, the convention
        the times on a compressor mean. */
    inline float timeCoeff(float ms, double sampleRate) noexcept
    {
        const double samples = std::max(1.0, (double) ms * 0.001 * sampleRate);
        return (float) (1.0 - std::exp(-1.0 / samples));
    }

    inline float toDb(float linear) noexcept { return 20.0f * std::log10(std::max(linear, 1.0e-9f)); }
    inline float toGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
}

/** A ten-band graphic EQ: an octave-wide bell at each of 31 Hz to 16 kHz. */
class GraphicEq
{
public:
    static constexpr int                      kBands = 10;
    static constexpr std::array<float, kBands> kCentres { 31.25f, 62.5f, 125.0f, 250.0f, 500.0f,
                                                          1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int b = 0; b < kBands; ++b)
        {
            auto& band = bands_[(size_t) b];
            band.prepare(sampleRate_);
            band.setShape(ShelfPeakFilter::Shape::Peaking);
            band.setFrequency(kCentres[(size_t) b]);
            band.setQ(1.414f); // an octave wide
            band.setGainDb(gains_[(size_t) b]);
        }
    }

    void setGainDb(int band, float db)
    {
        if (band < 0 || band >= kBands || db == gains_[(size_t) band])
            return;
        gains_[(size_t) band] = db;
        bands_[(size_t) band].setGainDb(db);
    }

    float processSample(float x) noexcept
    {
        for (int b = 0; b < kBands; ++b)
            if (gains_[(size_t) b] != 0.0f && kCentres[(size_t) b] < sampleRate_ * 0.45)
                x = bands_[(size_t) b].processSample(x);
        return x;
    }

    float magnitudeDbAt(float hz) const
    {
        float db = 0.0f;
        for (int b = 0; b < kBands; ++b)
            if (gains_[(size_t) b] != 0.0f && kCentres[(size_t) b] < sampleRate_ * 0.45)
                db += bands_[(size_t) b].magnitudeDbAt(hz);
        return db;
    }

private:
    std::array<ShelfPeakFilter, kBands> bands_;
    std::array<float, kBands>           gains_ {};
    double                              sampleRate_ = 48000.0;
};

/**
    A split-band de-esser: the audio above a frequency is turned down while its
    level is over a threshold, by as much as it's over up to a limit, and the
    rest is left alone. The split is a fourth-order Linkwitz-Riley crossover,
    whose two bands add back to a flat response: a simpler split, the input
    less its high-pass, leaves a large out-of-phase share of a high tone in
    the "rest", and turning the high band down then barely turns it down.
*/
class DeEsser
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (auto& channel : splits_)
            for (int i = 0; i < 4; ++i)
            {
                auto& stage = channel[(size_t) i];
                stage.prepare(sampleRate_);
                stage.setMode(i < 2 ? StateVariableFilter::Mode::LowPass : StateVariableFilter::Mode::HighPass);
                stage.setCutoff(frequencyHz_);
                stage.setResonance(0.707f); // two Butterworths make a Linkwitz-Riley
            }
        attack_       = dsp::timeCoeff(0.5f, sampleRate_);
        release_      = dsp::timeCoeff(60.0f, sampleRate_);
        envelopeFall_ = 1.0f - release_;
        envelope_     = 0.0f;
        reductionDb_  = 0.0f;
    }

    void setFrequencyHz(float hz)
    {
        if (hz == frequencyHz_)
            return;
        frequencyHz_ = hz;
        for (auto& channel : splits_)
            for (auto& stage : channel)
                stage.setCutoff(hz);
    }

    void setThresholdDb(float db) noexcept    { thresholdDb_ = db; }
    void setMaxReductionDb(float db) noexcept { maxReductionDb_ = std::max(0.0f, db); }

    float currentReductionDb() const noexcept { return reductionDb_; }

    /** One stereo frame in place; one detector for both channels. */
    void processFrame(float* samples, int channels) noexcept
    {
        channels = std::clamp(channels, 0, 2);
        float lows[2] {}, highs[2] {};
        float level = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& stages = splits_[(size_t) ch];
            lows[ch]     = stages[1].processSample(stages[0].processSample(samples[ch]));
            highs[ch]    = stages[3].processSample(stages[2].processSample(samples[ch]));
            level        = std::max(level, std::abs(highs[ch]));
        }

        // A peak envelope, falling at the release rate: the level between a
        // waveform's peaks is the sound's level, not the zero it's crossing.
        envelope_ = std::max(level, envelope_ * envelopeFall_);

        const float over   = dsp::toDb(envelope_) - thresholdDb_;
        const float target = -std::clamp(over, 0.0f, maxReductionDb_);
        reductionDb_ += (target < reductionDb_ ? attack_ : release_) * (target - reductionDb_);

        const float gain = dsp::toGain(reductionDb_);
        for (int ch = 0; ch < channels; ++ch)
            samples[ch] = lows[ch] + highs[ch] * gain;
    }

private:
    std::array<std::array<StateVariableFilter, 4>, 2> splits_; // per channel: low, low, high, high
    double sampleRate_     = 48000.0;
    float  frequencyHz_    = 5500.0f;
    float  thresholdDb_    = -30.0f;
    float  maxReductionDb_ = 12.0f;
    float  attack_ = 0.0f, release_ = 0.0f, envelopeFall_ = 0.0f;
    float  envelope_    = 0.0f;
    float  reductionDb_ = 0.0f;
};

/**
    A downward expander, the gentle relative of a gate: below the threshold,
    every dB the signal falls becomes (ratio) dB, down to at most @p range of
    reduction. The level is a peak envelope that falls at the release rate, so
    the gain follows the sound rather than each waveform's zero crossings.
*/
class Expander
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        update();
        envelope_    = 0.0f;
        reductionDb_ = 0.0f;
    }

    void setThresholdDb(float db) noexcept { thresholdDb_ = db; }
    void setRatio(float ratio) noexcept    { ratio_ = std::max(1.0f, ratio); }
    void setRangeDb(float db) noexcept     { rangeDb_ = std::max(0.0f, db); }
    void setAttackMs(float ms)             { if (ms != attackMs_) { attackMs_ = ms; update(); } }
    void setReleaseMs(float ms)            { if (ms != releaseMs_) { releaseMs_ = ms; update(); } }

    float currentReductionDb() const noexcept { return reductionDb_; }

    void processFrame(float* samples, int channels) noexcept
    {
        float peak = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            peak = std::max(peak, std::abs(samples[ch]));

        envelope_ = std::max(peak, envelope_ * envelopeFall_);

        const float under  = thresholdDb_ - dsp::toDb(envelope_);
        const float target = under > 0.0f ? -std::min(rangeDb_, under * (ratio_ - 1.0f)) : 0.0f;

        // Opening back up is the attack; closing down is the release.
        reductionDb_ += (target > reductionDb_ ? attack_ : release_) * (target - reductionDb_);

        const float gain = dsp::toGain(reductionDb_);
        for (int ch = 0; ch < channels; ++ch)
            samples[ch] *= gain;
    }

private:
    void update()
    {
        attack_       = dsp::timeCoeff(attackMs_, sampleRate_);
        release_      = dsp::timeCoeff(releaseMs_, sampleRate_);
        envelopeFall_ = 1.0f - dsp::timeCoeff(std::max(1.0f, releaseMs_), sampleRate_);
    }

    double sampleRate_  = 48000.0;
    float  thresholdDb_ = -40.0f;
    float  ratio_       = 2.0f;
    float  rangeDb_     = 40.0f;
    float  attackMs_    = 5.0f;
    float  releaseMs_   = 100.0f;
    float  attack_ = 0.0f, release_ = 0.0f, envelopeFall_ = 0.0f;
    float  envelope_    = 0.0f;
    float  reductionDb_ = 0.0f;
};

/** Ring modulation: the input multiplied by a sine carrier, which replaces
    each frequency with its sum and difference with the carrier's. */
class RingModulator
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        phase_      = 0.0;
    }

    void setFrequencyHz(float hz) noexcept { frequencyHz_ = std::max(0.0f, hz); }
    void setMix(float mix) noexcept        { mix_ = std::clamp(mix, 0.0f, 1.0f); }

    /** The carrier for this frame; the same for every channel. */
    float nextCarrier() noexcept
    {
        const auto carrier = (float) std::sin(6.283185307179586 * phase_);
        phase_ += frequencyHz_ / sampleRate_;
        phase_ -= std::floor(phase_);
        return carrier;
    }

    float apply(float x, float carrier) const noexcept { return x * (1.0f - mix_) + x * carrier * mix_; }

private:
    double sampleRate_  = 48000.0;
    double phase_       = 0.0;
    float  frequencyHz_ = 440.0f;
    float  mix_         = 1.0f;
};

/** An auto-wah: a resonant band-pass swept between 350 Hz and up to about
    four octaves above by an LFO, normalised to unity at its peak. */
class Wah
{
public:
    static constexpr float kLowestHz = 350.0f;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        filter_.prepare(sampleRate_);
        filter_.setMode(StateVariableFilter::Mode::BandPass);
        filter_.setResonance(q_);
        filter_.setCutoff(kLowestHz);
    }

    void setPhase(double phase) noexcept    { phase_ = phase - std::floor(phase); }
    void setRateHz(float hz) noexcept       { rateHz_ = std::clamp(hz, 0.01f, 20.0f); }
    void setDepth(float depth) noexcept     { depth_ = std::clamp(depth, 0.0f, 1.0f); }
    void setMix(float mix) noexcept         { mix_ = std::clamp(mix, 0.0f, 1.0f); }

    void setResonance(float q)
    {
        q_ = std::clamp(q, 0.5f, 20.0f);
        filter_.setResonance(q_);
    }

    float processSample(float x) noexcept
    {
        const double lfo = 0.5 - 0.5 * std::cos(6.283185307179586 * phase_);
        phase_ += rateHz_ / sampleRate_;
        phase_ -= std::floor(phase_);

        filter_.setCutoff(kLowestHz * std::pow(2.0f, (float) lfo * depth_ * kOctaves));
        const float band = filter_.processSample(x) / q_; // the band output peaks at Q
        return x * (1.0f - mix_) + band * mix_;
    }

private:
    static constexpr float kOctaves = 4.0f;

    StateVariableFilter filter_;
    double sampleRate_ = 48000.0;
    double phase_      = 0.0;
    float  rateHz_     = 1.5f;
    float  depth_      = 0.8f;
    float  q_          = 4.0f;
    float  mix_        = 1.0f;
};

/**
    A multitap echo: several repeats of the input at a fixed spacing, each
    quieter than the last, as a tape echo's repeats are. Unlike the Delay
    effect, whose repeats come from feeding its output back in, every tap here
    is read from the same line, so the echoes keep the sound they started with
    rather than being filtered and smeared a little more each time. Ping-pong
    puts every other tap on the other side.
*/
class MultitapEcho
{
public:
    static constexpr int    kMaxTaps    = 8;
    static constexpr double kMaxDelayMs = 2000.0;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        const int size = (int) std::ceil(kMaxDelayMs * 0.001 * sampleRate_) * kMaxTaps + 4;
        for (auto& line : lines_)
            line.prepare(size);
    }

    void setTimeMs(float ms) noexcept   { timeMs_ = std::clamp(ms, 1.0f, (float) kMaxDelayMs); }
    void setTaps(int taps) noexcept     { taps_ = std::clamp(taps, 1, kMaxTaps); }
    void setDecay(float decay) noexcept { decay_ = std::clamp(decay, 0.0f, 0.95f); }
    void setMix(float mix) noexcept     { mix_ = std::clamp(mix, 0.0f, 1.0f); }
    void setPingPong(bool on) noexcept  { pingPong_ = on; }

    void processFrame(float* samples, int channels) noexcept
    {
        channels = std::clamp(channels, 0, 2);
        if (channels == 0)
            return;

        float wet[2] {};
        const double spacing = (double) timeMs_ * 0.001 * sampleRate_;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& line = lines_[(size_t) ch];
            line.processSampleFractional(samples[ch], 0.0, 0.0f); // writes; the taps read behind it

            float gain = decay_;
            for (int tap = 1; tap <= taps_; ++tap, gain *= decay_)
            {
                const float value = line.readFractional(spacing * tap);
                const int   side  = pingPong_ && channels > 1 && tap % 2 == 1 ? 1 - ch : ch;
                wet[side] += value * gain;
            }
        }

        for (int ch = 0; ch < channels; ++ch)
            samples[ch] = samples[ch] * (1.0f - mix_) + wet[ch] * mix_;
    }

private:
    std::array<DelayLine, 2> lines_;
    double sampleRate_ = 48000.0;
    float  timeMs_     = 250.0f;
    int    taps_       = 3;
    float  decay_      = 0.5f;
    float  mix_        = 0.35f;
    bool   pingPong_   = false;
};

/**
    A three-band compressor: the audio is split at two crossover frequencies,
    each band compressed on its own, and the three added back. Bass that
    pumps the whole mix down, or a harsh upper-middle, is handled where it is
    rather than by squashing everything.

    The splits are fourth-order Linkwitz-Riley, whose bands add back flat, so
    with nothing over its threshold the compressor is (to the ear) not there.
    Each band has one detector across both channels, as every other dynamics
    processor here does, so the stereo image doesn't move.
*/
class MultibandCompressor
{
public:
    static constexpr int kBands = 3;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        for (auto& channel : splits_)
            for (auto& stage : channel)
            {
                stage.prepare(sampleRate_);
                stage.setResonance(0.707f); // two Butterworths make a Linkwitz-Riley
            }

        for (auto& band : compressors_)
            band.prepare(sampleRate_);

        setCrossovers(lowHz_, highHz_);
    }

    void setCrossovers(float lowHz, float highHz)
    {
        lowHz_  = std::clamp(lowHz, 30.0f, 2000.0f);
        highHz_ = std::clamp(highHz, lowHz_ * 1.2f, (float) (sampleRate_ * 0.45));

        for (auto& channel : splits_)
        {
            channel[0].setMode(StateVariableFilter::Mode::LowPass);
            channel[1].setMode(StateVariableFilter::Mode::LowPass);
            channel[2].setMode(StateVariableFilter::Mode::HighPass);
            channel[3].setMode(StateVariableFilter::Mode::HighPass);
            channel[4].setMode(StateVariableFilter::Mode::LowPass);
            channel[5].setMode(StateVariableFilter::Mode::LowPass);
            channel[6].setMode(StateVariableFilter::Mode::HighPass);
            channel[7].setMode(StateVariableFilter::Mode::HighPass);

            for (int i = 0; i < 4; ++i)
                channel[(size_t) i].setCutoff(lowHz_);
            for (int i = 4; i < 8; ++i)
                channel[(size_t) i].setCutoff(highHz_);
        }
    }

    void setBand(int band, float thresholdDb, float ratio, float makeUpDb)
    {
        if (band < 0 || band >= kBands)
            return;

        compressors_[(size_t) band].setThresholdDb(thresholdDb);
        compressors_[(size_t) band].setRatio(ratio);
        makeUp_[(size_t) band] = dsp::toGain(makeUpDb);
    }

    void setAttackMs(float ms)
    {
        for (auto& band : compressors_)
            band.setAttackMs(ms);
    }

    void setReleaseMs(float ms)
    {
        for (auto& band : compressors_)
            band.setReleaseMs(ms);
    }

    /** How far band @p band is pulling down right now, in dB. */
    float bandReductionDb(int band) const noexcept
    {
        return band >= 0 && band < kBands ? compressors_[(size_t) band].currentReductionDb() : 0.0f;
    }

    void processFrame(float* samples, int channels) noexcept
    {
        channels = std::clamp(channels, 0, 2);
        if (channels == 0)
            return;

        float bands[kBands][2] {};
        float detector[kBands] {};

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& stages = splits_[(size_t) ch];

            const float low   = stages[1].processSample(stages[0].processSample(samples[ch]));
            const float rest  = stages[3].processSample(stages[2].processSample(samples[ch]));
            const float mid   = stages[5].processSample(stages[4].processSample(rest));
            const float high  = stages[7].processSample(stages[6].processSample(rest));

            bands[0][ch] = low;
            bands[1][ch] = mid;
            bands[2][ch] = high;

            for (int b = 0; b < kBands; ++b)
                detector[b] = std::max(detector[b], std::abs(bands[b][ch]));
        }

        float gains[kBands];
        for (int b = 0; b < kBands; ++b)
            gains[b] = compressors_[(size_t) b].gainFor(detector[b]) * makeUp_[(size_t) b];

        for (int ch = 0; ch < channels; ++ch)
        {
            float sum = 0.0f;
            for (int b = 0; b < kBands; ++b)
                sum += bands[b][ch] * gains[b];
            samples[ch] = sum;
        }
    }

private:
    // Per channel: two low and two high at the lower crossover, then two low
    // and two high at the upper one, which split what the first left.
    std::array<std::array<StateVariableFilter, 8>, 2> splits_;
    std::array<Compressor, kBands>                    compressors_;
    std::array<float, kBands>                         makeUp_ { 1.0f, 1.0f, 1.0f };
    double                                            sampleRate_ = 48000.0;
    float                                             lowHz_      = 200.0f;
    float                                             highHz_     = 3000.0f;
};

} // namespace soundsplice::engine
