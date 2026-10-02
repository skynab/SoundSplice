#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace soundsplice::app
{
/** Where the app keeps its settings - devices, favorites, macros, shortcuts.
    Shared with soundsplice-cli, which reads the macros saved there. */
inline juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "SoundSplice";
    opts.filenameSuffix      = ".settings";
    opts.folderName          = "SoundSplice";
    opts.osxLibrarySubFolder = "Application Support";
    return opts;
}

} // namespace soundsplice::app
