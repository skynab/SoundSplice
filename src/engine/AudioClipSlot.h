#pragma once

#include <memory>

#include "engine/ClipData.h"

namespace looper::engine
{
/**
    One audio clip's placement on a track's timeline: decoded audio data plus
    its [startBeats, startBeats + lengthBeats) window — the audio equivalent
    of ClipSlot (MIDI). AudioFilePlayerNode is given a track's whole list of
    AudioClipSlots and plays whichever one's window covers the current
    transport position (they're expected not to overlap); playback never
    loops within a clip (unlike a MIDI pattern) — it just plays once from the
    clip's start and goes silent once the file runs out or the window ends,
    whichever comes first.

    clipData is shared (not owned outright) so the same decoded file can back
    multiple slots or tracks without re-decoding — see
    AudioEngine::setTrackAudioClips, which caches decoded audio by file path.
    A single-clip track is given an effectively unbounded lengthBeats by the
    caller so it just plays once with no window gating — the original
    single-clip behaviour, preserved as the lengthBeats==huge special case.
*/
struct AudioClipSlot
{
    std::shared_ptr<ClipData> clipData;
    double startBeats  = 0.0;
    double lengthBeats = 0.0;

    /** Linear, already converted from model::Clip::gainDb by the caller —
        the audio thread shouldn't be doing decibel conversions per block. */
    float  gain        = 1.0f;
};

} // namespace looper::engine
