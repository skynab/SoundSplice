#pragma once

#include <algorithm>

#include "engine/StateVariableFilter.h"

namespace looper::engine
{
/**
    A magnetic guitar pickup's electrical response.

    The single most consequential thing missing from this signal path. A
    pickup is an inductor (2–8 H) loaded by cable and pot capacitance, which
    makes an RLC circuit with a **resonant peak** — around 2–3 kHz for a
    humbucker, 4–6 kHz for a single coil — and a 12 dB/octave rolloff above
    it. That peak is what the amp's front end then amplifies, and it is most
    of why an electric guitar sounds electric rather than like a harmonic
    series through a clipper.

    Before this, every filter between the string and the speaker in this
    codebase was a first-order real pole: `GuitarString`'s loop damper,
    `DriveEffect`'s pre-emphasis and tilt, and `CabinetSim`'s corners. Every Q
    was 0.5, so no peak existed anywhere in the chain at all.

    A resonant lowpass *is* that response, so this is `StateVariableFilter`
    used correctly rather than new DSP: 2-pole TPT, peak height set by
    resonance, 12 dB/octave above the corner.

    Applied to the summed output of all six strings, because one pickup senses
    the whole instrument — not per string.
*/
class Pickup
{
public:
    void prepare(double sampleRate) noexcept
    {
        filter_.prepare(sampleRate);
        filter_.setMode(StateVariableFilter::Mode::LowPass);
        apply();
        reset();
    }

    void reset() noexcept { filter_.reset(); }

    /** Where the resonance sits. Humbucker territory is 2–3kHz, single coil
        4–6kHz; the difference between them really is mostly this number. */
    void setResonanceHz(float hz) noexcept
    {
        resonanceHz_ = std::clamp(hz, 500.0f, 8000.0f);
        apply();
    }

    /** How pronounced the peak is. Around 1 is a gentle lift; 2 and above is
        the spiky, cutting character of a bright pickup into a lot of gain. */
    void setQ(float q) noexcept
    {
        q_ = std::clamp(q, 0.5f, 4.0f);
        apply();
    }

    /** Off means the string's output goes to the amp untouched — which is
        what this codebase did before, and is kept so the change can be
        measured against it rather than assumed. */
    void setEnabled(bool on) noexcept { enabled_ = on; }

    float processSample(float input) noexcept
    {
        return enabled_ ? filter_.processSample(input) : input;
    }

private:
    void apply() noexcept
    {
        filter_.setCutoff(resonanceHz_);
        filter_.setResonance(q_);
    }

    StateVariableFilter filter_;
    float               resonanceHz_ = 3000.0f;
    float               q_           = 1.4f;
    bool                enabled_     = true;
};

} // namespace looper::engine
