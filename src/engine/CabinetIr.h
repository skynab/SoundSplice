#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace looper::engine
{
/**
    A cabinet impulse response, and the convolver that plays it.

    `CabinetSim` shapes a speaker with filters, and says of itself that it is
    "still not an impulse response: a convolution would be more faithful and
    would need an IR to ship, a partitioned convolver and a latency story."
    All three of those objections are answered here rather than accepted:

      - **Nothing is shipped.** The response is *synthesised* at prepare time
        from the cabinet's own filter network plus the time-domain structure
        below, so there is no asset, no licence, and no file to lose.
      - **No partitioning, and no latency.** The response is deliberately short
        (~10ms) and convolved directly in the time domain. A partitioned FFT
        convolver only earns its complexity on responses long enough to make
        direct convolution expensive, and it buys that with a block of latency
        — which on a guitar being played live is exactly what you cannot pay.
        Ten milliseconds is where a close-mic'd cabinet keeps essentially all
        of its character; what comes after is the room, and a close mic barely
        hears it.

    **What this adds that a filter network cannot.** An IIR filter has one path
    from input to output: it can shape magnitude, but it cannot represent the
    same sound arriving twice. A real cabinet does exactly that — the wave
    reaches the microphone straight from the cone, again off the baffle edge,
    and again off the back of the box — and those arrivals comb with each other
    and smear the cone's breakup in time. That structure is what the ear reads
    as "a speaker in a box with a microphone in front of it" rather than "a
    filter", and it is the part that no amount of extra poles reproduces.
*/

/** One early reflection: when it arrives, how loud, and whether it is
    inverted. */
struct CabinetReflection
{
    double delayMs  = 0.0;
    float  gain     = 0.0f;
    bool   inverted = false;
};

/**
    The arrivals that make up a close-mic'd cabinet, before any filtering.

    Times and levels are the geometry of an ordinary 4x12 with a mic close to
    the grille — a few centimetres for the cone-to-mic path, ~12cm to the
    baffle edge, ~30cm to the back of the box and out again. They are
    deliberately modest: this is a close mic, so the direct sound dominates and
    the reflections colour rather than double it.
*/
inline const std::vector<CabinetReflection>& cabinetReflections()
{
    static const std::vector<CabinetReflection> reflections {
        { 0.0,  1.00f, false }, // the direct arrival
        { 0.34, 0.34f, true  }, // baffle edge — inverted, as a diffraction is
        { 0.62, 0.22f, false }, // the neighbouring cone in a 4x12
        { 1.15, 0.18f, true  }, // off the back of the box
        { 2.05, 0.09f, false }, // and once more around it
    };
    return reflections;
}

/**
    Builds the pre-filter impulse train: the reflections above, plus a short
    decaying noise tail for the cone's breakup ringing.

    The tail is noise rather than silence because a speaker cone does not stop
    when the signal does — it breaks up and rings, briefly and chaotically, and
    that is audible as the "air" around a mic'd cab. Kept low and short; more
    of it stops sounding like a speaker and starts sounding like a room.

    @p length is in samples and is the whole response, so the caller decides
    how much of the tail survives.
*/
inline std::vector<float> buildCabinetImpulseTrain(double sampleRate, int length)
{
    std::vector<float> impulse((size_t) std::max(1, length), 0.0f);

    if (sampleRate <= 0.0)
        return impulse;

    for (const auto& reflection : cabinetReflections())
    {
        const int at = (int) std::lround(reflection.delayMs * 0.001 * sampleRate);
        if (at < 0 || at >= (int) impulse.size())
            continue;

        impulse[(size_t) at] += reflection.inverted ? -reflection.gain : reflection.gain;
    }

    // Cone breakup: a short burst of noise under the reflections, decaying
    // fast. Deterministic, so a project sounds the same on every run and every
    // machine — a cabinet that was subtly different each launch would be a
    // very confusing bug to chase.
    uint32_t noise = 0x1234567u;
    const double tailSeconds = 0.004;
    const int    tailLength  = std::min((int) impulse.size(), (int) (tailSeconds * sampleRate));

    for (int i = 1; i < tailLength; ++i)
    {
        noise = noise * 1664525u + 1013904223u;
        const float white = (float) ((int32_t) noise) * (1.0f / 2147483648.0f);
        const float decay = (float) std::exp(-6.0 * (double) i / (double) tailLength);

        impulse[(size_t) i] += 0.06f * white * decay;
    }

    return impulse;
}

/**
    Direct-form FIR convolution.

    Direct rather than FFT for the reason given above: at a few hundred taps
    the arithmetic is cheap, and it is the only form with genuinely zero
    latency. Nothing here allocates once prepared.
*/
class CabinetConvolver
{
public:
    /** Hands over the response. Message thread. */
    void setImpulseResponse(std::vector<float> impulse)
    {
        impulse_ = std::move(impulse);
        history_.assign(impulse_.size(), 0.0f);
        writeIndex_ = 0;
    }

    void reset() noexcept
    {
        std::fill(history_.begin(), history_.end(), 0.0f);
        writeIndex_ = 0;
    }

    bool isReady() const noexcept { return ! impulse_.empty(); }

    int length() const noexcept { return (int) impulse_.size(); }

    float processSample(float input) noexcept
    {
        const int size = (int) impulse_.size();
        if (size <= 0)
            return input;

        history_[(size_t) writeIndex_] = input;

        float sum   = 0.0f;
        int   index = writeIndex_;

        for (int tap = 0; tap < size; ++tap)
        {
            sum += impulse_[(size_t) tap] * history_[(size_t) index];
            if (--index < 0)
                index = size - 1;
        }

        if (++writeIndex_ >= size)
            writeIndex_ = 0;

        return sum;
    }

private:
    std::vector<float> impulse_;
    std::vector<float> history_;
    int                writeIndex_ = 0;
};

/** The total energy of a response — what level-matching two of them means. */
inline double cabinetImpulseEnergy(const std::vector<float>& impulse)
{
    double energy = 0.0;
    for (float tap : impulse)
        energy += (double) tap * (double) tap;
    return std::sqrt(energy);
}

/**
    Scales @p impulse to carry @p targetEnergy.

    Level-matching is not cosmetic: the IR and the filter network it was built
    from are alternatives for the same slot, and if switching between them
    changed the volume then every comparison anyone made — by ear or by
    measurement — would be measuring the level difference instead of the sound.

    Matched on **energy**, not on DC gain. Matching DC was the first attempt
    and it made the cabinet louder, because the reflections partly cancel at DC
    (they sum to well under one there) so holding DC constant scales every
    other frequency up to compensate. Energy is what the ear is closer to, and
    it is what keeps a mixed signal at the level it arrived.
*/
inline void normaliseCabinetImpulse(std::vector<float>& impulse, double targetEnergy)
{
    const double energy = cabinetImpulseEnergy(impulse);

    if (energy < 1.0e-9 || targetEnergy <= 0.0)
        return;

    const auto scale = (float) (targetEnergy / energy);
    for (auto& tap : impulse)
        tap *= scale;
}

} // namespace looper::engine
