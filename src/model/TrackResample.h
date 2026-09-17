#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "model/Song.h"

namespace soundsplice::model
{
/**
    Resample Track: the audio files a track plays, and pointing its clips at
    converted copies of them. A clip's offset, fades and volume curve are all
    in seconds, so a copy at another rate plays the same audio in the same
    place and nothing else about the clip changes. Session-grid clips change
    along with the arrangement's.
*/
namespace trackresample
{
    /** Each audio file track @p track plays, once, in the order first used. */
    inline std::vector<std::string> audioFilesOf(const Track& track)
    {
        std::vector<std::string> files;
        const auto note = [&files](const Clip& clip)
        {
            if (clip.type == ClipType::Audio && ! clip.audioFile.empty()
                && std::find(files.begin(), files.end(), clip.audioFile) == files.end())
                files.push_back(clip.audioFile);
        };

        for (const auto& clip : track.clips)
            note(clip);
        for (const auto& slot : track.sessionSlots)
            if (slot.hasClip)
                note(slot.clip);
        return files;
    }

    /** Points every audio clip of @p track that plays a key of @p replacements
        at its value. The number of clips changed. */
    inline int replaceAudioFiles(Track& track, const std::map<std::string, std::string>& replacements)
    {
        int        changed = 0;
        const auto replace = [&](Clip& clip)
        {
            if (clip.type != ClipType::Audio)
                return;
            const auto found = replacements.find(clip.audioFile);
            if (found == replacements.end())
                return;
            clip.audioFile = found->second;
            ++changed;
        };

        for (auto& clip : track.clips)
            replace(clip);
        for (auto& slot : track.sessionSlots)
            if (slot.hasClip)
                replace(slot.clip);
        return changed;
    }
}

} // namespace soundsplice::model
