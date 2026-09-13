#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Shortcuts.h"

namespace soundsplice::commands
{
/**
    Every command the app offers, defined once: its id, menu text, category,
    description and default shortcuts.

    MainComponent registers these with a juce::ApplicationCommandManager, and
    that is what the menus and the keyboard both go through — and what a
    command palette, macros or customisable shortcuts would go through later.
    Before, a command was spelled out three times (a menu id, a case in
    menuItemSelected, and a line in keyPressed), and adding one meant finding
    all three and keeping them agreeing.

    What a command does, and when it's available, depends on the document, so
    that half lives with MainComponent (MainComponent_Commands.cpp). This is
    the static half, kept apart so it can be checked on its own — see
    tests/gui/CommandTableTests.cpp.

    **Shared shortcuts.** A key may belong to more than one command when they
    apply in different contexts. The command manager gives a key to the first
    command, in this table's order, that is currently enabled, so the
    context-specific command comes first and is enabled only in its context,
    and the general one takes the key everywhere else:

      - Copy Audio and Paste Audio come before Copy Notes and Paste Notes:
        cmd+C and cmd+V act on the waveform while the Audio pane is in front
        (see the note in Shortcuts.h).
      - Delete Selected Clip comes before Delete Track: the bare delete key
        removes a selected clip rather than the track it's on.
*/
enum Id : int
{
    // Clear of juce::StandardApplicationCommandIDs, and of the ids the View
    // menu gives its panel and layout entries.
    kFirstId = 0x2000,

    // File
    newProject = kFirstId,
    openProject,
    saveProject,
    saveProjectAs,
    previewAudioFile,
    importAudio,
    importMidi,
    exportMidi,
    exportAudio,
    setProjectRoot,
    audioSettings,
    followSystemOutput,

    // Edit
    undo,
    redo,
    clearNotes,
    cutAudio,
    copyAudio,
    pasteAudio,
    copyNotes,
    pasteNotes,
    deleteAudio,
    trimToSelection,
    splitAtCursor,
    silenceAudio,
    fadeIn,
    fadeOut,
    reverseAudio,
    copyClip,
    pasteClip,
    duplicateClip,
    deleteClip,
    deleteSelectedClip,
    copyTrack,
    pasteTrack,
    duplicateTrack,
    renameTrack,
    deleteTrack,
    quantize,
    swingLight,
    swingMedium,
    swingHeavy,

    // Transport
    playPause,
    goToStart,
    goToEnd,
    backOneBar,
    forwardOneBar,
    record,
    loop,

    // View
    timeFormatBarsBeats,
    timeFormatMinutesSeconds,
    timeFormatSamples,
    timeFormatTimecode,
    timecode24,
    timecode25,
    timecode30,
    zoomIn,
    zoomOut,
    snapToGrid,
    resetLayout,
};

/** One command's fixed description. */
struct Definition
{
    Id                          id;
    const char*                 name;        // as the menu shows it
    const char*                 category;    // File, Edit, Transport or View
    const char*                 description; // one sentence saying what it does
    std::vector<juce::KeyPress> keys;        // default shortcuts, all from Shortcuts.h
};

/** Every command, in registration order, which is also the order a shared
    shortcut is offered to them in. */
inline const std::vector<Definition>& all()
{
    static const std::vector<Definition> table {
        { newProject,       "New Project",          "File", "Start an empty project, asking to save this one first.", { keys::newProject } },
        { openProject,      "Open Project...",      "File", "Open a saved project.", { keys::open } },
        { saveProject,      "Save Project",         "File", "Save the project over its file, or choose one the first time.", { keys::save } },
        { saveProjectAs,    "Save Project As...",   "File", "Save the project to a new file.", { keys::saveAs } },
        { previewAudioFile, "Preview Audio File...", "File", "Listen to an audio file without adding it to the project.", {} },
        { importAudio,      "Import Audio to Track...   (or drag files in)", "File", "Add an audio file to a new track.", {} },
        { importMidi,       "Import MIDI...",       "File", "Add the tracks of a MIDI file to the project.", {} },
        { exportMidi,       "Export MIDI...",       "File", "Write the project's notes to a MIDI file.", {} },
        { exportAudio,      "Export Audio...",      "File", "Render the mix, or each track as a stem, to audio files.", { keys::exportAudio } },
        { setProjectRoot,   "Set Project Root Folder...", "File", "Choose the folder the file browser starts in.", {} },
        { audioSettings,    "Audio Settings...",    "File", "Choose the audio and MIDI devices.", {} },
        { followSystemOutput, "Follow System Output Device", "File", "Switch output when the system's default device changes.", {} },

        { undo,             "Undo",                 "Edit", "Undo the last edit.", { keys::undo } },
        { redo,             "Redo",                 "Edit", "Redo the last undone edit.", { keys::redo, keys::redoAlt } },
        { clearNotes,       "Clear Notes",          "Edit", "Remove every note from the open clip.", {} },
        { cutAudio,         "Cut Audio",            "Edit", "Cut the audio editor's selection to the audio clipboard.", { keys::cutAudio } },
        { copyAudio,        "Copy Audio",           "Edit", "Copy the audio editor's selection to the audio clipboard.", { keys::copyNotes } },
        { pasteAudio,       "Paste Audio",          "Edit", "Paste the audio clipboard at the audio editor's cursor.", { keys::pasteNotes } },
        { copyNotes,        "Copy Notes",           "Edit", "Copy the selected notes.", { keys::copyNotes } },
        { pasteNotes,       "Paste Notes",          "Edit", "Paste copied notes into the open clip.", { keys::pasteNotes } },
        { deleteAudio,      "Delete Audio",         "Edit", "Remove the audio editor's selection.", {} },
        { trimToSelection,  "Trim to Selection",    "Edit", "Cut the clip down to the audio editor's selection.", {} },
        { splitAtCursor,    "Split at Cursor",      "Edit", "Split the clip in two where the selection starts.", {} },
        { silenceAudio,     "Silence Audio",        "Edit", "Replace the audio editor's selection with silence.", {} },
        { fadeIn,           "Fade In",              "Edit", "Fade the audio editor's selection in from silence.", {} },
        { fadeOut,          "Fade Out",             "Edit", "Fade the audio editor's selection out to silence.", {} },
        { reverseAudio,     "Reverse Audio",        "Edit", "Play the audio editor's selection backwards.", {} },
        { copyClip,         "Copy Clip",            "Edit", "Copy the selected clip.", { keys::copyClip } },
        { pasteClip,        "Paste Clip",           "Edit", "Paste the copied clip onto the selected track.", { keys::pasteClip } },
        { duplicateClip,    "Duplicate Clip",       "Edit", "Put a copy of the selected clip straight after it.", { keys::duplicate } },
        { deleteClip,       "Delete Clip",          "Edit", "Remove the selected clip.", { keys::deleteClip } },
        { deleteSelectedClip, "Delete Selected Clip", "Edit", "Remove the selected clip, if there is one.", { keys::deleteTrack, keys::deleteTrackAlt } },
        { copyTrack,        "Copy Track",           "Edit", "Copy the selected track and everything on it.", { keys::copyTrack } },
        { pasteTrack,       "Paste Track",          "Edit", "Paste the copied track after the selected one.", { keys::pasteTrack } },
        { duplicateTrack,   "Duplicate Track",      "Edit", "Put a copy of the selected track straight after it.", { keys::duplicateTrack } },
        { renameTrack,      "Rename Track...",      "Edit", "Rename the selected track.", {} },
        { deleteTrack,      "Delete Track",         "Edit", "Remove the selected track.", { keys::deleteTrack, keys::deleteTrackAlt } },
        { quantize,         "Quantize",             "Edit", "Move the selected notes onto the grid.", { keys::quantize } },
        { swingLight,       "Swing - Light",        "Edit", "Quantize with a light swing.", {} },
        { swingMedium,      "Swing - Medium",       "Edit", "Quantize with a medium swing.", {} },
        { swingHeavy,       "Swing - Heavy",        "Edit", "Quantize with a heavy swing.", {} },

        { playPause,        "Play / Pause",         "Transport", "Start or pause playback.", { keys::playPause } },
        { goToStart,        "Go to Start",          "Transport", "Move the playhead to the start.", { keys::toStart } },
        { goToEnd,          "Go to End",            "Transport", "Move the playhead to the end of the arrangement.", { keys::toEnd } },
        { backOneBar,       "Back One Bar",         "Transport", "Move the playhead back a bar.", { keys::backOneBar } },
        { forwardOneBar,    "Forward One Bar",      "Transport", "Move the playhead forward a bar.", { keys::onOneBar } },
        { record,           "Record",               "Transport", "Start or stop recording onto the selected track.", { keys::record } },
        { loop,             "Loop",                 "Transport", "Loop playback over what's arranged.", { keys::loop } },

        { timeFormatBarsBeats,      "Bars and Beats",      "View", "Count the ruler, grid and position in bars and beats.", {} },
        { timeFormatMinutesSeconds, "Minutes and Seconds", "View", "Count the ruler, grid and position in minutes and seconds.", {} },
        { timeFormatSamples,        "Samples",             "View", "Count the ruler, grid and position in samples at the device's rate.", {} },
        { timeFormatTimecode,       "Timecode",            "View", "Count the ruler, grid and position in hours, minutes, seconds and frames.", {} },
        { timecode24,               "24 fps",              "View", "Count timecode at 24 frames per second, as film does.", {} },
        { timecode25,               "25 fps",              "View", "Count timecode at 25 frames per second, as PAL video does.", {} },
        { timecode30,               "30 fps",              "View", "Count timecode at 30 frames per second (non-drop).", {} },
        { zoomIn,           "Zoom In",              "View", "Zoom the timeline in.", { keys::zoomIn } },
        { zoomOut,          "Zoom Out",             "View", "Zoom the timeline out.", { keys::zoomOut } },
        { snapToGrid,       "Snap Clips to Grid   (hold Alt to invert)", "View", "Snap dragged clips to whole beats.", {} },
        { resetLayout,      "Reset Layout",         "View", "Put the panes back where this layout starts them.", {} },
    };

    return table;
}

/** The definition for @p id, or nullptr if it isn't a command. */
inline const Definition* find(int id)
{
    for (const auto& definition : all())
        if (definition.id == id)
            return &definition;
    return nullptr;
}

} // namespace soundsplice::commands
