#pragma once

#include <string>

#include <juce_core/juce_core.h>

namespace looper::app
{
/**
    Names for the files in a stem export.

    Its own header because it is pure string handling, and the ways it can go
    wrong are all quiet ones: a track called "Gtr / Lead" would try to create a
    directory, two tracks called "Audio" would overwrite each other, and a track
    with no name at all would produce a file called ".wav" that some file
    browsers simply do not show.
*/

/** The folder stems go into, beside the master file the user chose.

    A folder rather than loose siblings: a project with eight tracks otherwise
    scatters nine files into whatever directory happened to be open, and the
    stems are only meaningful as a set. */
inline juce::File stemFolderFor(const juce::File& masterFile)
{
    return masterFile.getParentDirectory()
               .getChildFile(masterFile.getFileNameWithoutExtension() + " stems");
}

/**
    One stem's file name: a 1-based, zero-padded index, the track's name, and
    the extension.

    The index is not decoration. It makes the folder sort in track order rather
    than alphabetically, and it is what keeps two tracks with the same name -
    which the app allows, and which happens the moment you add two audio tracks
    without renaming them - from landing on the same file.
*/
inline juce::String stemFileName(int oneBasedIndex, const std::string& trackName,
                                 const juce::String& extension)
{
    const auto index = juce::String(juce::jmax(1, oneBasedIndex)).paddedLeft('0', 2);

    auto name = juce::String(trackName).trim();
    if (name.isEmpty())
        name = "Track"; // "01 .wav" is a file some browsers won't show at all

    // Turns separators and anything else the filesystem objects to into
    // something safe. Applied to the name alone, so it can't eat the index or
    // the extension.
    name = juce::File::createLegalFileName(name);

    // createLegalFileName leaves a name that is *only* punctuation as empty or
    // near enough, so this is checked again rather than assumed.
    if (name.trim().isEmpty())
        name = "Track";

    return index + " " + name.trim() + "." + extension;
}

} // namespace looper::app
