#pragma once

#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "model/Song.h"

namespace soundsplice::app::media
{
/**
    A saved project's own audio: the "<Name> Audio" folder beside its
    .soundsplice file.

    Recordings and edits belong to a project, but used to be written to two
    app-wide folders under Documents and referred to by full path, so a
    project couldn't be moved or zipped without breaking, and superseded edit
    files piled up forever. Now:

      - once a project is saved, new recordings and edits go into its audio
        folder;
      - saving collects the ones still in the app's scratch folders (or in
        another project's audio folder, after Save As) into it;
      - the project file refers to anything inside its own folder by a
        relative path with '/' separators, so the whole folder moves, between
        machines and systems too; audio elsewhere (an imported file the user
        keeps in their own library) is still referred to where it is;
      - audio files in the folder that nothing uses any more are moved to the
        trash on save.

    In memory every path stays absolute; relative paths exist only in the file.
    Uses juce::File rather than std::filesystem because a std::string path on
    Windows is read in the ANSI code page, which mangles any name outside it.
*/

/** The audio folder for a project saved at @p projectFile. */
inline juce::File audioFolderFor(const juce::File& projectFile)
{
    return projectFile.getSiblingFile(projectFile.getFileNameWithoutExtension() + " Audio");
}

inline juce::File fileFromPath(const std::string& path)
{
    return juce::File(juce::String::fromUTF8(path.c_str()));
}

inline std::string pathOf(const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

/** Calls @p fn with every audio clip's path in @p song, arrangement and
    session grid alike: `std::string&` for a mutable song, `const std::string&`
    for a const one. */
template <typename SongType, typename Fn>
void forEachAudioPath(SongType& song, Fn&& fn)
{
    for (auto& track : song.tracks)
    {
        for (auto& clip : track.clips)
            if (clip.type == model::ClipType::Audio && ! clip.audioFile.empty())
                fn(clip.audioFile);

        for (auto& slot : track.sessionSlots)
            if (slot.hasClip && slot.clip.type == model::ClipType::Audio && ! slot.clip.audioFile.empty())
                fn(slot.clip.audioFile);
    }
}

/** How @p audio is written into a project file saved at @p projectFile:
    relative to the project's folder, with '/' separators, when it lies inside
    that folder; its full path otherwise. */
inline std::string storedPath(const juce::File& audio, const juce::File& projectFile)
{
    const auto folder = projectFile.getParentDirectory();
    if (audio.isAChildOf(folder))
        return audio.getRelativePathFrom(folder).replaceCharacter('\\', '/').toStdString();
    return pathOf(audio);
}

/** The file a stored path names, for a project file at @p projectFile. Full
    paths (every project saved before relative paths existed) are used as they
    are. */
inline juce::File resolvedPath(const std::string& stored, const juce::File& projectFile)
{
    const auto text = juce::String::fromUTF8(stored.c_str());
    if (juce::File::isAbsolutePath(text))
        return juce::File(text);

    return projectFile.getParentDirectory().getChildFile(
        text.replaceCharacter('/', juce::File::getSeparatorChar()));
}

/** @p song as it should be written to @p projectFile. */
inline model::Song withStoredPaths(model::Song song, const juce::File& projectFile)
{
    forEachAudioPath(song, [&projectFile](std::string& path) { path = storedPath(fileFromPath(path), projectFile); });
    return song;
}

/** @p song as read from @p projectFile, with every path made absolute again. */
inline model::Song withResolvedPaths(model::Song song, const juce::File& projectFile)
{
    forEachAudioPath(song, [&projectFile](std::string& path) { path = pathOf(resolvedPath(path, projectFile)); });
    return song;
}

/** Whether saving into @p projectAudioFolder should take a copy of @p audio:
    it isn't in that folder yet, and it is in one of @p ownedFolders, the
    folders holding audio the app made rather than audio the user brought. */
inline bool shouldCollect(const juce::File& audio, const juce::File& projectAudioFolder,
                          const std::vector<juce::File>& ownedFolders)
{
    if (audio.isAChildOf(projectAudioFolder))
        return false;

    for (const auto& folder : ownedFolders)
        if (audio.isAChildOf(folder))
            return true;

    return false;
}

/** A copy of @p source in @p folder. A file already there with the same name
    and identical contents is reused, so saving twice doesn't copy twice; one
    with the same name but different contents is never overwritten. Returns
    @p source itself if it can't be copied, so a failure leaves the project
    pointing at audio that still plays. */
inline juce::File collectInto(const juce::File& source, const juce::File& folder)
{
    if (source.isAChildOf(folder) || ! source.existsAsFile() || folder.createDirectory().failed())
        return source;

    auto target = folder.getChildFile(source.getFileName());
    if (target.existsAsFile())
    {
        if (target.getSize() == source.getSize() && target.hasIdenticalContentTo(source))
            return target;

        target = target.getNonexistentSibling(false);
    }

    return source.copyFileTo(target) ? target : source;
}

/** Audio files directly inside @p folder that aren't in @p referenced. Other
    files, and anything in subfolders, are never reported. */
inline juce::Array<juce::File> unusedAudioFiles(const juce::File& folder, const juce::Array<juce::File>& referenced)
{
    juce::Array<juce::File> unused;
    if (! folder.isDirectory())
        return unused;

    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3",
                                                           juce::File::findFiles))
        if (! referenced.contains(entry.getFile()))
            unused.add(entry.getFile());

    return unused;
}

} // namespace soundsplice::app::media
