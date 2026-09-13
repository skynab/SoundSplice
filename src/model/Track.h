#pragma once

#include <map>
#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Clip.h"
#include "model/DrumKit.h"
#include "model/GuitarSettings.h"
#include "model/Effects.h"
#include "model/SynthSettings.h"

namespace looper::model
{
enum class TrackType
{
    Instrument, // MIDI clips driving a synth
    Audio,      // audio-file clips
    Drum,       // MIDI clips driving a per-pad drum kit (see DrumKit)
    Guitar,     // MIDI clips driving six plucked strings (see engine::GuitarNode)

    /**
        A group bus: a track that *receives* other tracks' output instead of
        generating any of its own.

        Deliberately a track type rather than a separate Bus entity alongside
        Song::tracks. A bus needs a fader, pan, mute, a meter, an insert chain,
        automation and a mixer strip — every one of which a Track already has
        and every one of which would otherwise have to be built again, along
        with a second selection model for the panes to understand. What makes a
        bus different is only where its audio comes from, and that is one field
        (Track::outputBusId on its members) rather than a parallel hierarchy.
    */
    Bus
};

/** Which of a track's parameters an automation lane drives (see
    Track::automation). Stored as the map's key, so adding an automatable
    parameter is a new enumerator plus the code that applies it — not a new
    field on Track, a new serialization record and a new playback branch, as
    it was when gain was the only one.

    The numeric values are written to the project file, so they are part of
    the format: append, never renumber. */
enum class TrackParam
{
    Gain      = 0, // dB
    Pan       = 1, // -1..+1
    SendLevel = 2  // 0..1
};

/** One cell of the session grid: a clip, or nothing. A vector of these on a
    track is indexed by scene, so an empty slot has to be representable rather
    than simply absent — the index *is* the scene.

    The clip's startBeats is meaningless here and ignored: a session clip has
    no timeline position, only a slot and a length to loop on. */
struct SessionSlot
{
    bool hasClip = false;
    Clip clip;

    bool operator==(const SessionSlot&) const = default;
};

struct Track
{
    int               id     = 0;
    std::string       name;
    TrackType         type   = TrackType::Instrument;
    float             gainDb     = 0.0f;
    float             pan        = 0.0f; // -1 = hard left, 0 = centre, +1 = hard right
    bool              muted      = false;
    bool              solo       = false;
    float             sendLevel  = 0.0f; // 0..1, pre-fader send to the shared send bus

    // ARGB, or 0 for the default lane colour. Stored as the value rather than
    // as an index into the palette so extending or reordering that palette
    // can't silently recolour existing projects.
    unsigned int      colour     = 0;
    /** The id of the Bus track this one feeds, or -1 for the master.

        An id rather than an index, for the same reason a sidechain source is
        one: indices move when a track is deleted or reordered, and a track
        silently re-routing itself into a different group would be a bug nobody
        would think to look for. -1 by default, so every existing track goes
        straight to the master exactly as it always has. */
    int               outputBusId = -1;

    std::vector<Clip> clips;

    // The session grid's column for this track, indexed by scene. Kept the
    // same length as Song::scenes (see model::addScene). Deliberately a
    // separate container from `clips` rather than a flag on them: the
    // arrangement is a sequence of placements, the session is a grid of
    // alternatives, and merging the two would put a meaningless startBeats on
    // every session clip. A track plays from one or the other, never both.
    std::vector<SessionSlot> sessionSlots;

    // Automation lanes, keyed by TrackParam. A parameter with no lane (or an
    // empty one) simply uses its static value, which is why an unautomated
    // track carries no lanes at all rather than a set of empty ones.
    std::map<int, AutomationLane> automation;
    DrumKit           drumKit; // only meaningful when type == Drum; empty pads otherwise
    SynthSettings     synthSettings; // only meaningful when type == Instrument
    GuitarSettings    guitarSettings; // only meaningful when type == Guitar

    // This track's insert effects, in order, applied to its output before the
    // fader (and so before its send too). A slot is a built-in or a hosted
    // plugin — see model::EffectSlot. Empty by default, so a track that has
    // never been touched sounds exactly as it did before inserts existed.
    //
    // This replaced a fixed filter/delay/reverb trio when plugin hosting
    // arrived: a hosted plugin is an effect in the same chain as the
    // built-ins, and keeping them apart would have meant two effect concepts
    // each needing their own ordering, bypass, serialization and UI. Older
    // projects migrate into three slots in the original order (see
    // model::deserialize), so they keep sounding the same.
    std::vector<EffectSlot> effectChain;

    /** The first slot of @p kind, or nullptr. The engine still applies one
        built-in of each kind (a variable-length chain is stage 2 of §20), so
        this is how it finds them. */
    const EffectSlot* firstEffect(EffectKind kind) const
    {
        for (const auto& slot : effectChain)
            if (slot.kind == kind)
                return &slot;
        return nullptr;
    }

    bool operator==(const Track&) const = default;

    /** The lane driving @p param, or nullptr if that parameter isn't
        automated. Read side — never creates a lane, so merely asking doesn't
        change the document. */
    const AutomationLane* lane(TrackParam param) const
    {
        const auto it = automation.find((int) param);
        return (it != automation.end() && ! it->second.empty()) ? &it->second : nullptr;
    }

    /** The lane driving @p param, creating an empty one if needed. Write
        side, for recording automation. */
    AutomationLane& laneFor(TrackParam param) { return automation[(int) param]; }

    /** True if any parameter on this track is automated. */
    bool hasAutomation() const
    {
        for (const auto& [param, lane] : automation)
            if (! lane.empty())
                return true;
        return false;
    }
};

} // namespace looper::model
