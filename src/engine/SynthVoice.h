#pragma once

#include <algorithm>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/MidiNote.h"
#include "engine/Oscillator.h"
#include "engine/StateVariableFilter.h"

namespace looper::engine
{
/** The sound every SynthVoice can play. */
struct SynthSound final : juce::SynthesiserSound
{
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

/** Oscillator waveform choices. At namespace scope (not nested in SynthVoice)
    so SynthVoiceSettings below can reference it before SynthVoice itself is
    declared; SynthVoice::Waveform stays valid via the alias in that class,
    so every existing call site spelling it that way keeps compiling. */
enum class Waveform { Sine, Saw, Square, Triangle };

/** Unison voices live in a fixed array, never a vector — no allocation on
    the audio thread, the rule every other per-voice DSP in this engine
    already follows. */
inline constexpr int kMaxUnisonVoices = 7;

/**
    Everything SynthInstrumentNode pushes into a SynthVoice once per block.

    A plain aggregate rather than applySettings' previous 7 positional
    parameters, which was already cramped before the filter-envelope,
    sub-oscillator and unison fields below needed to join it — a call site
    with a dozen same-typed positional arguments is exactly the "which bool
    was that again" mistake a named struct avoids.
*/
struct SynthVoiceSettings
{
    Waveform                waveform = Waveform::Sine;
    juce::ADSR::Parameters  adsr;

    bool  filterEnabled   = false;
    int   filterMode      = 0;
    float filterCutoff    = 1000.0f;
    float filterResonance = 0.707f;

    // Filter envelope: sweeps the filter's cutoff over the note by
    // filterEnvAmount (Hz, bipolar) scaled by a *second*, independent ADSR —
    // real analog synths run one envelope per destination, and reusing the
    // amp envelope for both would tie "how long the note is loud" to "how
    // bright it is," which is only true by coincidence. Amount 0 (the
    // default) means the cutoff never moves, whatever this ADSR is doing.
    float                   filterEnvAmount = 0.0f;
    juce::ADSR::Parameters  filterEnvAdsr;

    // A sub-oscillator: a plain sine one octave down, mixed in for low-end
    // weight. Not a second selectable waveform — a sine adds no harmonic
    // content of its own to alias or clash against the main oscillator,
    // which is why analog bass subs are almost always exactly this.
    bool  subOscEnabled = false;
    float subOscLevel   = 0.3f;

    // unisonVoices copies of the main oscillator, detuned symmetrically
    // across unisonDetuneCents and summed. <= 1 means "off," and takes
    // SynthVoice's original single-oscillator code path unchanged (see
    // renderNextBlock's fast path).
    int   unisonVoices      = 1;
    float unisonDetuneCents = 12.0f;

    float outputGain = 1.0f;
};

/**
    A single polyphonic voice: an oscillator (selectable waveform, optional
    unison stack and sub-oscillator) shaped by an ADSR envelope, optionally
    through its own state-variable filter whose cutoff can itself be swept by
    a second, independent ADSR — the per-track timbre a SynthInstrumentNode's
    voices all share (see SynthInstrumentNode::refreshVoiceSettings, which
    pushes the owning track's model::SynthSettings into every voice once per
    block, the same "refreshed once per block" convention FilterEffect uses
    for the master filter). juce::Synthesiser owns a pool of these and
    handles note allocation, voice stealing, and sample-accurate MIDI
    dispatch.

    renderNextBlock takes one of two paths. The **fast path** — no unison, no
    sub-oscillator, no filter-envelope amount, i.e. every existing project's
    settings — is the exact code this class always had, kept verbatim so
    those settings' output can't drift by so much as a rounding bit (this
    project treats several bounce-tool renders, e.g. rmsDry, as exact
    regression sentinels). The **general path** is only reached once a preset
    actually asks for one of the new capabilities.
*/
class SynthVoice final : public juce::SynthesiserVoice
{
public:
    using Waveform = engine::Waveform; // keeps "SynthVoice::Waveform" spellings valid

    void setADSR(const juce::ADSR::Parameters& params) { adsr_.setParameters(params); }

    /** Pushes this track's current timbre into the voice. Cheap (a handful of
        field copies, no allocation) — called every block regardless of
        whether anything changed, same as FilterEffect::process. */
    void applySettings(const SynthVoiceSettings& settings) noexcept
    {
        waveform_          = settings.waveform;
        adsr_.setParameters(settings.adsr);

        filterEnabled_     = settings.filterEnabled;
        filter_.setMode((StateVariableFilter::Mode) settings.filterMode);
        baseFilterCutoff_  = settings.filterCutoff;
        filter_.setCutoff(baseFilterCutoff_); // the fast path's only cutoff write; the
                                               // general path overwrites this every sample
        filter_.setResonance(settings.filterResonance);

        filterEnvAmount_   = settings.filterEnvAmount;
        filterEnv_.setParameters(settings.filterEnvAdsr);

        subOscEnabled_     = settings.subOscEnabled;
        subOscLevel_       = settings.subOscLevel;

        unisonVoices_      = juce::jlimit(1, kMaxUnisonVoices, settings.unisonVoices);
        unisonDetuneCents_ = settings.unisonDetuneCents;

        outputGain_        = settings.outputGain;
    }

    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SynthSound*>(sound) != nullptr;
    }

    void startNote(int midiNote, float velocity, juce::SynthesiserSound*, int /*pitchWheel*/) override
    {
        phase_     = 0.0;
        level_     = velocity;
        frequency_ = midiNoteToHertz(midiNote);
        adsr_.noteOn();
        filterEnv_.noteOn();

        subPhase_ = 0.0;
        for (auto& p : unisonPhases_)
            p = 0.0;
    }

    void stopNote(float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr_.noteOff();
            filterEnv_.noteOff();
        }
        else
        {
            adsr_.reset();
            filterEnv_.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void setCurrentPlaybackSampleRate(double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate(newRate);
        if (newRate > 0.0)
        {
            adsr_.setSampleRate(newRate);
            filterEnv_.setSampleRate(newRate);
            filter_.prepare(newRate);
        }
    }

    void renderNextBlock(juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr_.isActive())
            return;

        // Held as a member so renderWaveform() can see it: PolyBLEP needs to
        // know how far one sample advances the phase to size its correction.
        increment_ = juce::MathConstants<double>::twoPi * frequency_ / getSampleRate();

        const bool useGeneralPath = unisonVoices_ > 1 || subOscEnabled_ || filterEnvAmount_ != 0.0f;

        if (! useGeneralPath)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float env = adsr_.getNextSample();
                float       sample = renderWaveform() * env * level_ * 0.3f * outputGain_;
                if (filterEnabled_)
                    sample = filter_.processSample(sample);

                phase_ += increment_;
                if (phase_ >= juce::MathConstants<double>::twoPi)
                    phase_ -= juce::MathConstants<double>::twoPi;

                for (int ch = output.getNumChannels(); --ch >= 0;)
                    output.addSample(ch, startSample + i, sample);

                if (! adsr_.isActive())
                {
                    clearCurrentNote();
                    break;
                }
            }
            return;
        }

        refreshGeneralPathIncrements();

        for (int i = 0; i < numSamples; ++i)
        {
            const float env       = adsr_.getNextSample();
            const float filterEnv = filterEnv_.getNextSample();

            float oscSum = 0.0f;
            for (int u = 0; u < unisonVoices_; ++u)
                oscSum += Oscillator::sample(oscillatorWaveformFor(waveform_), unisonPhases_[u], unisonIncrements_[u]);
            oscSum /= (float) unisonVoices_;

            if (subOscEnabled_)
                oscSum += subOscLevel_ * Oscillator::sample(Oscillator::Waveform::Sine, subPhase_, subIncrement_);

            float sample = oscSum * env * level_ * 0.3f * outputGain_;

            if (filterEnabled_)
            {
                const float modulatedCutoff =
                    std::clamp(baseFilterCutoff_ + filterEnvAmount_ * filterEnv, 20.0f, 20000.0f);
                filter_.setCutoff(modulatedCutoff);
                sample = filter_.processSample(sample);
            }

            for (int u = 0; u < unisonVoices_; ++u)
            {
                unisonPhases_[u] += unisonIncrements_[u];
                if (unisonPhases_[u] >= 1.0)
                    unisonPhases_[u] -= 1.0;
            }
            subPhase_ += subIncrement_;
            if (subPhase_ >= 1.0)
                subPhase_ -= 1.0;

            for (int ch = output.getNumChannels(); --ch >= 0;)
                output.addSample(ch, startSample + i, sample);

            if (! adsr_.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    /** Band-limited waveform generation (see engine::Oscillator, where the
        anti-aliasing lives and is measured). Used by the fast path only.

        The sine case deliberately still reads the radian phase directly
        rather than going through Oscillator: it is the default waveform, and
        computing it as sin(2*pi*normalised) instead would round differently
        and shift every existing project's output — including the bounce
        tool's rmsDry sentinel — for no audible gain. Sine has no harmonics to
        band-limit, so there is nothing to gain by routing it through. */
    float renderWaveform() const noexcept
    {
        if (waveform_ == Waveform::Sine)
            return (float) std::sin(phase_);

        constexpr double twoPi = juce::MathConstants<double>::twoPi;
        const double normalisedPhase     = phase_ / twoPi;
        const double normalisedIncrement = increment_ / twoPi;

        switch (waveform_)
        {
            case Waveform::Saw:
                return Oscillator::sample(Oscillator::Waveform::Saw, normalisedPhase, normalisedIncrement);
            case Waveform::Square:
                return Oscillator::sample(Oscillator::Waveform::Square, normalisedPhase, normalisedIncrement);
            case Waveform::Triangle:
                return Oscillator::sample(Oscillator::Waveform::Triangle, normalisedPhase, normalisedIncrement);
            case Waveform::Sine:
                break; // handled above
        }
        return 0.0f;
    }

    static Oscillator::Waveform oscillatorWaveformFor(Waveform w) noexcept
    {
        switch (w)
        {
            case Waveform::Sine:     return Oscillator::Waveform::Sine;
            case Waveform::Saw:      return Oscillator::Waveform::Saw;
            case Waveform::Square:   return Oscillator::Waveform::Square;
            case Waveform::Triangle: return Oscillator::Waveform::Triangle;
        }
        return Oscillator::Waveform::Sine;
    }

    /** Recomputes every unison voice's (and the sub-oscillator's) per-sample
        phase increment from the current note frequency — once per block,
        the same "increment refreshed each render call, phase persists
        across calls" pattern the fast path's own increment_ already uses.
        Detune is spread symmetrically about the fundamental, so a single
        active voice (unisonVoices == 1, reached here only because
        sub-osc/filter-envelope pulled in the general path) always lands at
        exactly 0 cents — i.e. the plain fundamental, undetuned. */
    void refreshGeneralPathIncrements() noexcept
    {
        const double sampleRate = getSampleRate();
        if (sampleRate <= 0.0)
            return;

        for (int u = 0; u < unisonVoices_; ++u)
        {
            const double centered = unisonVoices_ > 1
                ? ((double) u / (double) (unisonVoices_ - 1) - 0.5) * 2.0 // -1..1 across the stack
                : 0.0;
            const double cents = centered * (double) unisonDetuneCents_;
            const double freq  = frequency_ * std::pow(2.0, cents / 1200.0);
            unisonIncrements_[u] = freq / sampleRate; // normalised (0..1) phase per sample
        }
        subIncrement_ = (frequency_ * 0.5) / sampleRate; // one octave down
    }

    juce::ADSR adsr_;
    double     phase_     = 0.0;
    double     increment_ = 0.0; // radians per sample, refreshed each block (fast path only)
    double     frequency_ = 440.0;
    float      level_     = 0.0f;

    Waveform             waveform_      = Waveform::Sine;
    bool                 filterEnabled_ = false;
    StateVariableFilter  filter_;
    float                baseFilterCutoff_ = 1000.0f;
    float                outputGain_       = 1.0f;

    juce::ADSR filterEnv_;
    float      filterEnvAmount_ = 0.0f;

    bool   subOscEnabled_ = false;
    float  subOscLevel_   = 0.3f;
    double subPhase_      = 0.0; // normalised 0..1 (general path only)
    double subIncrement_  = 0.0;

    int    unisonVoices_      = 1;
    float  unisonDetuneCents_ = 12.0f;
    double unisonPhases_[kMaxUnisonVoices]     = {}; // normalised 0..1 (general path only)
    double unisonIncrements_[kMaxUnisonVoices] = {};
};

} // namespace looper::engine
