#pragma once

#include <array>

namespace looper::engine
{
/** A small, fixed set of one-click kit characters for a Drum track — the
    drum counterpart to GuitarTone and SynthTone. */
enum class DrumKitStyle
{
    Classic,    // the starting kit: generic, unmistakable kick/snare/hat/clap
    Industrial, // harder and dirtier - the cyberpunk end
    Midtempo    // deep and slow-decaying, for halftime grooves
};

constexpr int kNumDrumKitStyles = 3;

/** Display name for @p style, as shown on the drums pane's kit buttons. */
inline const char* drumKitStyleName(DrumKitStyle style)
{
    switch (style)
    {
        case DrumKitStyle::Classic:    return "Classic";
        case DrumKitStyle::Industrial: return "Industrial";
        case DrumKitStyle::Midtempo:   return "Midtempo";
    }
    return "Classic";
}

/** One pad of a kit style.

    Carries a sample *stem* rather than a path: a file path is exactly the
    kind of thing neither this layer nor the model layer knows about (see
    MainComponent::factoryDrumKitDirectory, which is what resolves these).
    That keeps the table JUCE-free and headlessly testable, which matters
    because the invariant below is not one you'd notice breaking by ear. */
struct DrumKitStylePad
{
    int         noteNumber;
    const char* label;
    const char* sampleStem;
    float       gainDb;
    float       pitchSemitones;
};

inline constexpr int kDrumKitStylePads = 4;

/**
    The four pads for @p style.

    Every style deliberately uses the *same* four note numbers and labels,
    and varies only which sample each pad plays and how it's trimmed. That
    isn't a coincidence worth preserving loosely — it's load-bearing:

      - Clips store raw MIDI note numbers, so a kit that renumbered its pads
        would silently orphan every note already written against the track.
        The notes would still be in the clip; nothing would play them.
      - engine::generateDrumLoop finds its kick/snare/hat by pad *label*
        (see MainComponent's noteForLabel), so renaming them would quietly
        send generated patterns to the wrong pad.

    Both failures are silent, which is why this is pinned by a test rather
    than left to whoever adds the next style.
*/
inline std::array<DrumKitStylePad, kDrumKitStylePads> padsForDrumKitStyle(DrumKitStyle style)
{
    switch (style)
    {
        case DrumKitStyle::Classic:
            return { {
                { 36, "Kick",  "Kick Tight",    0.0f, 0.0f },
                { 38, "Snare", "Snare Crisp",   0.0f, 0.0f },
                { 42, "Hat",   "Hat Closed",    0.0f, 0.0f },
                { 45, "Other", "Clap Classic",  0.0f, 0.0f },
            } };

        case DrumKitStyle::Industrial:
            // Hard and metallic: the driven kick, the noisy snare, and the
            // tightest hat, with the hat pitched up and pulled back so it
            // reads as a machine tick rather than a cymbal.
            return { {
                { 36, "Kick",  "Kick Industrial",  0.0f,  0.0f },
                { 38, "Snare", "Snare Industrial", 0.0f, -1.0f },
                { 42, "Hat",   "Hat Tight",       -3.0f,  4.0f },
                { 45, "Other", "Clap Tight",      -1.0f,  0.0f },
            } };

        case DrumKitStyle::Midtempo:
            // Deep and long: at halftime there's room for the kick to
            // actually decay, and pitching it down is most of what makes a
            // 100bpm groove feel heavy rather than merely slow.
            return { {
                { 36, "Kick",  "Kick Sub",       1.0f, -2.0f },
                { 38, "Snare", "Snare Fat",      0.0f, -1.0f },
                { 42, "Hat",   "Hat Closed",    -2.0f,  0.0f },
                { 45, "Other", "Clap Roomy",     0.0f,  0.0f },
            } };
    }

    return padsForDrumKitStyle(DrumKitStyle::Classic);
}

} // namespace looper::engine
