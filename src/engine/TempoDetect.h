#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "engine/Fft.h"

namespace looper::engine
{
/**
    Estimates the tempo of a decoded audio loop.

    JUCE-free and built on the engine's own FFT, like every other analysis
    module here, so what it claims is testable headlessly — which for a tempo
    detector is unusually valuable: a synthetic click train at a known BPM has
    an exact right answer, so accuracy is an assertion rather than something
    only checkable by ear.

    The method is the standard one, in three stages:

      1. **Onset envelope** by spectral flux — the summed *positive* change in
         magnitude between successive STFT frames. Positive-only is the whole
         trick: it responds to energy arriving (a hit) and ignores energy
         decaying (its tail), which is what makes a beat a spike rather than a
         plateau.
      2. **Autocorrelation** of that envelope. A periodic sequence of onsets
         correlates with itself at the beat period, so the lag of the strongest
         peak is the beat.
      3. **Octave resolution**, which is not optional — see below.

    What this deliberately does *not* do is find where the beats *are* (phase).
    A loop is assumed to start on its downbeat, which is true of essentially
    every loop anyone drags in from a library, and finding the offset as well
    is a much larger problem for a much smaller payoff.
*/
struct TempoEstimate
{
    /** Beats per minute, or 0 if nothing could be estimated (silence, or a
        clip too short to contain a measurable beat). */
    double bpm = 0.0;

    /**
        Roughly 0..1, how strongly periodic the onsets actually were.

        Exists so a weak result can be *offered* rather than silently applied.
        A tempo detector that always returns a number, with no way to tell a
        confident 128 from a coin-flip 91, is one that will eventually warp a
        sustained pad to a beat it invented.
    */
    double confidence = 0.0;

    bool isUsable() const { return bpm > 0.0; }
};

namespace tempodetect
{
    /** STFT size for the onset envelope. Smaller than the phase vocoder's
        2048: this needs *time* resolution (when did the hit happen) rather
        than frequency resolution (what pitch was it), which is the opposite
        trade-off. */
    inline constexpr int kFftSize = 1024;

    /** Quarter-frame hop, giving ~86 envelope samples per second at 44.1kHz —
        fine enough to place a beat to within a few milliseconds. */
    inline constexpr int kHop = kFftSize / 4;

    /** The tempo range considered before octave resolution. Wide, because
        halving and doubling are handled explicitly afterwards. */
    inline constexpr double kMinBpm = 50.0;
    inline constexpr double kMaxBpm = 220.0;

    /** Where a tempo is *reported*, once octave errors are resolved.
        Musically this is the range people actually name a tempo in: a 170 BPM
        drum & bass loop is called 170, not 85, and a 70 BPM beat is called 70,
        not 140. */
    inline constexpr double kPreferredMinBpm = 70.0;
    inline constexpr double kPreferredMaxBpm = 180.0;

    /**
        The onset-strength envelope of @p samples: one value per STFT hop,
        each the summed positive magnitude change from the previous frame.

        Exposed (rather than kept private to detectTempo) because it is the
        stage worth inspecting on its own when a detection looks wrong, and
        because it is independently testable — a click train must produce
        spikes at the clicks.
    */
    inline std::vector<float> onsetEnvelope(const std::vector<float>& samples, double sampleRate)
    {
        std::vector<float> envelope;

        if (sampleRate <= 0.0 || (int) samples.size() < kFftSize)
            return envelope;

        const int numBins = kFftSize / 2;

        // Hann, computed once. Windowing matters here for the usual reason:
        // an unwindowed frame boundary is itself a discontinuity, and a
        // discontinuity looks exactly like an onset.
        std::vector<float> window((size_t) kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / (kFftSize - 1)));

        std::vector<float> previousMagnitude((size_t) numBins, 0.0f);
        std::vector<float> real, imaginary;
        bool               havePrevious = false;

        for (int start = 0; start + kFftSize <= (int) samples.size(); start += kHop)
        {
            real.assign((size_t) kFftSize, 0.0f);
            imaginary.assign((size_t) kFftSize, 0.0f);

            for (int i = 0; i < kFftSize; ++i)
                real[(size_t) i] = samples[(size_t) (start + i)] * window[(size_t) i];

            fft::transform(real, imaginary, false);

            float flux = 0.0f;
            for (int bin = 0; bin < numBins; ++bin)
            {
                const float magnitude = std::sqrt(real[(size_t) bin] * real[(size_t) bin]
                                                + imaginary[(size_t) bin] * imaginary[(size_t) bin]);

                // Positive differences only: energy arriving is an onset,
                // energy decaying is the tail of one that already happened.
                const float difference = magnitude - previousMagnitude[(size_t) bin];
                if (difference > 0.0f)
                    flux += difference;

                previousMagnitude[(size_t) bin] = magnitude;
            }

            // The first frame has nothing to differ from, so its "flux" is
            // just its own magnitude — a false onset at time zero, and one
            // that would bias every autocorrelation that follows.
            if (havePrevious)
                envelope.push_back(flux);
            havePrevious = true;
        }

        return envelope;
    }

    /** Subtracts a local mean and clamps at zero, leaving only the peaks.

        Without this, a loud sustained section raises the whole envelope and
        the autocorrelation starts measuring the *section*, not the beat. */
    inline void removeEnvelopeBias(std::vector<float>& envelope)
    {
        if (envelope.empty())
            return;

        double mean = 0.0;
        for (float value : envelope)
            mean += value;
        mean /= (double) envelope.size();

        for (auto& value : envelope)
            value = std::max(0.0f, value - (float) mean);
    }
}

/**
    Estimates the tempo of @p samples (mono, already decoded).

    @p sampleRate is the audio's own rate, not the device's — this analyses the
    file as it exists on disk.
*/
inline TempoEstimate detectTempo(const std::vector<float>& samples, double sampleRate)
{
    using namespace tempodetect;

    TempoEstimate estimate;

    auto envelope = onsetEnvelope(samples, sampleRate);
    if (envelope.size() < 8)
        return estimate; // too short to contain a measurable beat

    // Crest factor of the raw envelope, measured before the bias removal that
    // is about to flatten it: how much the loudest onset stands above the
    // average one. A drum loop is mostly silence with spikes (high crest); a
    // sustained tone is flat (crest ~1). Feeds the confidence below.
    double peakiness = 0.0;
    {
        double mean = 0.0;
        double peak = 0.0;
        for (float value : envelope)
        {
            mean += (double) value;
            peak  = std::max(peak, (double) value);
        }
        mean /= (double) envelope.size();

        if (mean > 0.0)
        {
            // A crest of 1 is perfectly flat and worth no confidence at all;
            // by about 4 the onsets are unmistakable.
            const double crest = peak / mean;
            peakiness = std::clamp((crest - 1.0) / 3.0, 0.0, 1.0);
        }
    }

    removeEnvelopeBias(envelope);

    // Envelope samples per second: the rate the autocorrelation lags are in.
    const double envelopeRate = sampleRate / (double) kHop;

    const int minLag = (int) std::floor(envelopeRate * 60.0 / kMaxBpm);
    const int maxLag = (int) std::ceil (envelopeRate * 60.0 / kMinBpm);

    if (minLag < 1 || maxLag <= minLag || maxLag >= (int) envelope.size())
    {
        // Not enough envelope to hold even one period of the slowest tempo
        // considered. Reported as "no estimate" rather than as a guess from
        // the fragment that does fit.
        if (maxLag >= (int) envelope.size())
            return estimate;
        return estimate;
    }

    /** Normalised autocorrelation at @p lag: the mean product of the envelope
        with itself shifted, divided by the overlap so that long lags aren't
        penalised simply for overlapping less. */
    auto correlationAt = [&envelope](int lag)
    {
        const int overlap = (int) envelope.size() - lag;
        if (overlap <= 0)
            return 0.0;

        double sum = 0.0;
        for (int i = 0; i < overlap; ++i)
            sum += (double) envelope[(size_t) i] * (double) envelope[(size_t) (i + lag)];

        return sum / (double) overlap;
    };

    int    bestLagInteger  = 0;
    double bestCorrelation = 0.0;
    double totalCorrelation = 0.0;
    int    consideredLags   = 0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const double correlation = correlationAt(lag);
        totalCorrelation += correlation;
        ++consideredLags;

        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestLagInteger  = lag;
        }
    }

    if (bestLagInteger <= 0 || bestCorrelation <= 0.0)
        return estimate;

    // --- Octave resolution.
    //
    // The failure mode of every naive tempo detector, and it cannot be fixed
    // by folding the *result* into a preferred range: onsets a beat apart are
    // also two beats apart, so a 174 BPM break correlates just as well at 87 —
    // and 87 and 174 are both perfectly ordinary tempos, so there is nothing
    // out-of-range to notice. Whichever of the two wins is then decided by
    // noise, which is exactly what happened here (174 came back as 87).
    //
    // Resolved by preferring the *fastest* interpretation that the envelope
    // genuinely supports: if half the winning lag correlates nearly as well,
    // there really are onsets at that rate and the faster reading is the true
    // one. A loop that has no onsets in between — a 76 BPM beat — sees almost
    // no correlation at half its lag, so its slower reading survives.
    {
        constexpr double kSubdivisionThreshold = 0.8;

        for (int divisor : { 4, 3, 2 })
        {
            const int candidate = bestLagInteger / divisor;
            if (candidate < minLag)
                continue;

            if (correlationAt(candidate) >= kSubdivisionThreshold * bestCorrelation)
            {
                bestLagInteger  = candidate;
                bestCorrelation = correlationAt(candidate);
                break; // largest subdivision that holds up; smaller ones are then folded below
            }
        }
    }

    // Parabolic interpolation around the winning lag, so the reported tempo
    // isn't quantised to whole envelope samples. At ~86 envelope samples a
    // second a whole-sample lag is worth well over a BPM up at 170, which is
    // the difference between a loop that drifts and one that doesn't. Done
    // after the octave choice, so it refines the lag actually being reported.
    double bestLag = (double) bestLagInteger;
    {
        if (bestLagInteger > minLag && bestLagInteger < maxLag)
        {
            const double before = correlationAt(bestLagInteger - 1);
            const double at     = correlationAt(bestLagInteger);
            const double after  = correlationAt(bestLagInteger + 1);

            const double denominator = before - 2.0 * at + after;
            if (std::abs(denominator) > 1.0e-12)
            {
                const double offset = 0.5 * (before - after) / denominator;
                if (std::abs(offset) <= 1.0)
                    bestLag += offset;
            }
        }
    }

    double bpm = 60.0 * envelopeRate / bestLag;

    // Now fold into the range tempos are actually *named* in. This catches
    // what the subdivision step above deliberately overshoots on: hats on
    // sixteenths legitimately support a reading four times the pulse, and
    // halving it back is how a person would say it.
    while (bpm < kPreferredMinBpm && bpm > 0.0)
        bpm *= 2.0;
    while (bpm > kPreferredMaxBpm)
        bpm /= 2.0;

    // The fold can overshoot for genuinely extreme tempos (a 60 BPM loop
    // doubles to 120, fine; a 200 BPM one halves to 100, also fine) - but if
    // it lands outside the range considered at all, there was nothing here.
    if (bpm < kMinBpm || bpm > kMaxBpm)
        return estimate;

    estimate.bpm = bpm;

    // --- Confidence.
    //
    // Two independent things have to be true for a tempo to mean anything,
    // and measuring only the first is what made a sustained sine report full
    // confidence in a tempo it did not have:
    //
    //   1. The envelope repeats at the chosen lag (periodicity), and
    //   2. There are distinct onsets in it at all (peakiness).
    //
    // Peakiness is the one that was missing. After bias removal a signal with
    // no onsets is essentially zeros, so its "peak above its own mean" ratio
    // is numerical noise — which can be arbitrarily large. Crest factor,
    // measured *before* bias removal, is what separates a drum loop from a
    // held chord: a click train's envelope is mostly silence with occasional
    // spikes, a pad's is flat.
    const double meanCorrelation = consideredLags > 0
                                     ? totalCorrelation / (double) consideredLags
                                     : 0.0;

    const double periodicity = meanCorrelation > 0.0
        ? std::clamp((bestCorrelation / meanCorrelation - 1.0) / 2.0, 0.0, 1.0)
        : 0.0;

    estimate.confidence = periodicity * peakiness;

    return estimate;
}

/**
    The time-stretch factor that makes a clip recorded at @p sourceBpm play at
    @p projectBpm — in timeStretch()'s terms, where 2.0 is twice as long.

    A loop faster than the project has to be *stretched* to fit it (a 174 BPM
    break in a 120 BPM song plays 1.45x longer), which is why this is source
    over project and not the other way round — the inversion is the easiest
    mistake to make here and the hardest to notice, since it is still
    "in time", just at the wrong tempo.

    Returns 1.0 (no stretching) whenever warping cannot or should not happen:
    switched off, or a tempo that isn't known. An unknown source tempo is the
    common case for any file whose detection was inconclusive, and playing it
    unwarped is right — the alternative is warping by a ratio derived from a
    number nobody has.
*/
inline double warpStretchFactor(double sourceBpm, double projectBpm, bool warpEnabled)
{
    if (! warpEnabled || sourceBpm <= 0.0 || projectBpm <= 0.0)
        return 1.0;

    // Clamped at two octaves either way. Past that the phase vocoder stops
    // producing anything musically useful and the rendering cost climbs with
    // the output length, so a wildly wrong source tempo (a mis-detection, or a
    // hand-typed 12) degrades into something audible rather than into a hang.
    return std::clamp(sourceBpm / projectBpm, 0.25, 4.0);
}

/** Convenience for the common "I have interleaved or multi-channel audio"
    case: averages channels to mono first, since tempo is a property of the
    performance rather than of either channel. */
inline TempoEstimate detectTempo(const std::vector<std::vector<float>>& channels, double sampleRate)
{
    if (channels.empty())
        return {};

    if (channels.size() == 1)
        return detectTempo(channels[0], sampleRate);

    size_t length = channels[0].size();
    for (const auto& channel : channels)
        length = std::min(length, channel.size());

    std::vector<float> mono(length, 0.0f);
    for (const auto& channel : channels)
        for (size_t i = 0; i < length; ++i)
            mono[i] += channel[i];

    const float scale = 1.0f / (float) channels.size();
    for (auto& sample : mono)
        sample *= scale;

    return detectTempo(mono, sampleRate);
}

} // namespace looper::engine
