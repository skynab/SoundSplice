#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace looper
{
/** The kinds of file the file manager cares about — used to color-code the
    grid and tree, and (for Folder) to distinguish navigation targets. */
enum class FileKind { Audio, Midi, Project, Folder, Other };

inline FileKind classifyFile(const juce::File& file)
{
    if (file.isDirectory())
        return FileKind::Folder;
    if (file.hasFileExtension("wav;aiff;aif;flac;ogg;mp3;m4a;mp4"))
        return FileKind::Audio;
    if (file.hasFileExtension("mid;midi"))
        return FileKind::Midi;
    if (file.hasFileExtension("looper"))
        return FileKind::Project;
    return FileKind::Other;
}

inline juce::Colour colourForFileKind(FileKind kind)
{
    switch (kind)
    {
        case FileKind::Audio:   return juce::Colour(0xff5a9bd4);
        case FileKind::Midi:    return juce::Colour(0xff6abf69);
        case FileKind::Project: return juce::Colour(0xffd4a55a);
        case FileKind::Folder:  return juce::Colours::white.withAlpha(0.75f);
        case FileKind::Other:
        default:                return juce::Colours::white.withAlpha(0.35f);
    }
}

inline juce::String labelForFileKind(FileKind kind)
{
    switch (kind)
    {
        case FileKind::Audio:   return "Audio";
        case FileKind::Midi:    return "MIDI";
        case FileKind::Project: return "Project";
        case FileKind::Folder:  return "Folder";
        case FileKind::Other:
        default:                return "Other";
    }
}

} // namespace looper
