#pragma once

#include <juce_core/juce_core.h>

namespace looper
{
/**
    The audio formats this app will import, in one place.

    Every file chooser and every drop target has to agree on this. When they
    were separate lists the two drifted in a way nobody could see: a format
    offered by the "Import Audio" chooser could be silently ignored by a drag
    and drop, which reads as "drag and drop is broken" rather than as "that
    format isn't handled". One list, used by both.

    Matches what juce::AudioFormatManager::registerBasicFormats() actually
    supports, which is what AudioEngine decodes with.
*/
namespace audiofiles
{
    /** For juce::FileChooser, which wants a semicolon-separated wildcard list. */
    inline const char* wildcards() { return "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3"; }

    /** True if @p file looks like something importAudioFileAtBeat could open.
        Extension-based on purpose: a drag target has to answer during the
        drag, before the file is opened, and reading headers mid-drag would
        stall the UI on a slow or absent volume. */
    inline bool isImportableAudioFile(const juce::File& file)
    {
        return file.hasFileExtension("wav;aiff;aif;flac;ogg;mp3");
    }

    /** As above, for the path strings juce::FileDragAndDropTarget hands over
        (it reports paths, not File objects). */
    inline bool isImportableAudioFile(const juce::String& path)
    {
        return isImportableAudioFile(juce::File(path));
    }

    /** Every importable file among @p paths, in order. A multi-file drag is
        normal — selecting three takes in Finder and dragging them together
        should import three clips, not one — and non-audio files mixed into
        the selection are skipped rather than failing the whole drop. */
    inline juce::Array<juce::File> importableFilesIn(const juce::StringArray& paths)
    {
        juce::Array<juce::File> files;
        for (const auto& path : paths)
        {
            const juce::File file(path);
            if (file.existsAsFile() && isImportableAudioFile(file))
                files.add(file);
        }
        return files;
    }

    /** The drag description a file dragged from the Files pane carries.

        Prefixed and path-carrying so any target can identify the drag and
        recover the file from the description alone. The alternative — the
        target reaching back into the source component and asking it what was
        selected — only works for the one source type it knows about, which
        is exactly why dragging from the file *grid* did nothing while
        dragging from the directory tree worked. */
    inline juce::String dragDescriptionFor(const juce::File& file)
    {
        return "looper:file:" + file.getFullPathName();
    }

    /** The file a drag description names, or an invalid File if it isn't
        one of ours. */
    inline juce::File fileFromDragDescription(const juce::var& description)
    {
        const auto text = description.toString();
        if (! text.startsWith("looper:file:"))
            return {};

        const juce::File file(text.fromFirstOccurrenceOf("looper:file:", false, false));
        return file.existsAsFile() ? file : juce::File{};
    }

    /** True if @p paths contains anything worth accepting — what a drop
        target answers during the drag, so the cursor shows whether the drop
        will do something. */
    inline bool containsImportableAudio(const juce::StringArray& paths)
    {
        for (const auto& path : paths)
            if (isImportableAudioFile(path))
                return true;
        return false;
    }
}

} // namespace looper
