#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "engine/SequencerMath.h"
#include "model/Song.h"

namespace looper
{
/** One audio clip whose stored length didn't match its file, and what it was
    changed to. */
struct ClipLengthFix
{
    int         trackIndex     = -1;
    int         clipIndex      = -1;
    std::string audioFile;
    double      oldLengthBeats = 0.0;
    double      newLengthBeats = 0.0;
};

/**
    Re-measures every audio clip's length from its file and corrects it where
    the two disagree.

    This exists because of a bug, now fixed, where a freshly recorded take was
    always given a flat four beats regardless of how long the take actually
    ran: songEndBeats summed clip.startBeats + clip.lengthBeats, so a ten-bar
    recording was reported as one bar long and the transport wrapped seconds
    into it. Fixing the record path stops it happening again, but it does
    nothing for a project saved while the bug was still there — those clips
    keep whatever wrong length they were given until something re-measures
    them.

    An audio clip's length is derived, not chosen: importAudioFileAtBeat and
    the (now fixed) recording path both set it from the file's own duration.
    So any audio clip whose length disagrees with its file is wrong on that
    basis alone, whether or not the mismatch happens to be exactly four beats
    — checking for "four beats exactly" would both miss a take that was
    genuinely four beats short before the bug and, on a different bpm, would
    have produced some other wrong number rather than four.

    @p probeSeconds is a callable from an audio file path to its duration in
    seconds (0 or negative if it can't be read), injected so this can be
    tested without real audio files or a JUCE audio format manager.

    A clip whose file can't be probed is left untouched rather than guessed
    at — a missing or moved file is a different problem, and silently
    replacing its length with a fallback would hide that it happened.
*/
template <typename ProbeSeconds>
std::vector<ClipLengthFix> repairAudioClipLengths(model::Song& song, ProbeSeconds probeSeconds,
                                                   double toleranceBeats = 1.0e-3)
{
    std::vector<ClipLengthFix> fixes;

    for (int t = 0; t < (int) song.tracks.size(); ++t)
    {
        auto& track = song.tracks[(size_t) t];
        for (int c = 0; c < (int) track.clips.size(); ++c)
        {
            auto& clip = track.clips[(size_t) c];
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;

            const double seconds = probeSeconds(clip.audioFile);
            if (seconds <= 0.0)
                continue; // unreadable or missing file; not this function's problem to solve

            const double measured = engine::beatsForSeconds(seconds, song.bpm);
            if (measured <= 0.0)
                continue;

            if (std::abs(measured - clip.lengthBeats) > toleranceBeats)
            {
                fixes.push_back({ t, c, clip.audioFile, clip.lengthBeats, measured });
                clip.lengthBeats = measured;
            }
        }
    }

    return fixes;
}

} // namespace looper
