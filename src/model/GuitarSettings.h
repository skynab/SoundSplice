#pragma once

#include <array>

namespace looper::model
{
/** How many strings the instrument has. Mirrors engine::kNumGuitarStrings;
    `model` can't include the engine's guitar header, and six is not a number
    either side should be guessing at. */
inline constexpr int kNumGuitarStrings = 6;

/**
    Per-track guitar settings (model::Track::guitarSettings) — the model-side
    counterpart to engine::GuitarNode, the same way SynthSettings pairs with
    SynthInstrumentNode.

    Tuning is stored as MIDI note numbers rather than frequencies, because
    that's how players describe it: standard is E2 A2 D3 G3 B3 E4, and drop-D
    is one number changed.

    Every default is chosen so a freshly added guitar track sounds like a
    guitar without touching anything.
*/
struct GuitarSettings
{
    std::array<int, kNumGuitarStrings> tuning { 40, 45, 50, 55, 59, 64 };

    float decaySeconds = 3.0f;  // T60 at the fundamental
    float brightness   = 0.7f;  // 0 = dull, 1 = bright
    float pickPosition = 0.22f; // 0 = at the bridge, 0.5 = middle of the string
    float pickHardness = 0.6f;  // 0 = fingertip, 1 = plectrum

    // 0 leaves strings ringing after a note-off, which is what a guitar does;
    // 1 stops them dead. See engine::GuitarNode for why ringing is the default.
    float muteOnNoteOff = 0.0f;

    // The pickup's electrical resonance - see engine::Pickup. A magnetic
    // pickup is an RLC circuit with a peak at 2-3kHz (humbucker) or 4-6kHz
    // (single coil) and a 12dB/oct rolloff above it, and that peak is most of
    // what makes an electric guitar sound electric rather than like a plucked
    // string. Nothing in this signal path produced a resonance before it.
    float pickupResonanceHz = 3000.0f;
    float pickupQ           = 1.4f;

    // What a palm-muted note is, rather than how it differs from an open one -
    // absolute values so a preset states the sound directly. A palm mute is
    // short *and* dark: the picking hand resting at the bridge kills the tail
    // and rolls off the top, and a note that was only shortened reads as cut
    // off rather than chugged. See engine::Articulation.
    /** How much a note's velocity brightens it, on top of its loudness — see
        engine::GuitarString::setVelocitySensitivity. Non-zero by default:
        every note sounding timbrally identical was the loudest complaint the
        instrument had, and 0 restores exactly the old behaviour. */
    float velocitySensitivity = 0.5f;

    /** String stiffness, 0..1 — how far the partials stretch sharp (see
        engine::GuitarString::setStiffness). Real on any string and strongest
        on thick wound ones, which is why GuitarNode scales it down as the
        strings get thinner. */
    float stiffness = 0.4f;

    /** Energy crossing between strings at the bridge, 0..1 — sympathetic
        ringing, and most of what makes a chord bloom. */
    float stringCoupling = 0.35f;

    /** How far the strings are spread across the stereo field, 0..1.
        Mono-compatible at any setting (see engine::GuitarNode::setWidth). */
    float stereoWidth = 0.3f;

    float palmMuteDecaySeconds = 0.18f;
    float palmMuteBrightness   = 0.25f;

    bool operator==(const GuitarSettings&) const = default;
};

} // namespace looper::model
