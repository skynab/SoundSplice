#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Oversampler.h"
#include "engine/Waveshaper.h"

namespace looper::engine
{
/**
    An overdrive/distortion pedal: gain into a clipper, through a speaker.

    Signal path, and the reason for the order:

        pre-emphasis -> drive -> shaper -> cabinet -> tilt -> level

    Pre-emphasis sits *before* the shaper because a tone control before
    clipping decides which frequencies get distorted, while one after only
    shapes what came out. Both are real pedal designs and they sound
    different; tilting the input toward the mids is what stops a chord from
    turning to mush, since a loud low string otherwise dominates the clipper
    and takes the rest of the chord down with it.

    The cabinet is not optional and not a later refinement. Clipping puts
    enormous energy above a guitar speaker's ~5kHz corner, and that energy is
    heard as fizz. Shipping the drive without it would sound wrong, and the
    wrongness would be blamed on the drive.

    Parameters are atomics set from the message thread and read per block, the
    same arrangement FilterEffect and DelayEffect use: a knob turn must not
    rebuild the chain, since that would reset every tail in it.
*/
class DriveEffect
{
public:
    /**
        How many gain stages the signal passes through, 1..kMaxStages.

        One is a pedal: a single clipper, and exactly what this did before the
        parameter existed. Two or three is an amp, and the difference is not
        "more distortion" — it is a different *kind*.

        Three things change when stages cascade:

          - Each stage clips gently and the next one clips *that*, so the
            composite curve has a far softer knee and much more compression
            than one hard push ever produces.
          - Every stage adds its own harmonics to a signal that already has
            harmonics, which multiplies them out into a dense spectrum rather
            than the fixed harmonic set a single shaper gives at any drive.
          - Between stages the bass is rolled off *before* the next clipper
            sees it. That is what makes a high-gain amp tight instead of
            muddy: low strings otherwise intermodulate with everything above
            them, and no amount of EQ afterwards separates them again.

        Defaults to 1, so every existing project and preset is untouched; the
        amp-like tones opt in.
    */
    void setStages(int stages) { stages_.store(stages, std::memory_order_relaxed); }

    /** Convolve a synthesised cabinet response rather than filtering — see
        engine::CabinetSim::setUseImpulseResponse. */
    void setCabinetIr(bool use) { cabinetIr_.store(use, std::memory_order_relaxed); }

    void prepare(double sampleRate, int /*blockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        for (auto& channel : channels_)
        {
            for (auto& shaper : channel.shapers)
                shaper.reset();
            for (auto& stage : channel.inter)
                stage = {};
            channel.oversampler.reset();
            channel.cab.prepare(sampleRate_);
            channel.preState     = 0.0f;
            channel.preTopState  = 0.0f;
            channel.tiltState    = 0.0f;
        }

        // ~700Hz: above the low strings' fundamentals, below where the pick
        // attack lives, which is the split that keeps chords defined.
        preCoeff_    = onePoleCoeff(700.0f);
        // ...and ~2.5kHz above it, which is what makes the pre-emphasis an
        // actual bandpass. See the comment in process().
        preTopCoeff_ = onePoleCoeff(2500.0f);
        tiltCoeff_   = onePoleCoeff(900.0f);

        // Interstage coupling. ~180Hz is high for a coupling capacitor and
        // deliberately so: it is the bass cut that keeps the *next* clipper
        // from being handed a low string's full energy, which is the whole
        // point of cascading rather than turning one stage up. The lowpass is
        // a triode's Miller capacitance — it stops each stage handing the next
        // one fizz to multiply.
        interHighpassCoeff_   = onePoleCoeff(120.0f);
        interLowpassCoeff_    = onePoleCoeff(6500.0f);

        // The cascade runs *inside* the oversampler, so when oversampling is
        // on these filters see 4x the rate and need coefficients for it.
        // Running base-rate coefficients at 4x would put the corners two
        // octaves too low and quietly change the whole voicing.
        interHighpassCoeffOs_ = onePoleCoeffAt(120.0f,  sampleRate_ * 4.0);
        interLowpassCoeffOs_  = onePoleCoeffAt(6500.0f, sampleRate_ * 4.0);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setDrive(float drive)    { drive_.store(drive, std::memory_order_relaxed); }
    void setTone(float tone)      { tone_.store(tone, std::memory_order_relaxed); }
    void setLevel(float level)    { level_.store(level, std::memory_order_relaxed); }
    void setHardClip(bool hard)   { hard_.store(hard, std::memory_order_relaxed); }
    void setCabinet(bool on)      { cabinet_.store(on, std::memory_order_relaxed); }
    void setAsymmetry(float value) { asymmetry_.store(value, std::memory_order_relaxed); }
    void setOversample(bool on)    { oversample_.store(on, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float drive   = juce::jlimit(0.1f, 100.0f, drive_.load(std::memory_order_relaxed));
        const float tone    = juce::jlimit(0.0f, 1.0f, tone_.load(std::memory_order_relaxed));
        const float level   = juce::jlimit(0.0f, 2.0f, level_.load(std::memory_order_relaxed));
        const bool  hard    = hard_.load(std::memory_order_relaxed);
        const bool  cabinet = cabinet_.load(std::memory_order_relaxed);
        const float asym    = juce::jlimit(-1.0f, 1.0f, asymmetry_.load(std::memory_order_relaxed));
        const bool  overs   = oversample_.load(std::memory_order_relaxed);

        // Loud settings would otherwise just get louder: a drive pedal that
        // doubles as a volume control is unusable, so the make-up gain falls
        // as the drive rises. Not exact loudness matching — just enough that
        // sweeping the knob auditions the *tone* rather than the level.
        const float makeUp = level / std::sqrt(juce::jmax(1.0f, drive));

        const int numChannels = juce::jmin(buffer.getNumChannels(), (int) kMaxChannels);

        for (int channelIndex = 0; channelIndex < numChannels; ++channelIndex)
        {
            auto& state  = channels_[(size_t) channelIndex];
            auto* samples = buffer.getWritePointer(channelIndex);

            const int stages = juce::jlimit(1, kMaxStages, stages_.load(std::memory_order_relaxed));
            state.cab.setUseImpulseResponse(cabinetIr_.load(std::memory_order_relaxed));

            // Every stage gets the full drive, which is what a real cascaded
            // preamp does — its stages are not one stage's worth of gain
            // shared out, they each have their own.
            //
            // Sharing it out as the n-th root was the first attempt and it was
            // measurably wrong: three stages at 12^(1/3) barely clip at all,
            // and the "cascade" came out with a tenth of the high-order
            // content of the single stage it was supposed to enrich. A
            // cascade is not a gentler way to reach the same distortion; it is
            // more distortion, of a different shape.
            //
            // What stops that from being merely a louder fuzz is the
            // interstage filtering below: clipping is close to idempotent, so
            // a second clipper handed the first one's output would do almost
            // nothing. Reshaping the wave between stages is what gives the
            // next one something to work on, and it is where the character
            // actually comes from.
            const float perStageDrive = drive;

            for (int stage = 0; stage < stages; ++stage)
            {
                auto& shaper = state.shapers[(size_t) stage];
                shaper.setKind(hard ? Waveshaper::Kind::Hard : Waveshaper::Kind::Soft);
                shaper.setDrive(perStageDrive);
                shaper.setAsymmetry(asym);
            }

            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                float x = samples[n];

                // Pre-emphasis, as a genuine **bandpass**. The bass cut below
                // is what keeps a low string from dominating the clipper and
                // taking the rest of a chord down with it.
                //
                // The rolloff above it is the half that used to be missing:
                // `x - 0.6*LP(x)` alone is a low-*shelf* cut with unity gain
                // at high frequency, so it fed the clipper *more* treble than
                // it received, and every harmonic that generated then had to
                // be cleaned up downstream. A real high-gain boost cuts both
                // ends, specifically so the clipper is never shown fizz in
                // the first place - it is far easier not to generate it than
                // to filter it out afterwards.
                state.preState += preCoeff_ * (x - state.preState);
                x = x - 0.6f * state.preState;

                state.preTopState += preTopCoeff_ * (x - state.preTopState);
                x = state.preTopState;

                // Only the nonlinear part runs at 4x. The pre-emphasis and
                // the cabinet are linear, so oversampling them would cost the
                // same and change nothing: aliasing is generated by the
                // nonlinearity and nowhere else.
                //
                // The *whole cascade* goes inside one oversampler call rather
                // than each stage separately: the interstage filters are part
                // of the nonlinear network, and converting up and down between
                // every stage would trip through the conversion filters three
                // times for no benefit.
                const float highpassCoeff = overs ? interHighpassCoeffOs_ : interHighpassCoeff_;
                const float lowpassCoeff  = overs ? interLowpassCoeffOs_  : interLowpassCoeff_;

                const auto cascade = [&state, stages, highpassCoeff, lowpassCoeff](float v)
                {
                    for (int stage = 0; stage < stages; ++stage)
                    {
                        if (stage > 0)
                        {
                            auto& inter = state.inter[(size_t) (stage - 1)];

                            // Bass out before the next clipper sees it: the
                            // reason a cascade is tight rather than muddy.
                            inter.highpass += highpassCoeff * (v - inter.highpass);
                            v -= inter.highpass;

                            // ...and treble out, so each stage isn't handed
                            // the previous one's fizz to multiply.
                            inter.lowpass += lowpassCoeff * (v - inter.lowpass);
                            v = inter.lowpass;
                        }

                        v = state.shapers[(size_t) stage].processSample(v);
                    }

                    return v;
                };

                x = overs ? state.oversampler.process(x, cascade) : cascade(x);

                if (cabinet)
                    x = state.cab.processSample(x);

                // Post tilt: the pedal's tone knob, sweeping between darker
                // and brighter around a fixed corner.
                state.tiltState += tiltCoeff_ * (x - state.tiltState);
                const float high = x - state.tiltState;
                x = state.tiltState + high * (0.25f + 1.75f * tone);

                samples[n] = x * makeUp;
            }
        }
    }

private:
    static constexpr size_t kMaxChannels = 2;

public:
    /** Three is where a real high-gain preamp sits, and past it the stages
        stop adding character and only add noise-floor. */
    static constexpr int kMaxStages = 3;

private:

    struct ChannelState
    {
        /** One shaper per stage rather than one reused: the anti-aliasing is
            an antiderivative method and therefore *stateful* — it needs each
            stage's previous input. Sharing one would mix three stages'
            histories together and the aliasing suppression would be wrong in
            a way that only shows as a faint hiss on fast material. */
        std::array<Waveshaper, kMaxStages> shapers;

        /** Interstage coupling: a highpass and a lowpass between each pair of
            stages, i.e. one fewer than there are stages. */
        struct Interstage { float highpass = 0.0f; float lowpass = 0.0f; };
        std::array<Interstage, kMaxStages - 1> inter;

        Oversampler4x oversampler;
        CabinetSim    cab;
        float      preState    = 0.0f;
        float      preTopState = 0.0f;
        float      tiltState   = 0.0f;
    };

    float onePoleCoeff(float hz) const { return onePoleCoeffAt(hz, sampleRate_); }

    static float onePoleCoeffAt(float hz, double rate)
    {
        const float x = (float) (6.2831853 * (double) hz / (rate > 0.0 ? rate : 48000.0));
        return juce::jlimit(0.0f, 1.0f, x / (1.0f + x));
    }

    double sampleRate_ = 48000.0;
    float  preCoeff_    = 0.1f;
    float  preTopCoeff_ = 0.5f;
    float  tiltCoeff_   = 0.1f;
    float  interHighpassCoeff_   = 0.1f;
    float  interLowpassCoeff_    = 0.5f;
    float  interHighpassCoeffOs_ = 0.03f;
    float  interLowpassCoeffOs_  = 0.3f;

    std::array<ChannelState, kMaxChannels> channels_;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> drive_   { 4.0f };
    std::atomic<float> tone_    { 0.5f };
    std::atomic<float> level_   { 0.7f };
    std::atomic<bool>  hard_    { false };
    std::atomic<bool>  cabinet_ { true };
    std::atomic<float> asymmetry_  { 0.0f };
    std::atomic<int>   stages_     { 1 };
    std::atomic<bool>  cabinetIr_  { false };
    std::atomic<bool>  oversample_ { false };
};

} // namespace looper::engine
