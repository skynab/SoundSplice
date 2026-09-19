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
    importRawData,
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
    studioFadeOut,
    repairAudio,
    clickRemoval,
    clipFix,
    humRemoval,
    spectralDelete,
    spectralGain,
    spectralRepair,
    spectralEq,
    spectralShelf,
    vocalReduction,
    adaptiveNoiseReduction,
    decrackle,
    spectralClipEdit,
    spectralClipEditsRemove,
    crossfadeClips,
    truncateSilence,
    autoDuck,
    repeatSelection,
    changeTempo,
    paulstretch,
    applyEffects,
    measureLoudness,
    plotSpectrum,
    amplitudeStatistics,
    findClipping,
    labelSounds,
    beatFinder,
    contrastBackground,
    contrast,
    generateTone,
    generateChirp,
    generateNoise,
    generateSilence,
    generateDtmf,
    generateRhythm,
    generatePluck,
    captureRoomTone,
    generateRoomTone,
    normalizeLoudness,
    matchEqReference,
    matchEq,
    copyClip,
    pasteClip,
    duplicateClip,
    splitAtPlayhead,
    joinClips,
    duplicateSelection,
    detachAtSilences,
    findZeroCrossings,
    deleteClip,
    deleteSelectedClip,
    copyTrack,
    pasteTrack,
    duplicateTrack,
    splitStereoToMono,
    swapChannels,
    makeStereoTrack,
    resampleTrack,
    mixAndRender,
    renameTrack,
    deleteTrack,
    quantize,
    swingLight,
    swingMedium,
    swingHeavy,

    // Transport
    playPause,
    playFaster,
    playSlower,
    playNormalSpeed,
    goToStart,
    goToEnd,
    backOneBar,
    forwardOneBar,
    record,
    loop,

    // Markers
    addMarker,
    addMarkerFromSelection,
    previousMarker,
    nextMarker,
    importMarkers,
    exportMarkers,
    deleteAllMarkers,

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
    zoomToSelection,
    fitProject,
    fitVertically,
    showClipEnvelopes,
    waveformDbScale,
    spectrogramView,
    spectrogramLog,
    spectrogramLinear,
    spectrogramMel,
    trackSpectrograms,
    spectrogramSplit,
    spectrogramSettings,
    nextOpenFile,
    previousOpenFile,
    closeOpenFile,
    closeAllOpenFiles,
    snapToGrid,
    snapToMarkers,
    snapToClipEdges,
    autoCrossfades,
    resetLayout,
};

/** One command's fixed description. */
struct Definition
{
    Id                          id;
    const char*                 name;        // as the menu shows it
    const char*                 category;    // File, Edit, Transport, Markers or View
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
        { importRawData,    "Import Raw Data...",   "File", "Add a headerless file of samples to a new track, saying how its bytes are stored.", {} },
        { exportMidi,       "Export MIDI...",       "File", "Write the project's notes to a MIDI file.", {} },
        { exportAudio,      "Export Audio...",      "File", "Render the mix, or each track as a stem, to audio files.", { keys::exportAudio } },
        { setProjectRoot,   "Set Project Root Folder...", "File", "Choose the folder the file browser starts in.", {} },
        { audioSettings,    "Audio Settings...",    "File", "Choose the audio and MIDI devices.", {} },
        { followSystemOutput, "Follow System Output Device", "File", "Switch output when the system's default device changes.", {} },

        { undo,             "Undo",                 "Edit", "Undo the last edit.", { keys::undo } },
        { redo,             "Redo",                 "Edit", "Redo the last undone edit.", { keys::redo, keys::redoAlt } },
        { clearNotes,       "Clear Notes",          "Edit", "Remove every note from the open clip.", {} },
        { cutAudio,         "Cut Audio",            "Edit", "Cut the arrangement's time selection, or the audio editor's selection, to the clipboard.", { keys::cutAudio } },
        { copyAudio,        "Copy Audio",           "Edit", "Copy the audio editor's selection to the audio clipboard.", { keys::copyNotes } },
        { pasteAudio,       "Paste Audio",          "Edit", "Paste at the arrangement's time selection, or at the audio editor's cursor.", { keys::pasteNotes } },
        { copyNotes,        "Copy Notes",           "Edit", "Copy the selected notes.", { keys::copyNotes } },
        { pasteNotes,       "Paste Notes",          "Edit", "Paste copied notes into the open clip.", { keys::pasteNotes } },
        { deleteAudio,      "Delete Audio",         "Edit", "Remove the arrangement's time selection, closing the gap, or the audio editor's selection.", {} },
        { trimToSelection,  "Trim to Selection",    "Edit", "Cut the clip down to the audio editor's selection.", {} },
        { splitAtCursor,    "Split at Cursor",      "Edit", "Split the clip in two where the selection starts.", {} },
        { silenceAudio,     "Silence Audio",        "Edit", "Silence the arrangement's time selection, or the audio editor's selection.", {} },
        { fadeIn,           "Fade In",              "Edit", "Fade the audio editor's selection in from silence.", {} },
        { fadeOut,          "Fade Out",             "Edit", "Fade the audio editor's selection out to silence.", {} },
        { reverseAudio,     "Reverse Audio",        "Edit", "Play the audio editor's selection backwards.", {} },
        { repairAudio,      "Repair",               "Edit", "Redraw a short selection (up to half a second) from what the audio either side of it predicts.", {} },
        { clickRemoval,     "Click Removal...",     "Edit", "Find clicks and pops in the audio editor's selection and fill each from the audio around it.", {} },
        { clipFix,          "Clip Fix...",          "Edit", "Rebuild clipped peaks in the audio editor's selection.", {} },
        { humRemoval,       "Hum Removal...",       "Edit", "Notch out 50 or 60 Hz mains hum and its harmonics from the audio editor's selection.", {} },
        { spectralDelete,   "Spectral Delete",      "Edit", "Remove the frequencies in a box dragged on the spectrogram, over its time.", {} },
        { spectralGain,     "Spectral Gain...",     "Edit", "Turn the frequencies in a box dragged on the spectrogram up or down, over its time.", {} },
        { spectralRepair,   "Spectral Repair",      "Edit", "Rebuild what's painted on the spectrogram (Ctrl-drag; add Alt for harmonics, Shift to lasso), or a box dragged on it, from what those frequencies do either side, to heal a cough or a clunk.", {} },
        { spectralEq,       "Spectral EQ...",       "Edit", "A bell of gain across the frequencies in a box dragged on the spectrogram, strongest at its middle.", {} },
        { spectralClipEdit, "Add Clip Spectral Edit...", "Edit", "Keep a gain on a box dragged on the spectrogram with the clip, applied as it plays and removable later; the file is untouched.", {} },
        { spectralClipEditsRemove, "Remove Clip Spectral Edits", "Edit", "Take the clip's kept spectral edits off where the selection is, or all of them.", {} },
        { adaptiveNoiseReduction, "Adaptive Noise Reduction...", "Edit", "Take steady noise (hiss, hum, air) down without a noise print, following it as it changes, over the selection or the whole clip.", {} },
        { decrackle,        "DeCrackle...",         "Edit", "Mend crackle, the many tiny clicks of worn vinyl or a bad cable, over the selection or the whole clip.", {} },
        { vocalReduction,   "Vocal Reduction and Isolation...", "Edit", "Take what's panned to the centre of a stereo clip (usually the vocal) out, or keep it alone, over the selection or the whole clip.", {} },
        { spectralShelf,    "Spectral Shelf...",    "Edit", "A shelf of gain ramping across a box dragged on the spectrogram and holding above or below it.", {} },
        { studioFadeOut,    "Studio Fade Out",      "Edit", "Fade the audio editor's selection out while a low-pass filter darkens it, as a mixed fade sounds.", {} },
        { autoDuck,         "Auto Duck...",         "Edit", "Dip the selected tracks wherever the lowest selected track is sounding, as an editable volume curve on each clip.", {} },
        { truncateSilence,  "Truncate Silence...",  "Edit", "Shorten every pause in the time selection that is silent on all its tracks, closing up the time.", {} },
        { repeatSelection,  "Repeat...",            "Edit", "Put copies of the time selection straight after it.", {} },
        { changeTempo,      "Change Tempo...",      "Edit", "Make the selected audio clip faster or slower without changing its pitch.", {} },
        { paulstretch,      "Paulstretch...",       "Edit", "Stretch the selected clip many times over into a smooth pad.", {} },
        { crossfadeClips,   "Crossfade Clips",      "Edit", "Overlap and crossfade neighbouring audio clips where they meet inside the time selection, using the audio beyond their edges.", {} },
        { generateTone,     "Tone...",              "Generate", "Generate a sine, square, sawtooth or triangle tone into the time selection, at the playhead, or on a new track.", {} },
        { generateChirp,    "Chirp...",             "Generate", "Generate a tone that sweeps from one frequency and level to another.", {} },
        { generateNoise,    "Noise...",             "Generate", "Generate white, pink or brown noise.", {} },
        { generateSilence,  "Silence...",           "Generate", "Generate silence, pushing what follows later.", {} },
        { generateDtmf,     "DTMF Tones...",        "Generate", "Generate the tones of a telephone keypad for a sequence of keys.", {} },
        { plotSpectrum,     "Plot Spectrum",        "Analyze", "Show the frequency content of the audio editor's selection, or the whole clip, in the Analyser pane.", {} },
        { amplitudeStatistics, "Amplitude Statistics", "Analyze", "Report the peak, RMS, DC offset and dynamic range of the audio editor's selection, or the whole clip.", {} },
        { findClipping,     "Find Clipping",        "Analyze", "Mark each run of clipped samples in the audio editor's selection, or the whole clip, with a marker range.", {} },
        { beatFinder,       "Beat Finder...",       "Analyze", "Mark each beat or hit in the audio editor's selection, or the whole clip, and estimate the tempo.", {} },
        { contrastBackground, "Set Contrast Background", "Analyze", "Measure the audio editor's selection as the background (music, noise) for Contrast.", {} },
        { contrast,         "Contrast",             "Analyze", "Compare the audio editor's selection (the foreground, speech) with the background: WCAG asks for 20 dB between them.", {} },
        { labelSounds,      "Label Sounds...",      "Analyze", "Mark each sound between silences in the audio editor's selection, or the whole clip, with a numbered marker range.", {} },
        { generatePluck,    "Pluck...",             "Generate", "Generate a plucked string at a pitch, ringing long or dying fast.", {} },
        { captureRoomTone,  "Capture Room Tone",    "Generate", "Measure the audio editor's selection, a passage of just the room, for Room Tone to recreate.", {} },
        { generateRoomTone, "Room Tone...",         "Generate", "Fill the time selection (or a length) with noise that sounds like the captured room, to patch gaps.", {} },
        { generateRhythm,   "Rhythm Track...",      "Generate", "Generate a click track: a tone on every beat, accented on the first of each bar.", {} },
        { measureLoudness,  "Measure Loudness",     "Analyze", "Measure the loudness (EBU R128) and true peak of the audio editor's selection, or the whole clip, into the Analyser pane.", {} },
        { matchEqReference, "Set as Match EQ Reference", "Edit", "Measure the tone of the audio editor's selection, or the whole clip, for Match EQ to aim at.", {} },
        { matchEq,          "Match EQ to Reference", "Edit", "Add a 31-band graphic EQ to the selected track that makes this clip's tone (or the selection's) match the reference's.", {} },
        { normalizeLoudness, "Normalize Loudness...", "Edit", "Set the selected clip's gain so its integrated loudness reaches a LUFS target.", {} },
        { applyEffects,     "Apply Effects...",     "Edit", "Render a chain of effects into the arrangement's time selection, or the audio editor's selection.", {} },
        { copyClip,         "Copy Clip",            "Edit", "Copy the selected clip.", { keys::copyClip } },
        { pasteClip,        "Paste Clip",           "Edit", "Paste the copied clip onto the selected track.", { keys::pasteClip } },
        { duplicateClip,    "Duplicate Clip",       "Edit", "Put a copy of the selected clip straight after it.", { keys::duplicate } },
        { splitAtPlayhead,  "Split at Playhead",    "Edit", "Split the audio clips under the playhead on the selected tracks.", { keys::splitAtPlayhead } },
        { joinClips,        "Join Clips",           "Edit", "Join clips that carry straight on from each other, within the time selection or across the selected tracks.", { keys::joinClips } },
        { duplicateSelection, "Duplicate Selection", "Edit", "Put a copy of the time selection straight after it.", {} },
        { detachAtSilences, "Detach at Silences...", "Edit", "Split audio clips where they fall silent, leaving the silence out.", {} },
        { findZeroCrossings, "Find Zero Crossings", "Edit", "Move the time selection's edges to the nearest places the audio crosses zero, so edits there don't click.", { keys::findZeroCrossings } },
        { deleteClip,       "Delete Clip",          "Edit", "Remove the selected clip.", { keys::deleteClip } },
        { deleteSelectedClip, "Delete Selected Clip", "Edit", "Remove the selected clip, if there is one.", { keys::deleteTrack, keys::deleteTrackAlt } },
        { copyTrack,        "Copy Track",           "Edit", "Copy the selected track and everything on it.", { keys::copyTrack } },
        { pasteTrack,       "Paste Track",          "Edit", "Paste the copied track after the selected one.", { keys::pasteTrack } },
        { duplicateTrack,   "Duplicate Track",      "Edit", "Put a copy of the selected track straight after it.", { keys::duplicateTrack } },
        { splitStereoToMono, "Split Stereo to Mono", "Edit", "Make the selected audio track two mono tracks, one playing its left channel and one its right.", {} },
        { swapChannels,     "Swap Channels",        "Edit", "Exchange the left and right channels of the selected audio track.", {} },
        { makeStereoTrack,  "Make Stereo Track",    "Edit", "Join the selected audio track and the one below it into one stereo track, the selected one on the left.", {} },
        { resampleTrack,    "Resample Track...",    "Edit", "Convert the selected audio track's audio to another sample rate, keeping its timing.", {} },
        { mixAndRender,     "Mix and Render to New Track", "Edit", "Render the time selection's tracks, or the selected track, into one audio file on a new track.", {} },
        { renameTrack,      "Rename Track...",      "Edit", "Rename the selected track.", {} },
        { deleteTrack,      "Delete Track",         "Edit", "Remove the selected track.", { keys::deleteTrack, keys::deleteTrackAlt } },
        { quantize,         "Quantize",             "Edit", "Move the selected notes onto the grid.", { keys::quantize } },
        { swingLight,       "Swing - Light",        "Edit", "Quantize with a light swing.", {} },
        { swingMedium,      "Swing - Medium",       "Edit", "Quantize with a medium swing.", {} },
        { swingHeavy,       "Swing - Heavy",        "Edit", "Quantize with a heavy swing.", {} },

        { playPause,        "Play / Pause",         "Transport", "Start or pause playback.", { keys::playPause } },
        { playFaster,       "Play Faster",          "Transport", "Play the song faster, higher in pitch, as a tape would.", {} },
        { playSlower,       "Play Slower",          "Transport", "Play the song slower, lower in pitch, as a tape would.", {} },
        { playNormalSpeed,  "Play at Normal Speed", "Transport", "Play the song at its own speed again.", {} },
        { goToStart,        "Go to Start",          "Transport", "Move the playhead to the start.", { keys::toStart } },
        { goToEnd,          "Go to End",            "Transport", "Move the playhead to the end of the arrangement.", { keys::toEnd } },
        { backOneBar,       "Back One Bar",         "Transport", "Move the playhead back a bar.", { keys::backOneBar } },
        { forwardOneBar,    "Forward One Bar",      "Transport", "Move the playhead forward a bar.", { keys::onOneBar } },
        { record,           "Record",               "Transport", "Start or stop recording onto the selected track.", { keys::record } },
        { loop,             "Loop",                 "Transport", "Loop playback over the time selection, or over what's arranged.", { keys::loop } },

        { addMarker,              "Add Marker",                  "Markers", "Put a marker at the playhead.", { keys::addMarker } },
        { addMarkerFromSelection, "Add Marker from Selection",   "Markers", "Mark the audio editor's selection as a range.", {} },
        { previousMarker,         "Previous Marker",             "Markers", "Move the playhead back to the previous marker.", { keys::previousMarker } },
        { nextMarker,             "Next Marker",                 "Markers", "Move the playhead on to the next marker.", { keys::nextMarker } },
        { importMarkers,          "Import Markers...",           "Markers", "Add markers from an Audacity label file.", {} },
        { exportMarkers,          "Export Markers...",           "Markers", "Write the markers to an Audacity label file.", {} },
        { deleteAllMarkers,       "Delete All Markers",          "Markers", "Remove every marker.", {} },

        { timeFormatBarsBeats,      "Bars and Beats",      "View", "Count the ruler, grid and position in bars and beats.", {} },
        { timeFormatMinutesSeconds, "Minutes and Seconds", "View", "Count the ruler, grid and position in minutes and seconds.", {} },
        { timeFormatSamples,        "Samples",             "View", "Count the ruler, grid and position in samples at the device's rate.", {} },
        { timeFormatTimecode,       "Timecode",            "View", "Count the ruler, grid and position in hours, minutes, seconds and frames.", {} },
        { timecode24,               "24 fps",              "View", "Count timecode at 24 frames per second, as film does.", {} },
        { timecode25,               "25 fps",              "View", "Count timecode at 25 frames per second, as PAL video does.", {} },
        { timecode30,               "30 fps",              "View", "Count timecode at 30 frames per second (non-drop).", {} },
        { zoomIn,           "Zoom In",              "View", "Zoom the timeline in.", { keys::zoomIn } },
        { zoomOut,          "Zoom Out",             "View", "Zoom the timeline out.", { keys::zoomOut } },
        { zoomToSelection,  "Zoom to Selection",    "View", "Zoom the timeline so the time selection fills the view.", { keys::zoomToSelection } },
        { fitProject,       "Fit Project",          "View", "Zoom the timeline so everything arranged fits the view.", { keys::fitProject } },
        { fitVertically,    "Fit Vertically",       "View", "Size the track lanes so every track fits the view's height.", { keys::fitVertically } },
        { spectrogramView,  "Spectrogram",          "View", "Show the audio editor's clip as a spectrogram, level by frequency over time, in place of the waveform.", {} },
        { spectrogramLog,    "Logarithmic",         "View", "Lay the spectrogram's frequencies out by octave, as they're heard.", {} },
        { spectrogramLinear, "Linear",              "View", "Lay the spectrogram's frequencies out evenly, which spaces harmonics evenly.", {} },
        { spectrogramSettings, "Spectrogram Settings...", "View", "Choose the spectrogram's window size and type, and how bright its colours are.", {} },
        { spectrogramSplit, "Waveform and Spectrogram", "View", "Show the audio editor's clip as a waveform above its spectrogram, to edit either.", {} },
        { trackSpectrograms, "Spectrograms in Tracks", "View", "Draw the audio clips in the tracks as spectrograms rather than waveforms.", {} },
        { spectrogramMel,    "Mel",                 "View", "Lay the spectrogram's frequencies out by perceived pitch.", {} },
        { waveformDbScale,  "Waveform dB Scale",    "View", "Draw the audio editor's waveform by level in decibels, so quiet passages can be seen.", {} },
        { nextOpenFile,     "Next Open File",       "View", "Show the next file in the Open Files list in the Audio editor.", { keys::nextOpenFile } },
        { previousOpenFile, "Previous Open File",   "View", "Show the previous file in the Open Files list in the Audio editor.", { keys::previousOpenFile } },
        { closeOpenFile,    "Close File",           "View", "Close the file showing in the Audio editor. Nothing is removed from the project.", { keys::closeOpenFile } },
        { closeAllOpenFiles, "Close All Files",     "View", "Empty the Open Files list. Nothing is removed from the project.", {} },
        { showClipEnvelopes, "Show Clip Volume Curves", "View", "Draw each audio clip's volume curve and edit it: click a clip to add a point, drag to move one, Alt-click to remove one.", {} },
        { snapToGrid,       "Snap Clips to Grid   (hold Alt to invert)", "View", "Snap dragged clips to whole beats.", {} },
        { snapToMarkers,    "Snap to Markers and Playhead", "View", "Pull dragged clip edges onto nearby markers and the playhead.", {} },
        { autoCrossfades,   "Automatic Crossfades", "View", "Crossfade audio clips wherever moving or resizing one makes it overlap its neighbour.", {} },
        { snapToClipEdges,  "Snap to Clip Edges", "View", "Pull dragged clip edges onto the edges of nearby clips.", {} },
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
