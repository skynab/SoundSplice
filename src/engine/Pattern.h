#pragma once

#include <vector>

namespace looper::engine
{
/** One note in a pattern. Times are in quarter-note beats from the pattern start. */
/** How a note is played, as distinct from how hard.

    Velocity could have carried this - quiet notes treated as muted - and that
    would have cost nothing to build. It was rejected because it conflates two
    independent things: a riff wants loud palm mutes and quiet open notes, and
    a threshold makes both impossible.

    Guitar tracks are the only instrument that acts on this today; everything
    else ignores it, which is why the default has to be the behaviour that
    already existed. */
enum class Articulation : int
{
    Normal = 0,

    /** Palm muted: the picking hand rests on the strings at the bridge, so the
        note is short and dark rather than merely quiet. See engine::GuitarNode
        for what that means in the string model, and model::GuitarSettings for
        the two values that shape it. */
    PalmMute = 1
};

struct Note
{
    double       startBeats   = 0.0;
    double       lengthBeats  = 0.25;
    int          noteNumber   = 60;
    float        velocity     = 0.8f; // 0..1
    Articulation articulation = Articulation::Normal;

    bool operator==(const Note&) const = default;
};

/**
    A sustain-pedal movement.

    The first thing in this engine that is a performance event but *not* a
    note. It lives on the pattern rather than being inferred from note lengths
    because it genuinely is independent: a pianist holds the pedal across
    phrases whose notes have long since been released, and that is the entire
    point of it — the notes keep ringing after the keys come up.

    Down and up rather than a continuous position: half-pedalling is real and
    deliberately out of scope (see docs/PLAN.md §34), and a bool cannot be
    mistaken for supporting it.
*/
struct PedalEvent
{
    double beat = 0.0;
    bool   down = false;

    bool operator==(const PedalEvent&) const = default;
};

/** A looping bar of notes. JUCE-free so it can be copied and snapshotted freely. */
struct Pattern
{
    std::vector<Note> notes;

    /** Sustain-pedal movements, in beats from the pattern start. Empty on
        every pattern that does not use one, which is all of them until a piano
        part is written — so nothing that existed before this pays for it. */
    std::vector<PedalEvent> pedals;

    double            lengthBeats = 4.0; // one 4/4 bar

    bool operator==(const Pattern&) const = default;
};

} // namespace looper::engine
