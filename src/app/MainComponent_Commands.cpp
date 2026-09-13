#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Commands: the menu bar and keyboard shortcuts, and what each one does.

namespace soundsplice
{
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "View" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (topLevelMenuIndex == 0) // File
    {
        addItem(menu, 1, "New Project", keys::newProject);
        addItem(menu, 2, "Open Project...", keys::open);
        addItem(menu, 3, "Save Project", keys::save,
                hasUnsavedChanges() || projectFile_ == juce::File{});
        addItem(menu, 24, "Save Project As...", keys::saveAs);
        menu.addSeparator();
        menu.addItem(4, "Preview Audio File...");
        menu.addItem(7, "Import Audio to Track...   (or drag files in)");
        menu.addItem(8, "Import MIDI...");
        menu.addItem(9, "Export MIDI...");
        addItem(menu, 5, "Export Audio...", keys::exportAudio);
        menu.addSeparator();
        menu.addItem(13, "Set Project Root Folder...");
        menu.addSeparator();
        menu.addItem(6, "Audio Settings...");
        // Right next to Audio Settings, which is where anyone whose sound is
        // coming out of the wrong device goes looking.
        menu.addItem(32, "Follow System Output Device", true, followSystemOutput_);
    }
    else if (topLevelMenuIndex == 1) // Edit
    {
        addItem(menu, 10, withAction("Undo", history_.canUndo(), history_.undoLabel()),
                keys::undo, history_.canUndo());
        addItem(menu, 11, withAction("Redo", history_.canRedo(), history_.redoLabel()),
                keys::redo, history_.canRedo());
        menu.addSeparator();
        menu.addItem(12, "Clear Notes");
        menu.addSeparator();
        // Notes and clips get their own commands rather than one pair whose
        // meaning depends on which pane has focus.
        addItem(menu, 15, "Copy Notes", keys::copyNotes);
        addItem(menu, 16, "Paste Notes", keys::pasteNotes, ! noteClipboard_.empty());
        menu.addSeparator();
        // Audio edits get their own names too, for the same reason notes and
        // clips do: a command means one thing rather than depending on which
        // pane has focus. They act on the audio editor's selection, so they
        // are only enabled when there is one.
        {
            const bool hasAudio     = selectedAudioClip() != nullptr;
            const bool hasSelection = hasAudio && ! audioEditor_.selection().isEmpty();

            // The shortcuts are shown because they really do work here —
            // but only while the Audio pane is in front, which is why they
            // are attached to these items rather than promised globally.
            addItem(menu, 50, "Cut Audio",   keys::cutAudio,   hasSelection);
            addItem(menu, 51, "Copy Audio",  keys::copyNotes,  hasSelection);
            addItem(menu, 52, "Paste Audio", keys::pasteNotes,
                    hasAudio && ! audioClipboard_.empty());
            menu.addItem(53, "Delete Audio",   hasSelection, false);
            menu.addItem(54, "Trim to Selection", hasSelection, false);
            menu.addItem(55, "Split at Cursor",   hasSelection, false);
            menu.addItem(56, "Silence Audio",  hasSelection, false);
            menu.addItem(57, "Fade In",        hasSelection, false);
            menu.addItem(58, "Fade Out",       hasSelection, false);
            menu.addItem(59, "Reverse Audio",  hasSelection, false);

        }
        menu.addSeparator();
        addItem(menu, 17, "Copy Clip", keys::copyClip);
        addItem(menu, 18, "Paste Clip", keys::pasteClip, ! clipClipboard_.empty());
        addItem(menu, 19, "Duplicate Clip", keys::duplicate);
        menu.addSeparator();
        addItem(menu, 28, "Copy Track", keys::copyTrack);
        addItem(menu, 29, "Paste Track", keys::pasteTrack, trackClipboard_.has_value());
        addItem(menu, 30, "Duplicate Track", keys::duplicateTrack);
        addItem(menu, 25, "Delete Clip", keys::deleteClip);
        menu.addSeparator();
        menu.addItem(26, "Rename Track...");
        // The last track isn't deletable: a song with none has no pane that
        // can do anything, and no obvious way back.
        addItem(menu, 27, "Delete Track", keys::deleteTrack, trackCount() > 1);
        menu.addSeparator();
        addItem(menu, 20, "Quantize", keys::quantize);
        menu.addItem(21, "Swing - Light");
        menu.addItem(22, "Swing - Medium");
        menu.addItem(23, "Swing - Heavy");
    }
    else if (topLevelMenuIndex == 2) // View
    {
        // Every pane, ticked when it's open. This is the only way back to a
        // pane once its tab has been closed, so the list is built from what
        // the workspace *knows about* rather than what's currently on screen.
        for (const auto& name : workspace_.registeredPanels())
        {
            const int id = kFirstPanelMenuId + panelMenuIndex(name);
            menu.addItem(id, name, true, workspace_.isPanelOpen(name));
        }

        menu.addSeparator();

        // Ids sit in 33..99: 1..32 are the fixed commands and panel toggles
        // run from kFirstPanelMenuId upward, so this is the only free range.
        juce::PopupMenu layoutMenu;
        for (int i = 0; i < layouts::kNumWorkspaces; ++i)
        {
            const auto workspace = (layouts::Workspace) i;
            layoutMenu.addItem(kFirstLayoutMenuId + i, layouts::workspaceName(workspace),
                               true, workspace == activeWorkspace_);
        }
        menu.addSubMenu("Layout", layoutMenu);

        // Alt inverts this for a single drag, so it's a default rather than
        // a lock — worth saying, since a user who finds it off will otherwise
        // hunt for the menu every time they want one clip on the grid.
        menu.addItem(33, "Snap Clips to Grid   (hold Alt to invert)",
                     true, arrangementView_.snapsToGrid());

        // Splitting is a drag gesture (drop a tab on a pane's edge), so the
        // only layout command left is a way back to this one's default.
        menu.addItem(14, "Reset Layout");
    }

    return menu;
}

void MainComponent::menuItemSelected(int menuItemID, int)
{
    switch (menuItemID)
    {
        case 1:  newProject(); break;
        case 2:  openProject(); break;
        case 3:  saveProject(); break;
        case 24: saveProjectAs(); break;
        case 25: deleteSelectedClip(); break;
        case 26: renameSelectedTrack(); break;
        case 27: deleteSelectedTrack(); break;
        case 28: copyTrack(); break;
        case 29: pasteTrack(); break;
        case 30: duplicateTrackAt(selectedTrackIndex_); break;
        case 4:  chooseFile(); break; // preview only - see chooseFile
        case 5:  exportAudioDialog(); break;
        case 6:  showAudioSettings(); break;
        case 7:  importAudioToNewTrack(); break;
        case 50: cutAudioSelection(); break;
        case 51: copyAudioSelection(); break;
        case 52: pasteAudioAtSelection(); break;
        case 53: deleteAudioSelection(); break;
        case 54: trimToAudioSelection(); break;
        case 55: splitClipAtSelection(); break;
        case 56: silenceAudioSelection(); break;
        case 57: fadeInAudioSelection(); break;
        case 58: fadeOutAudioSelection(); break;
        case 59: reverseAudioSelection(); break;

        case 33:
        {
            const bool snap = ! arrangementView_.snapsToGrid();
            arrangementView_.setSnapToGrid(snap);
            settings_.setValue("snapClipsToGrid", snap ? "1" : "0");
            settings_.saveIfNeeded();
            showStatus(snap ? "Clips snap to the grid" : "Clips move freely");
            break;
        }

        case 32:
            followSystemOutput_ = ! followSystemOutput_;
            settings_.setValue("followSystemOutput", followSystemOutput_ ? "1" : "0");
            settings_.saveIfNeeded();
            if (followSystemOutput_)
                followSystemOutputIfEnabled();
            showStatus(followSystemOutput_ ? "Following the system output device"
                                           : "Staying on the selected output device");
            break;
        case 8:  importMidiFileDialog(); break;
        case 9:  exportMidiFileDialog(); break;
        case 10: history_.undo(); refreshFromModel(); break;
        case 11: history_.redo(); refreshFromModel(); break;
        case 12: pianoRoll_.clear(); break;
        case 13: setProjectRootFolderDialog(); break;
        case 14: buildDefaultDockLayout(); saveDockLayout(); break;
        case 15: copyNotes(); break;
        case 16: pasteNotes(); break;
        case 17: copyClip(); break;
        case 18: pasteClip(); break;
        case 19: duplicateClip(); break;
        case 20: quantizeNotes(0.0); break;
        case 21: quantizeNotes(0.25); break;
        case 22: quantizeNotes(0.5); break;
        case 23: quantizeNotes(0.66); break;

        // A View-menu panel entry. Opening an already-open pane would be a
        // no-op the user would read as broken, so togglePanel reveals a buried
        // one and closes one that's already in front.
        default:
            // Checked before the panel range, which is unbounded above.
            if (menuItemID >= kFirstLayoutMenuId
                && menuItemID < kFirstLayoutMenuId + layouts::kNumWorkspaces)
            {
                applyWorkspaceLayout((layouts::Workspace) (menuItemID - kFirstLayoutMenuId));
                break;
            }

            if (menuItemID >= kFirstPanelMenuId)
                togglePanel(menuItemID - kFirstPanelMenuId);
            break;
    }
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // Undo and redo refresh the whole UI from the model afterwards, so they
    // are handled apart from the commands that don't.
    if (key == keys::undo || key == keys::redo || key == keys::redoAlt)
    {
        const bool undoing = (key == keys::undo);
        const auto action  = undoing ? history_.undoLabel() : history_.redoLabel();
        const bool did     = undoing ? history_.canUndo() : history_.canRedo();

        if (undoing)
            history_.undo();
        else
            history_.redo();

        refreshFromModel();

        // The menu would have named the action; a keystroke has to say it
        // some other way, or undo is a silent jump the user has to diff.
        if (did)
            showStatus(withAction(undoing ? "Undo" : "Redo", true, action));

        return true;
    }

    // Transport shortcuts trigger the buttons rather than repeating what they
    // do: the button stays the single definition of the action, and it
    // visibly reacts — pressing space and seeing nothing move on screen reads
    // as a dropped keystroke.
    if (key == keys::playPause)  { playPauseButton.triggerClick();     return true; }
    if (key == keys::toStart)    { firstFrameButton.triggerClick();    return true; }
    if (key == keys::toEnd)      { lastFrameButton.triggerClick();     return true; }
    if (key == keys::backOneBar) { previousFrameButton.triggerClick(); return true; }
    if (key == keys::onOneBar)   { nextFrameButton.triggerClick();     return true; }
    if (key == keys::record)     { recordButton.triggerClick();        return true; }
    if (key == keys::loop)       { loopButton.triggerClick();          return true; }

    if (key == keys::newProject) { newProject();       return true; }
    if (key == keys::open)       { openProject();      return true; }
    if (key == keys::save)       { saveProject();      return true; }
    if (key == keys::saveAs)     { saveProjectAs();    return true; }
    if (key == keys::exportAudio) { exportAudioDialog(); return true; }

    // While the Audio pane is in front, the standard shortcuts act on the
    // waveform. Everywhere else they keep their existing note meaning — see
    // the note in Shortcuts.h on why this one command is context-sensitive
    // when none of the others are.
    if (workspace_.isPanelActive("Audio") && selectedAudioClip() != nullptr)
    {
        if (key == keys::cutAudio)   { cutAudioSelection();     return true; }
        if (key == keys::copyNotes)  { copyAudioSelection();    return true; }
        if (key == keys::pasteNotes) { pasteAudioAtSelection(); return true; }
    }

    if (key == keys::copyNotes)  { copyNotes();        return true; }
    if (key == keys::pasteNotes) { pasteNotes();       return true; }
    if (key == keys::copyClip)   { copyClip();         return true; }
    if (key == keys::pasteClip)  { pasteClip();        return true; }
    if (key == keys::duplicate)  { duplicateClip();    return true; }
    if (key == keys::quantize)   { quantizeNotes(0.0); return true; }
    if (key == keys::deleteClip) { deleteSelectedClip(); return true; }

    if (key == keys::copyTrack)      { copyTrack();  return true; }
    if (key == keys::pasteTrack)     { pasteTrack(); return true; }
    if (key == keys::duplicateTrack) { duplicateTrackAt(selectedTrackIndex_); return true; }

    // Unmodified, so a focused text field consumes it first and this can't
    // interrupt typing. Bound to "delete track" (see Shortcuts.h), but a
    // selected clip is the more specific target: every other DAW's bare
    // delete key acts on the clip first, and firing track deletion out from
    // under a clip the user was just editing is destructive and surprising.
    // keys::deleteClip (cmd+backspace) still works as an explicit,
    // clip-only alternative.
    if (key == keys::deleteTrack || key == keys::deleteTrackAlt)
    {
        if (hasSelectedClip())
            deleteSelectedClip();
        else
            deleteSelectedTrack();
        return true;
    }

    if (key == keys::zoomIn)  { setTimelineZoom(arrangementView_.zoom() * 1.25f); return true; }
    if (key == keys::zoomOut) { setTimelineZoom(arrangementView_.zoom() / 1.25f); return true; }

    return false;
}

} // namespace soundsplice
