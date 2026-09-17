#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <vector>

namespace soundsplice::engine
{
/**
    Loudness as broadcast and streaming measure it: ITU-R BS.1770-4 and EBU
    R128. Momentary (400 ms), short-term (3 s) and gated integrated loudness in
    LUFS, loudness range (EBU Tech 3342) in LU, and true peak in dBTP.

    One implementation for everything that needs a loudness: the analyser's
    measurement of a selection, Normalize Loudness, and the live meter on the
    master bus. So it runs on the audio thread: prepare allocates, process
    never does.

    Integrated loudness and loudness range come from histograms of block
    loudness, 0.01 LU wide, each holding the exact energy of the blocks that
    fell in it, so a measurement of any length takes the same memory and the
    gating is exact to well within the 0.1 LU EBU Tech 3341 asks for.

    JUCE-free, so it's tested against the EBU's own test signals.
*/
class LoudnessMeter
{
public:
    static constexpr double kSilence = -std::numeric_limits<double>::infinity();

    /** Sizes everything for @p channels channels at @p sampleRate. */
    void prepare(double sampleRate, int channels)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_   = std::max(1, channels);
        hopLength_  = std::max(1, (int) std::lround(sampleRate_ * 0.1));

        filters_.assign((size_t) channels_, KWeighting(sampleRate_));
        hopSquares_.assign((size_t) channels_, 0.0);

        oversampling_ = sampleRate_ < 96000.0 ? 4 : sampleRate_ < 192000.0 ? 2 : 1;
        buildInterpolator();
        history_.assign((size_t) channels_, std::vector<float>((size_t) kTapsPerPhase, 0.0f));

        reset();
    }

    /** Starts measuring afresh. */
    void reset() noexcept
    {
        for (auto& filter : filters_)
            filter.reset();
        std::fill(hopSquares_.begin(), hopSquares_.end(), 0.0);
        for (auto& channel : history_)
            std::fill(channel.begin(), channel.end(), 0.0f);

        hops_.fill(0.0);
        hopCount_        = 0;
        hopFill_         = 0;
        historyAt_       = 0;
        blocks_.clear();
        shortTermBlocks_.clear();
        samplePeak_      = 0.0;
        truePeak_        = 0.0;
        maxMomentary_    = kSilence;
        maxShortTerm_    = kSilence;
    }

    double sampleRate() const noexcept { return sampleRate_; }
    int    channels() const noexcept { return channels_; }

    /** Measures @p numSamples more samples of @p numChannels channels. Extra
        channels beyond those prepared are ignored; missing ones count as silence. */
    void process(const float* const* data, int numChannels, int numSamples) noexcept
    {
        const int channels = std::min(numChannels, channels_);

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float  sample   = data[ch][i];
                const double weighted = filters_[(size_t) ch].process((double) sample);
                hopSquares_[(size_t) ch] += weighted * weighted;
                measurePeak(ch, sample);
            }
            historyAt_ = (historyAt_ + 1) % kTapsPerPhase;

            if (++hopFill_ == hopLength_)
                finishHop();
        }
    }

    /** Loudness of the last 400 ms; silence until that much has been measured. */
    double momentaryLufs() const noexcept { return hopCount_ >= 4 ? loudnessOf(meanOfLastHops(4)) : kSilence; }

    /** Loudness of the last 3 s; silence until that much has been measured. */
    double shortTermLufs() const noexcept { return hopCount_ >= 30 ? loudnessOf(meanOfLastHops(30)) : kSilence; }

    double maxMomentaryLufs() const noexcept { return maxMomentary_; }
    double maxShortTermLufs() const noexcept { return maxShortTerm_; }

    /** Gated integrated loudness of everything measured since the last reset:
        blocks under -70 LUFS are ignored, then those more than 10 LU under the
        loudness of the rest. Silence when nothing passes the gates. */
    double integratedLufs() const noexcept
    {
        return blocks_.gatedLoudness(-10.0);
    }

    /** Loudness range in LU (EBU Tech 3342): the spread between the 10th and
        95th percentiles of short-term loudness, gated at -70 LUFS and 20 LU
        under their own loudness. 0 until there are short-term values. */
    double loudnessRangeLu() const noexcept
    {
        const double gate = Histogram::loudnessOf(shortTermBlocks_.energyAbove(kAbsoluteGate),
                                                  shortTermBlocks_.countAbove(kAbsoluteGate));
        if (! std::isfinite(gate))
            return 0.0;

        const double relative = std::max(kAbsoluteGate, gate - 20.0);
        const double low      = shortTermBlocks_.percentile(relative, 0.10);
        const double high     = shortTermBlocks_.percentile(relative, 0.95);
        return std::isfinite(low) && std::isfinite(high) ? std::max(0.0, high - low) : 0.0;
    }

    /** The highest sample, in dBFS. */
    double samplePeakDb() const noexcept { return decibels(samplePeak_); }

    /** The highest point of the signal between samples too, as a DAC would
        reconstruct it, estimated by oversampling (BS.1770-4 annex 2), in dBTP. */
    double truePeakDb() const noexcept { return decibels(std::max(truePeak_, samplePeak_)); }

private:
    static constexpr double kAbsoluteGate = -70.0;
    static constexpr int    kTapsPerPhase = 16;

    //==========================================================================
    /** The BS.1770 K-weighting: a high shelf for the head's effect, then a
        high-pass (RLB). Coefficients derived for any rate, matching the
        standard's 48 kHz tables. */
    struct KWeighting
    {
        explicit KWeighting(double rate)
        {
            const double pi = 3.14159265358979323846;

            double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
            double k  = std::tan(pi * f0 / rate);
            const double vh = std::pow(10.0, gain / 20.0);
            const double vb = std::pow(vh, 0.4996667741545416);
            double a0 = 1.0 + k / q + k * k;
            shelf = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                      2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };

            f0 = 38.13547087602444;
            q  = 0.5003270373238773;
            k  = std::tan(pi * f0 / rate);
            a0 = 1.0 + k / q + k * k;
            highPass = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
        }

        struct Biquad
        {
            double b0, b1, b2, a1, a2;
            double z1 = 0.0, z2 = 0.0;

            double process(double x) noexcept
            {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };

        double process(double x) noexcept { return highPass.process(shelf.process(x)); }
        void   reset() noexcept { shelf.z1 = shelf.z2 = highPass.z1 = highPass.z2 = 0.0; }

        Biquad shelf {}, highPass {};
    };

    //==========================================================================
    /** Block loudness, binned 0.01 LU wide from -70 to +30 LUFS, with each
        bin's block count and exact total energy. */
    class Histogram
    {
    public:
        static constexpr int    kBins    = 10000;
        static constexpr double kLowest  = -70.0;
        static constexpr double kPerLu   = 100.0;

        void clear() noexcept
        {
            std::fill(counts_.begin(), counts_.end(), 0);
            std::fill(energy_.begin(), energy_.end(), 0.0);
        }

        void add(double energy) noexcept
        {
            const double loudness = LoudnessMeter::loudnessOf(energy);
            if (! (loudness > kLowest))
                return;
            const auto bin = (size_t) std::clamp((int) ((loudness - kLowest) * kPerLu), 0, kBins - 1);
            ++counts_[bin];
            energy_[bin] += energy;
        }

        std::uint64_t countAbove(double lufs) const noexcept
        {
            std::uint64_t count = 0;
            for (int bin = firstBinAbove(lufs); bin < kBins; ++bin)
                count += counts_[(size_t) bin];
            return count;
        }

        double energyAbove(double lufs) const noexcept
        {
            double energy = 0.0;
            for (int bin = firstBinAbove(lufs); bin < kBins; ++bin)
                energy += energy_[(size_t) bin];
            return energy;
        }

        /** The loudness of the mean of @p count blocks totalling @p energy. */
        static double loudnessOf(double energy, std::uint64_t count) noexcept
        {
            return count == 0 ? kSilence : LoudnessMeter::loudnessOf(energy / (double) count);
        }

        /** The loudness of the blocks above the absolute gate and then above
            @p relativeLu (negative) under their own loudness. */
        double gatedLoudness(double relativeLu) const noexcept
        {
            const double ungated = loudnessOf(energyAbove(kAbsoluteGate), countAbove(kAbsoluteGate));
            if (! std::isfinite(ungated))
                return kSilence;

            const double gate = std::max(kAbsoluteGate, ungated + relativeLu);
            return loudnessOf(energyAbove(gate), countAbove(gate));
        }

        /** The block loudness below which @p fraction of the blocks above
            @p lufs fall. */
        double percentile(double lufs, double fraction) const noexcept
        {
            const auto total = countAbove(lufs);
            if (total == 0)
                return kSilence;

            const auto    wanted = (std::uint64_t) std::floor(fraction * (double) (total - 1));
            std::uint64_t seen   = 0;
            for (int bin = firstBinAbove(lufs); bin < kBins; ++bin)
            {
                seen += counts_[(size_t) bin];
                if (seen > wanted)
                    return kLowest + ((double) bin + 0.5) / kPerLu;
            }
            return kSilence;
        }

    private:
        /** Bins wholly above @p lufs, so a gate "above" a level excludes a
            block exactly at it only to within a bin. */
        static int firstBinAbove(double lufs) noexcept
        {
            return std::clamp((int) std::ceil((lufs - kLowest) * kPerLu), 0, kBins);
        }

        // On the heap: the two are 120 KB, too much to put on a stack with
        // every meter.
        std::vector<std::uint32_t> counts_ = std::vector<std::uint32_t>(kBins, 0);
        std::vector<double>        energy_ = std::vector<double>(kBins, 0.0);
    };

    //==========================================================================
    static double loudnessOf(double energy) noexcept
    {
        return energy > 0.0 ? -0.691 + 10.0 * std::log10(energy) : kSilence;
    }

    static double decibels(double linear) noexcept
    {
        return linear > 0.0 ? 20.0 * std::log10(linear) : kSilence;
    }

    /** BS.1770 channel weights, for the L R C LFE Ls Rs order: surrounds count
        1.41 and the LFE not at all. */
    double channelWeight(int channel) const noexcept
    {
        if (channels_ < 6)
            return 1.0;
        return channel == 3 ? 0.0 : channel == 4 || channel == 5 ? 1.41 : 1.0;
    }

    void finishHop() noexcept
    {
        double energy = 0.0;
        for (int ch = 0; ch < channels_; ++ch)
        {
            energy += channelWeight(ch) * hopSquares_[(size_t) ch] / (double) hopLength_;
            hopSquares_[(size_t) ch] = 0.0;
        }

        hops_[(size_t) (hopCount_ % kHopsKept)] = energy;
        ++hopCount_;
        hopFill_ = 0;

        // A 400 ms block every 100 ms, overlapping by 75%, as BS.1770 gates.
        if (hopCount_ >= 4)
        {
            const double block = meanOfLastHops(4);
            blocks_.add(block);
            maxMomentary_ = std::max(maxMomentary_, loudnessOf(block));
        }

        if (hopCount_ >= 30)
        {
            const double shortTerm = meanOfLastHops(30);
            shortTermBlocks_.add(shortTerm);
            maxShortTerm_ = std::max(maxShortTerm_, loudnessOf(shortTerm));
        }
    }

    double meanOfLastHops(int count) const noexcept
    {
        double sum = 0.0;
        for (int i = 1; i <= count; ++i)
            sum += hops_[(size_t) ((hopCount_ - i) % kHopsKept)];
        return sum / count;
    }

    //==========================================================================
    /** A 4x (or 2x) interpolator: Kaiser-windowed sinc, kTapsPerPhase input
        samples per output point. */
    void buildInterpolator()
    {
        const int    length = kTapsPerPhase * oversampling_;
        const double centre = (length - 1) * 0.5;
        const double pi     = 3.14159265358979323846;
        const double beta   = 7.0;

        const auto besselI0 = [](double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 50; ++k)
            {
                term *= (x * 0.5 / k) * (x * 0.5 / k);
                sum += term;
            }
            return sum;
        };

        coefficients_.assign((size_t) length, 0.0f);
        for (int n = 0; n < length; ++n)
        {
            const double t      = (n - centre) / oversampling_;
            const double sinc   = std::abs(t) < 1.0e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
            const double r      = (n - centre) / (centre + 1.0);
            const double window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
            coefficients_[(size_t) n] = (float) (sinc * window);
        }
    }

    void measurePeak(int channel, float sample) noexcept
    {
        samplePeak_ = std::max(samplePeak_, (double) std::abs(sample));
        if (oversampling_ == 1)
            return;

        auto& history = history_[(size_t) channel];
        history[(size_t) historyAt_] = sample;

        // history[historyAt_] is the newest sample, history[historyAt_ - j]
        // the one j before it.
        for (int phase = 0; phase < oversampling_; ++phase)
        {
            double sum = 0.0;
            for (int j = 0; j < kTapsPerPhase; ++j)
            {
                const auto index = (size_t) ((historyAt_ - j + kTapsPerPhase) % kTapsPerPhase);
                sum += history[index] * coefficients_[(size_t) (j * oversampling_ + phase)];
            }
            truePeak_ = std::max(truePeak_, std::abs(sum));
        }
    }

    static constexpr int kHopsKept = 30;

    double sampleRate_ = 48000.0;
    int    channels_   = 2;
    int    hopLength_  = 4800;

    std::vector<KWeighting>         filters_;
    std::vector<double>             hopSquares_;
    std::array<double, kHopsKept>   hops_ {};
    std::int64_t                    hopCount_ = 0;
    int                             hopFill_  = 0;

    Histogram blocks_;
    Histogram shortTermBlocks_;
    double    maxMomentary_ = kSilence;
    double    maxShortTerm_ = kSilence;

    int                             oversampling_ = 4;
    std::vector<float>              coefficients_;
    std::vector<std::vector<float>> history_;
    int                             historyAt_  = 0;
    double                          samplePeak_ = 0.0;
    double                          truePeak_   = 0.0;
};

/** Everything a loudness measurement of a passage reports. */
struct LoudnessReport
{
    double integratedLufs   = LoudnessMeter::kSilence;
    double loudnessRangeLu  = 0.0;
    double maxMomentaryLufs = LoudnessMeter::kSilence;
    double maxShortTermLufs = LoudnessMeter::kSilence;
    double truePeakDb       = LoudnessMeter::kSilence;
    double samplePeakDb     = LoudnessMeter::kSilence;
    double seconds          = 0.0;

    static LoudnessReport of(const LoudnessMeter& meter, double seconds)
    {
        return { meter.integratedLufs(), meter.loudnessRangeLu(), meter.maxMomentaryLufs(),
                 meter.maxShortTermLufs(), meter.truePeakDb(),    meter.samplePeakDb(), seconds };
    }

    /** The same audio heard @p gainDb louder: every level moves, the range doesn't. */
    LoudnessReport withGain(double gainDb) const
    {
        auto out = *this;
        for (auto* level : { &out.integratedLufs, &out.maxMomentaryLufs, &out.maxShortTermLufs,
                             &out.truePeakDb, &out.samplePeakDb })
            *level += gainDb; // -inf stays -inf
        return out;
    }
};

/** The gain, in dB, that brings a measured integrated loudness to
    @p targetLufs, held back so the true peak stays at or under
    @p ceilingDbtp when @p limitToCeiling. Nothing when the audio has no
    measurable loudness (silence, or under 400 ms of it). */
struct LoudnessGain
{
    double gainDb     = 0.0;
    bool   limited    = false; // the ceiling, not the target, set the gain
    double reachesLufs = 0.0;  // the integrated loudness after the gain
};

inline bool loudnessGainFor(double integratedLufs, double truePeakDbtp, double targetLufs, double ceilingDbtp,
                            bool limitToCeiling, LoudnessGain& out) noexcept
{
    if (! std::isfinite(integratedLufs))
        return false;

    out.gainDb  = targetLufs - integratedLufs;
    out.limited = false;
    if (limitToCeiling && std::isfinite(truePeakDbtp) && truePeakDbtp + out.gainDb > ceilingDbtp)
    {
        out.gainDb  = ceilingDbtp - truePeakDbtp;
        out.limited = true;
    }
    out.reachesLufs = integratedLufs + out.gainDb;
    return true;
}

} // namespace soundsplice::engine
