#pragma once

#include <map>
#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Clip.h"
#include "model/Effects.h"
#include "model/SynthSettings.h"

namespace soundsplice::model
{
/** The numeric values are written to the project file, so they are part of
    the format: append, never renumber. */
enum class TrackType
{
    Instrument = 0, // MIDI clips driving a synth
    Audio      = 1  // audio-file clips
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
    Pan       = 1  // -1..+1
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

    // ARGB, or 0 for the default lane colour. Stored as the value rather than
    // as an index into the palette so extending or reordering that palette
    // can't silently recolour existing projects.
    unsigned int      colour     = 0;

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
    SynthSettings     synthSettings; // only meaningful when type == Instrument

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

} // namespace soundsplice::model
