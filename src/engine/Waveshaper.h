#pragma once

#include <algorithm>
#include <cmath>
#include <array>
#include <vector>

#include "engine/CabinetIr.h"
#include "engine/ShelfPeakFilter.h"
#include "engine/StateVariableFilter.h"

namespace looper::engine
{
/**
    Anti-aliased waveshaping, the core of a drive pedal.

    Clipping generates harmonics without limit. Every one above Nyquist folds
    back to a frequency unrelated to anything being played, so it isn't heard
    as brightness — it's heard as metallic, detuned grit that gets worse the
    higher you play. That folding is the main thing separating a distortion
    that sounds like an amp from one that sounds like a broken converter.

    Oversampling is the usual fix and costs a resampler, its filters, and
    several times the work per sample. First-order antiderivative
    anti-aliasing (ADAA) buys a measured 4.5-7dB of alias reduction for a few
    flops and no buffers — which also means there is nothing to allocate, so
    it is RT-safe by construction rather than by discipline.

    That is a real improvement, not a solved problem: heavy drive on high
    notes will still fold. Second-order ADAA or 2x oversampling on top is the
    next step if it isn't enough, and both fit behind this interface.

    For a memoryless shaper f with antiderivative F:

        y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])

    which is f averaged over the segment the signal actually crossed this
    sample, instead of a point sample of it. That average is what suppresses
    the aliases.

    JUCE-free so the aliasing claim can be measured headlessly rather than
    asserted in a comment — see the tests, which drive a sine whose fifth
    harmonic folds to a bin nothing else occupies.
*/
class Waveshaper
{
public:
    enum class Kind
    {
        Soft = 0, // tanh: an overdrive, compressing gradually into clip
        Hard      // a fuzz: flat above the threshold
    };

    /** Note that the at-rest input of a *biased* shaper is the bias, not
        zero: leaving the ADAA history at zero makes the first sample after a
        reset integrate the curve from 0 up to the bias, which injects a step
        into the DC blocker that then takes tens of milliseconds to decay -
        i.e. a thump on the first note. */
    void reset() noexcept
    {
        lastInput_ = (double) asymmetry_;
        dcX1_ = dcY1_ = 0.0;
    }

    void setKind(Kind kind) noexcept { kind_ = kind; }

    /** @p drive multiplies the input before shaping, which is what a drive
        knob does: the shaper's own curve never changes, you just push more
        signal into the same curve. */
    void setDrive(float drive) noexcept { drive_ = std::max(0.01f, drive); }

    /** A DC offset added before shaping, so the curve clips the two halves of
        the waveform differently and **even** harmonics appear - a 2nd, most
        audibly. A symmetric curve produces odd harmonics only, no matter how
        hard it is driven, and that is most of what a clipper lacks against a
        real tube stage: every tube stage is asymmetric, and "warmth" is very
        largely the 2nd harmonic that asymmetry generates.

        The offset is removed again after shaping, so this changes the tone
        without putting DC on the bus.

        Defaults to 0, which is bit-identical to the symmetric shaper this had
        before - existing uses are unchanged until they ask for asymmetry. */
    void setAsymmetry(float amount) noexcept { asymmetry_ = std::clamp(amount, -1.0f, 1.0f); }

    /** The bias currently in effect. */
    float asymmetry() const noexcept { return asymmetry_; }

    float processSample(float input) noexcept
    {
        const double bias = (double) asymmetry_;

        const double x0 = lastInput_;
        const double x1 = (double) input * (double) drive_ + bias;
        lastInput_      = x1;

        const double delta = x1 - x0;

        // As x1 approaches x0 the quotient becomes 0/0 and, well before that,
        // loses precision: F is order 1-10 at usable drive settings while the
        // difference goes to zero, so the subtraction cancels away most of the
        // significant digits. A sine passes through this at both peaks, every
        // cycle.
        //
        // Done in double so that region stays well conditioned. Measured, this
        // makes no difference to the aliasing figures against a float version
        // — the ill-conditioned window is narrow and rarely landed in — but
        // it costs almost nothing and the hazard is real rather than
        // theoretical.
        //
        // A sustained note is exactly where consecutive samples are nearly
        // equal, so getting the fallback wrong turns held notes into noise,
        // which is precisely backwards.
        const double shaped = (std::abs(delta) < kMinDelta)
                                  ? shape(0.5 * (x0 + x1))
                                  : (antiderivative(x1) - antiderivative(x0)) / delta;

        // Zero bias takes the untouched path, so the default stays
        // bit-identical to the symmetric shaper rather than merely close.
        return bias == 0.0 ? (float) shaped : (float) removeOffset(shaped, bias);
    }

    /** The shaper itself, with no anti-aliasing. Only useful for comparison —
        the tests measure how much aliasing the ADAA path avoids relative to
        this. */
    float processSampleNaive(float input) noexcept
    {
        const double bias   = (double) asymmetry_;
        const double shaped = shape((double) input * (double) drive_ + bias);
        return bias == 0.0 ? (float) shaped : (float) removeOffset(shaped, bias);
    }

private:
    // Only has to cover a genuine 0/0. In double the cancellation near it
    // still leaves ample precision, and the midpoint fallback is a good
    // approximation over so short a segment anyway.
    static constexpr double kMinDelta = 1.0e-8;

    double shape(double x) const noexcept
    {
        if (kind_ == Kind::Hard)
            return std::clamp(x, -1.0, 1.0);
        return std::tanh(x);
    }

    double antiderivative(double x) const noexcept
    {
        if (kind_ == Kind::Hard)
        {
            // The integral of clamp: quadratic inside the linear region,
            // linear outside it, and continuous at the corners.
            const double a = std::abs(x);
            return a <= 1.0 ? 0.5 * x * x : a - 0.5;
        }

        // The integral of tanh is log(cosh(x)) — but cosh overflows to
        // infinity around |x| = 710, and a drive pedal is exactly where large
        // inputs turn up. This identity is equal to it and overflows nowhere:
        //     log(cosh x) = |x| + log1p(exp(-2|x|)) - log 2
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - kLogTwo;
    }

    /**
        Removes the offset an asymmetric curve leaves behind, in two parts,
        because it has two parts.

        The at-rest offset is exactly `shape(bias)` and is subtracted in
        closed form. Doing this rather than leaving it to the filter is what
        makes silence come out as *exactly* silence: a DC blocker alone starts
        from zero state and has to settle through the step, which at a 4Hz
        corner takes tens of milliseconds - audible as a thump every time the
        pedal is switched in.

        What remains is level-dependent: asymmetric clipping produces a DC
        component that varies with how hard the curve is being driven at that
        moment, so there is no one number that covers a note decaying from
        fully clipped back to rest. That part goes through the DC blocker.

        The offset matters because DC eats headroom in every stage downstream
        while moving nothing audible.
    */
    double removeOffset(double shaped, double bias) noexcept
    {
        return blockDc(shaped - shape(bias));
    }

    /** One-pole DC blocker. The pole sits at ~4Hz at 48kHz (lower at higher
        rates), well below anything a guitar produces, so it takes the offset
        out without touching the attack transient the asymmetry exists to
        shape. */
    double blockDc(double x) noexcept
    {
        const double y = x - dcX1_ + kDcPole * dcY1_;
        dcX1_ = x;
        dcY1_ = y;
        return y;
    }

    static constexpr double kDcPole = 0.9995;
    static constexpr double kLogTwo = 0.69314718055994531;

    Kind   kind_      = Kind::Soft;
    float  drive_     = 1.0f;
    float  asymmetry_ = 0.0f;
    double lastInput_ = 0.0;
    double dcX1_      = 0.0;
    double dcY1_      = 0.0;
};

/**
    A guitar speaker, roughly.

    A cabinet is not a pair of corner frequencies, and treating it as one is
    most of why a distortion chain reads as "plugin" rather than "amp". A real
    4x12 has:

      - a **resonant bump around 100Hz** where the cone and cabinet volume
        resonate, not a gentle 6dB/oct slope into nothing,
      - a **cone-breakup peak around 2kHz**, which is what makes a guitar
        *cut* through a mix,
      - a **notch around 3.5kHz** just above it, the other half of that
        characteristic honk,
      - and a **cliff above 5kHz** far steeper than a guitar amp's electronics
        - 4 poles here, so fizz is genuinely gone rather than 12dB down.

    The previous version had only the first and last of those, and both as
    first-order real poles. That made it simultaneously too dull where a
    guitar cuts and too leaky where fizz lives - the two halves of the
    complaint that prompted this. The peak/notch pair in between is the part
    you hear missing.

    Still not an impulse response: a convolution would be more faithful and
    would need an IR to ship, a partitioned convolver and a latency story.
    But the midrange structure is cheap, and it is what your ear identifies
    as a speaker.
*/
class CabinetSim
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        lowResonance_.prepare(sampleRate_);
        lowResonance_.setShape(ShelfPeakFilter::Shape::Peaking);

        presence_.prepare(sampleRate_);
        presence_.setShape(ShelfPeakFilter::Shape::Peaking);

        notch_.prepare(sampleRate_);
        notch_.setShape(ShelfPeakFilter::Shape::Peaking);

        for (auto& stage : topEnd_)
        {
            stage.prepare(sampleRate_);
            stage.setMode(StateVariableFilter::Mode::LowPass);
        }

        setCharacter(kDefaultLowResonanceHz, kDefaultPresenceHz,
                     kDefaultNotchHz, kDefaultTopHz);
        setLowCut(kDefaultLowCutHz);
        reset();

        buildImpulseResponse();
    }

    /**
        Convolve a synthesised impulse response instead of running the filter
        chain — see engine/CabinetIr.h for what that buys and why it is direct
        rather than partitioned.

        The response is built *from this cabinet's own filters*, so the
        voicing is identical by construction and the only difference is the
        time-domain structure the filters cannot express. Off by default, so
        nothing that has not asked for it changes.
    */
    void setUseImpulseResponse(bool use) noexcept { useImpulseResponse_ = use; }
    bool usesImpulseResponse() const noexcept { return useImpulseResponse_; }

    void reset() noexcept
    {
        lowResonance_.reset();
        presence_.reset();
        notch_.reset();
        for (auto& stage : topEnd_)
            stage.reset();
        highPassState_ = 0.0f;
        convolver_.reset();
    }

    /** The four frequencies that decide which speaker this is. Defaults are a
        4x12 with V30-ish voicing; a smaller open-back combo is the same
        structure with the presence peak higher and the top corner lower. */
    void setCharacter(float lowResonanceHz, float presenceHz,
                      float notchHz, float topHz) noexcept
    {
        lowResonance_.setFrequency(lowResonanceHz);
        lowResonance_.setQ(kLowResonanceQ);
        lowResonance_.setGainDb(kLowResonanceDb);

        presence_.setFrequency(presenceHz);
        presence_.setQ(kPresenceQ);
        presence_.setGainDb(kPresenceDb);

        notch_.setFrequency(notchHz);
        notch_.setQ(kNotchQ);
        notch_.setGainDb(kNotchDb);

        // Butterworth, which means each 2-pole section gets its *own* Q -
        // 0.707 in all of them is the common mistake, and it droops the
        // passband by 6dB per extra section instead of leaving it flat. That
        // droop is attenuation stolen from the guitar's own range rather than
        // from the fizz, so getting these right is what makes the rolloff
        // steep where it needs to be.
        static constexpr float kButterworthQ[] = { 0.5176f, 0.7071f, 1.9319f };
        for (size_t i = 0; i < topEnd_.size(); ++i)
        {
            topEnd_[i].setCutoff(topHz);
            topEnd_[i].setResonance(kButterworthQ[i]);
        }
    }

    /** Where the cabinet stops producing bottom end at all. Separate from the
        resonance above it, because they are separate physical things: the
        resonance is the cone, this is the cabinet simply not moving that much
        air. */
    void setLowCut(float hz) noexcept { highPassCoeff_ = onePoleCoeff(hz); }

    float processSample(float input) noexcept
    {
        return (useImpulseResponse_ && convolver_.isReady())
                   ? convolver_.processSample(input)
                   : filterSample(input);
    }

    /** The filter chain itself, without the impulse-response branch — the path
        the response is *built from*, so it has to stay reachable regardless of
        which mode is selected. */
    float filterSample(float input) noexcept
    {
        // Highpass first, by subtracting a lowpassed copy, so the resonance
        // below is shaping a signal that has already lost its subsonic
        // content rather than resonating on mud.
        highPassState_ += highPassCoeff_ * (input - highPassState_);
        float x = input - highPassState_;

        x = lowResonance_.processSample(x);
        x = presence_.processSample(x);
        x = notch_.processSample(x);

        // Six poles of lowpass: 36dB/octave, so 10kHz lands ~35dB down rather
        // than the ~12dB the old three-real-pole cabinet managed. A speaker's
        // acoustic rolloff really is this steep once the cone stops moving as
        // a piston, and this is the single change that turns "fizzy" into
        // "saturated".
        for (auto& stage : topEnd_)
            x = stage.processSample(x);

        return x;
    }

private:
    /**
        Synthesises this cabinet's response: the reflection train and breakup
        tail, run through the very filters this class would otherwise use.

        Running the *filters themselves* rather than restating their response
        is what guarantees the two modes are voiced identically — anything else
        would be a second description of the cabinet, free to drift from the
        first. Everything is reset afterwards, so building the response leaves
        no state behind for the audio that follows.
    */
    void buildImpulseResponse()
    {
        // ~10ms: long enough for every reflection plus the breakup tail, short
        // enough that direct convolution stays cheap — see CabinetIr.h.
        const int length = std::max(64, (int) std::lround(0.010 * sampleRate_));

        // The energy the filter chain's own response carries, measured rather
        // than assumed, so switching modes cannot change the level.
        reset();
        std::vector<float> filtered((size_t) length, 0.0f);
        for (int i = 0; i < length; ++i)
            filtered[(size_t) i] = filterSample(i == 0 ? 1.0f : 0.0f);

        const double targetEnergy = cabinetImpulseEnergy(filtered);

        reset();
        auto impulse = buildCabinetImpulseTrain(sampleRate_, length);
        for (auto& tap : impulse)
            tap = filterSample(tap);

        normaliseCabinetImpulse(impulse, targetEnergy);
        convolver_.setImpulseResponse(std::move(impulse));

        reset();
    }

    CabinetConvolver convolver_;
    bool             useImpulseResponse_ = false;

    static constexpr float kDefaultLowCutHz       = 80.0f;
    static constexpr float kDefaultLowResonanceHz = 105.0f;
    static constexpr float kDefaultPresenceHz     = 2000.0f;
    static constexpr float kDefaultNotchHz        = 3500.0f;
    static constexpr float kDefaultTopHz          = 5000.0f;

    static constexpr float kLowResonanceQ  = 1.2f;
    static constexpr float kLowResonanceDb = 5.0f;
    static constexpr float kPresenceQ      = 1.1f;
    static constexpr float kPresenceDb     = 6.0f;
    static constexpr float kNotchQ         = 2.0f;
    static constexpr float kNotchDb        = -8.0f;

    float onePoleCoeff(float hz) const noexcept
    {
        const float x = (float) (6.2831853 * (double) hz / sampleRate_);
        return std::clamp(x / (1.0f + x), 0.0f, 1.0f);
    }

    double sampleRate_ = 48000.0;

    ShelfPeakFilter                     lowResonance_, presence_, notch_;
    std::array<StateVariableFilter, 3>  topEnd_; // three 2-pole sections = 36dB/oct

    float highPassCoeff_ = 0.01f;
    float highPassState_ = 0.0f;
};

} // namespace looper::engine
