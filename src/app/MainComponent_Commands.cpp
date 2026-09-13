#include "MainComponentInternal.h"

#include "CommandTable.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Commands: what each one does and when it's available, and the menu bar built
// from them. The commands themselves are listed in CommandTable.h.

namespace soundsplice
{
juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget()
{
    return nullptr; // MainComponent handles every command itself
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& ids)
{
    for (const auto& definition : commands::all())
        ids.add(definition.id);
}

/** A command's name, shortcuts and, from the document, whether it can be used
    right now. The menus and the keyboard both ask this, so a shortcut can't
    act while its menu item is greyed out, or the reverse. */
void MainComponent::getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& info)
{
    const auto* definition = commands::find(commandID);
    if (definition == nullptr)
        return;

    info.setInfo(definition->name, definition->description, definition->category, 0);
    for (const auto& key : definition->keys)
        info.addDefaultKeypress(key.getKeyCode(), key.getModifiers());

    const bool hasAudio     = selectedAudioClip() != nullptr;
    const bool hasSelection = hasAudio && ! audioEditor_.selection().isEmpty();

    // Cut, copy and paste act on audio only while the Audio pane is in front:
    // they share cmd+X/C/V with the note commands, and this is what hands the
    // keys back to those everywhere else (see CommandTable.h).
    const bool audioInFront = hasAudio && workspace_.isPanelActive("Audio");

    switch (commandID)
    {
        case commands::saveProject:
            info.setActive(hasUnsavedChanges() || projectFile_ == juce::File{});
            break;

        case commands::followSystemOutput:
            info.setTicked(followSystemOutput_);
            break;

        // "Undo Delete track" rather than a bare "Undo": every edit already
        // records what it was.
        case commands::undo:
            info.shortName = withAction("Undo", history_.canUndo(), history_.undoLabel());
            info.setActive(history_.canUndo());
            break;

        case commands::redo:
            info.shortName = withAction("Redo", history_.canRedo(), history_.redoLabel());
            info.setActive(history_.canRedo());
            break;

        case commands::cutAudio:
        case commands::copyAudio:
            info.setActive(audioInFront && hasSelection);
            break;

        case commands::pasteAudio:
            info.setActive(audioInFront && ! audioClipboard_.empty());
            break;

        case commands::deleteAudio:
        case commands::trimToSelection:
        case commands::splitAtCursor:
        case commands::silenceAudio:
        case commands::fadeIn:
        case commands::fadeOut:
        case commands::reverseAudio:
            info.setActive(hasSelection);
            break;

        case commands::pasteNotes:
            info.setActive(! noteClipboard_.empty());
            break;

        case commands::pasteClip:
            info.setActive(! clipClipboard_.empty());
            break;

        case commands::pasteTrack:
            info.setActive(trackClipboard_.has_value());
            break;

        case commands::deleteSelectedClip:
            info.setActive(hasSelectedClip());
            break;

        // The last track isn't deletable: a song with none has no pane that
        // can do anything, and no obvious way back.
        case commands::deleteTrack:
            info.setActive(trackCount() > 1);
            break;

        case commands::zoomIn:
            info.setActive(arrangementView_.canZoomIn());
            break;

        case commands::zoomOut:
            info.setActive(arrangementView_.canZoomOut());
            break;

        case commands::snapToGrid:
            info.setTicked(arrangementView_.snapsToGrid());
            break;

        default:
            break;
    }
}

bool MainComponent::perform(const juce::ApplicationCommandTarget::InvocationInfo& invocation)
{
    const bool fromKeyPress =
        invocation.invocationMethod == juce::ApplicationCommandTarget::InvocationInfo::fromKeyPress;

    switch (invocation.commandID)
    {
        case commands::newProject:       newProject(); break;
        case commands::openProject:      openProject(); break;
        case commands::saveProject:      saveProject(); break;
        case commands::saveProjectAs:    saveProjectAs(); break;
        case commands::previewAudioFile: chooseFile(); break; // preview only - see chooseFile
        case commands::importAudio:      importAudioToNewTrack(); break;
        case commands::importMidi:       importMidiFileDialog(); break;
        case commands::exportMidi:       exportMidiFileDialog(); break;
        case commands::exportAudio:      exportAudioDialog(); break;
        case commands::setProjectRoot:   setProjectRootFolderDialog(); break;
        case commands::audioSettings:    showAudioSettings(); break;

        case commands::followSystemOutput:
            followSystemOutput_ = ! followSystemOutput_;
            settings_.setValue("followSystemOutput", followSystemOutput_ ? "1" : "0");
            settings_.saveIfNeeded();
            if (followSystemOutput_)
                followSystemOutputIfEnabled();
            showStatus(followSystemOutput_ ? "Following the system output device"
                                           : "Staying on the selected output device");
            break;

        case commands::undo:
        case commands::redo:
        {
            const bool undoing = invocation.commandID == commands::undo;
            const auto action  = undoing ? history_.undoLabel() : history_.redoLabel();
            const bool did     = undoing ? history_.canUndo() : history_.canRedo();

            if (undoing)
                history_.undo();
            else
                history_.redo();

            refreshFromModel();

            // The menu named the action; a keystroke has to say it some other
            // way, or undo is a silent jump the user has to work out.
            if (did && fromKeyPress)
                showStatus(withAction(undoing ? "Undo" : "Redo", true, action));
            break;
        }

        case commands::clearNotes:      pianoRoll_.clear(); break;
        case commands::cutAudio:        cutAudioSelection(); break;
        case commands::copyAudio:       copyAudioSelection(); break;
        case commands::pasteAudio:      pasteAudioAtSelection(); break;
        case commands::copyNotes:       copyNotes(); break;
        case commands::pasteNotes:      pasteNotes(); break;
        case commands::deleteAudio:     deleteAudioSelection(); break;
        case commands::trimToSelection: trimToAudioSelection(); break;
        case commands::splitAtCursor:   splitClipAtSelection(); break;
        case commands::silenceAudio:    silenceAudioSelection(); break;
        case commands::fadeIn:          fadeInAudioSelection(); break;
        case commands::fadeOut:         fadeOutAudioSelection(); break;
        case commands::reverseAudio:    reverseAudioSelection(); break;
        case commands::copyClip:        copyClip(); break;
        case commands::pasteClip:       pasteClip(); break;
        case commands::duplicateClip:   duplicateClip(); break;

        case commands::deleteClip:
        case commands::deleteSelectedClip:
            deleteSelectedClip();
            break;

        case commands::copyTrack:       copyTrack(); break;
        case commands::pasteTrack:      pasteTrack(); break;
        case commands::duplicateTrack:  duplicateTrackAt(selectedTrackIndex_); break;
        case commands::renameTrack:     renameSelectedTrack(); break;
        case commands::deleteTrack:     deleteSelectedTrack(); break;
        case commands::quantize:        quantizeNotes(0.0); break;
        case commands::swingLight:      quantizeNotes(0.25); break;
        case commands::swingMedium:     quantizeNotes(0.5); break;
        case commands::swingHeavy:      quantizeNotes(0.66); break;

        // Transport commands press the buttons rather than repeating what
        // they do: the button stays the one definition of the action, and it
        // visibly reacts, where pressing space and seeing nothing move on
        // screen reads as a dropped keystroke.
        case commands::playPause:     playPauseButton.triggerClick(); break;
        case commands::goToStart:     firstFrameButton.triggerClick(); break;
        case commands::goToEnd:       lastFrameButton.triggerClick(); break;
        case commands::backOneBar:    previousFrameButton.triggerClick(); break;
        case commands::forwardOneBar: nextFrameButton.triggerClick(); break;
        case commands::record:        recordButton.triggerClick(); break;
        case commands::loop:          loopButton.triggerClick(); break;

        case commands::zoomIn:  setTimelineZoom(arrangementView_.zoom() * 1.25f); break;
        case commands::zoomOut: setTimelineZoom(arrangementView_.zoom() / 1.25f); break;

        case commands::snapToGrid:
        {
            const bool snap = ! arrangementView_.snapsToGrid();
            arrangementView_.setSnapToGrid(snap);
            settings_.setValue("snapClipsToGrid", snap ? "1" : "0");
            settings_.saveIfNeeded();
            showStatus(snap ? "Clips snap to the grid" : "Clips move freely");
            break;
        }

        // Splitting is a drag gesture (drop a tab on a pane's edge), so the
        // only layout command is a way back to this layout's default.
        case commands::resetLayout:
            buildDefaultDockLayout();
            saveDockLayout();
            break;

        default:
            return false;
    }

    return true;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "View" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    // A command item takes its text, shortcut, and enabled and ticked state
    // from getCommandInfo, so none of that is repeated here.
    auto add = [this, &menu](commands::Id id) { menu.addCommandItem(&commandManager_, id); };

    if (topLevelMenuIndex == 0) // File
    {
        add(commands::newProject);
        add(commands::openProject);
        add(commands::saveProject);
        add(commands::saveProjectAs);
        menu.addSeparator();
        add(commands::previewAudioFile);
        add(commands::importAudio);
        add(commands::importMidi);
        add(commands::exportMidi);
        add(commands::exportAudio);
        menu.addSeparator();
        add(commands::setProjectRoot);
        menu.addSeparator();
        add(commands::audioSettings);
        // Right next to Audio Settings, which is where anyone whose sound is
        // coming out of the wrong device goes looking.
        add(commands::followSystemOutput);
    }
    else if (topLevelMenuIndex == 1) // Edit
    {
        add(commands::undo);
        add(commands::redo);
        menu.addSeparator();
        add(commands::clearNotes);
        menu.addSeparator();
        // Notes, audio, clips and tracks each get their own commands rather
        // than one set whose meaning depends on which pane has focus.
        add(commands::copyNotes);
        add(commands::pasteNotes);
        menu.addSeparator();
        add(commands::cutAudio);
        add(commands::copyAudio);
        add(commands::pasteAudio);
        add(commands::deleteAudio);
        add(commands::trimToSelection);
        add(commands::splitAtCursor);
        add(commands::silenceAudio);
        add(commands::fadeIn);
        add(commands::fadeOut);
        add(commands::reverseAudio);
        menu.addSeparator();
        add(commands::copyClip);
        add(commands::pasteClip);
        add(commands::duplicateClip);
        menu.addSeparator();
        add(commands::copyTrack);
        add(commands::pasteTrack);
        add(commands::duplicateTrack);
        add(commands::deleteClip);
        menu.addSeparator();
        add(commands::renameTrack);
        add(commands::deleteTrack);
        menu.addSeparator();
        add(commands::quantize);
        add(commands::swingLight);
        add(commands::swingMedium);
        add(commands::swingHeavy);
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

        juce::PopupMenu layoutMenu;
        for (int i = 0; i < layouts::kNumWorkspaces; ++i)
        {
            const auto workspace = (layouts::Workspace) i;
            layoutMenu.addItem(kFirstLayoutMenuId + i, layouts::workspaceName(workspace),
                               true, workspace == activeWorkspace_);
        }
        menu.addSubMenu("Layout", layoutMenu);

        add(commands::zoomIn);
        add(commands::zoomOut);
        add(commands::snapToGrid);
        add(commands::resetLayout);
    }

    return menu;
}

/** The View menu's own entries: its pane toggles and layouts, which are built
    from what the workspace holds and so aren't fixed commands. */
void MainComponent::menuItemSelected(int menuItemID, int)
{
    // A command item has already been performed by the command manager.
    if (menuItemID >= commands::kFirstId)
        return;

    // Checked before the panel range, which is unbounded above.
    if (menuItemID >= kFirstLayoutMenuId && menuItemID < kFirstLayoutMenuId + layouts::kNumWorkspaces)
    {
        applyWorkspaceLayout((layouts::Workspace) (menuItemID - kFirstLayoutMenuId));
        return;
    }

    // Opening an already-open pane would be a no-op the user would read as
    // broken, so togglePanel reveals a buried one and closes one that's
    // already in front.
    if (menuItemID >= kFirstPanelMenuId)
        togglePanel(menuItemID - kFirstPanelMenuId);
}

} // namespace soundsplice
