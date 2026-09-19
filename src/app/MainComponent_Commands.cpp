#include "MainComponentInternal.h"

#include "CommandTable.h"
#include "model/Markers.h"

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

        // A time selection in the arrangement counts too: these act on it
        // first (see perform).
        case commands::cutAudio:
        case commands::copyAudio:
            info.setActive((audioInFront && hasSelection) || ! timeSelection_.isEmpty());
            break;

        case commands::pasteAudio:
            info.setActive((audioInFront && ! audioClipboard_.empty())
                           || (timeSelection_.hasTracks() && ! rangeClipboard_.isEmpty()));
            break;

        case commands::deleteAudio:
        case commands::silenceAudio:
        case commands::applyEffects:
            info.setActive(hasSelection || ! timeSelection_.isEmpty());
            break;

        case commands::trimToSelection:
        case commands::splitAtCursor:
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

        case commands::splitAtPlayhead:
        case commands::joinClips:
        case commands::detachAtSilences:
            info.setActive(! arrangementEditTracks().empty());
            break;

        case commands::duplicateSelection:
        case commands::crossfadeClips:
        case commands::truncateSilence:
        case commands::repeatSelection:
        case commands::autoDuck:
            info.setActive(! timeSelection_.isEmpty());
            break;

        case commands::findZeroCrossings:
            info.setActive(timeSelection_.hasTracks());
            break;

        case commands::mixAndRender:
            info.setActive(renderJob_ == nullptr && ! arrangementEditTracks().empty());
            break;

        case commands::splitStereoToMono:
        case commands::swapChannels:
        {
            const auto& tracks = history_.current().tracks;
            info.setActive(selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) tracks.size()
                           && tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Audio);
            break;
        }

        case commands::resampleTrack:
        {
            const auto& tracks = history_.current().tracks;
            info.setActive(renderJob_ == nullptr && selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) tracks.size()
                           && tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Audio);
            break;
        }

        case commands::generateTone:
        case commands::generateChirp:
        case commands::generateNoise:
        case commands::generateSilence:
        case commands::generateDtmf:
        case commands::generatePluck:
        case commands::generateRoomTone:
        case commands::generateRhythm:
            info.setActive(renderJob_ == nullptr);
            break;

        case commands::contrast:
            info.setActive(selectedAudioClip() != nullptr && contrastBackgroundDb_.has_value());
            break;

        case commands::matchEq:
            info.setActive(renderJob_ == nullptr && selectedAudioClip() != nullptr && matchEqReference_.has_value());
            break;

        case commands::captureRoomTone:
        case commands::vocalReduction:
        case commands::adaptiveNoiseReduction:
        case commands::decrackle:
        case commands::plotSpectrum:
        case commands::amplitudeStatistics:
        case commands::findClipping:
        case commands::labelSounds:
        case commands::beatFinder:
        case commands::contrastBackground:
        case commands::measureLoudness:
        case commands::normalizeLoudness:
        case commands::matchEqReference:
        case commands::changeTempo:
        case commands::paulstretch:
            info.setActive(renderJob_ == nullptr && selectedAudioClip() != nullptr);
            break;

        case commands::makeStereoTrack:
        {
            const auto& tracks = history_.current().tracks;
            const int   index  = selectedTrackIndex_;
            info.setActive(renderJob_ == nullptr && index >= 0 && index + 1 < (int) tracks.size()
                           && tracks[(size_t) index].type == model::TrackType::Audio
                           && tracks[(size_t) index + 1].type == model::TrackType::Audio);
            break;
        }

        // The last track isn't deletable: a song with none has no pane that
        // can do anything, and no obvious way back.
        case commands::deleteTrack:
            info.setActive(trackCount() > 1);
            break;

        case commands::addMarkerFromSelection:
            info.setActive(hasSelection);
            break;

        case commands::previousMarker:
            info.setActive(model::previousMarkerStart(history_.current(), playheadBeat()).has_value());
            break;

        case commands::nextMarker:
            info.setActive(model::nextMarkerStart(history_.current(), playheadBeat()).has_value());
            break;

        case commands::exportMarkers:
        case commands::deleteAllMarkers:
            info.setActive(! history_.current().markers.empty());
            break;

        case commands::timeFormatBarsBeats:
            info.setTicked(timeDisplay_.format == app::TimeFormat::BarsBeats);
            break;

        case commands::timeFormatMinutesSeconds:
            info.setTicked(timeDisplay_.format == app::TimeFormat::MinutesSeconds);
            break;

        case commands::timeFormatSamples:
            info.setTicked(timeDisplay_.format == app::TimeFormat::Samples);
            break;

        case commands::timeFormatTimecode:
            info.setTicked(timeDisplay_.format == app::TimeFormat::Timecode);
            break;

        case commands::timecode24:
            info.setTicked(timeDisplay_.fps == 24);
            break;

        case commands::timecode25:
            info.setTicked(timeDisplay_.fps == 25);
            break;

        case commands::timecode30:
            info.setTicked(timeDisplay_.fps == 30);
            break;

        case commands::zoomIn:
            info.setActive(arrangementView_.canZoomIn());
            break;

        case commands::zoomOut:
            info.setActive(arrangementView_.canZoomOut());
            break;

        case commands::zoomToSelection:
            info.setActive(! timeSelection_.isEmpty());
            break;

        case commands::snapToGrid:
            info.setTicked(arrangementView_.snapsToGrid());
            break;

        case commands::snapToMarkers:
            info.setTicked(arrangementView_.snapsToMarkers());
            break;

        case commands::snapToClipEdges:
            info.setTicked(arrangementView_.snapsToClipEdges());
            break;

        case commands::showClipEnvelopes:
            info.setTicked(arrangementView_.showsEnvelopes());
            break;

        case commands::trackSpectrograms:
            info.setTicked(arrangementView_.showsSpectrograms());
            break;

        case commands::waveformDbScale:
            info.setTicked(audioEditor_.showsDbScale());
            break;

        case commands::spectrogramView:
            info.setTicked(audioEditor_.showsSpectrogram() && ! audioEditor_.showsSplitView());
            break;

        case commands::spectrogramSplit:
            info.setTicked(audioEditor_.showsSpectrogram() && audioEditor_.showsSplitView());
            break;

        case commands::spectrogramLog:
            info.setTicked(audioEditor_.spectrogramScale() == spectrogramimage::Scale::Logarithmic);
            break;
        case commands::spectrogramLinear:
            info.setTicked(audioEditor_.spectrogramScale() == spectrogramimage::Scale::Linear);
            break;
        case commands::spectrogramMel:
            info.setTicked(audioEditor_.spectrogramScale() == spectrogramimage::Scale::Mel);
            break;

        case commands::spectralDelete:
        case commands::spectralGain:
        case commands::spectralRepair:
            info.setActive(audioEditor_.frequencyBand().has_value() || audioEditor_.spectralBrush().has_value());
            break;

        case commands::spectralEq:
        case commands::spectralShelf:
        case commands::spectralClipEdit:
            info.setActive(audioEditor_.frequencyBand().has_value());
            break;

        case commands::spectralClipEditsRemove:
            info.setActive(selectedAudioClip() != nullptr && ! selectedAudioClip()->spectralEdits.empty());
            break;

        case commands::nextOpenFile:
        case commands::previousOpenFile:
            info.setActive(! openFiles_.isEmpty());
            break;

        case commands::closeOpenFile:
        {
            const auto* clip = selectedAudioClip();
            info.setActive(clip != nullptr && openFiles_.contains(clip->id));
            break;
        }

        case commands::closeAllOpenFiles:
            info.setActive(! openFiles_.isEmpty());
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
        case commands::importRawData:    importRawDataDialog(); break;
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
        // A time selection in the arrangement, when there is one, before the
        // audio editor's own selection.
        case commands::cutAudio:        if (! editTimeSelection("Cut", true, true, true)) cutAudioSelection(); break;
        case commands::copyAudio:       if (! editTimeSelection("Copy", true, false, false)) copyAudioSelection(); break;
        case commands::pasteAudio:      if (! pasteAtTimeSelection()) pasteAudioAtSelection(); break;
        case commands::copyNotes:       copyNotes(); break;
        case commands::pasteNotes:      pasteNotes(); break;
        case commands::deleteAudio:     if (! editTimeSelection("Delete", false, true, true)) deleteAudioSelection(); break;
        case commands::trimToSelection: trimToAudioSelection(); break;
        case commands::splitAtCursor:   splitClipAtSelection(); break;
        case commands::silenceAudio:    if (! editTimeSelection("Silence", false, true, false)) silenceAudioSelection(); break;
        case commands::fadeIn:          fadeInAudioSelection(); break;
        case commands::fadeOut:         fadeOutAudioSelection(); break;
        case commands::reverseAudio:    reverseAudioSelection(); break;
        case commands::studioFadeOut:   studioFadeOutAudioSelection(); break;
        case commands::repairAudio:     repairAudioSelection(); break;
        case commands::clickRemoval:    showClickRemovalDialog(); break;
        case commands::vocalReduction:  showVocalReductionDialog(); break;
        case commands::adaptiveNoiseReduction: showAdaptiveNoiseReductionDialog(); break;
        case commands::decrackle:       showDecrackleDialog(); break;
        case commands::clipFix:         showClipFixDialog(); break;
        case commands::humRemoval:      showHumRemovalDialog(); break;
        case commands::spectralDelete:  scaleSpectralSelection("Spectral delete", 0.0f); break;
        case commands::spectralGain:    showSpectralGainDialog(); break;
        case commands::spectralRepair:  repairSpectralSelection(); break;
        case commands::spectralEq:      showSpectralEqDialog(); break;
        case commands::spectralShelf:   showSpectralShelfDialog(); break;
        case commands::spectralClipEdit:        showSpectralClipEditDialog(); break;
        case commands::spectralClipEditsRemove: removeSpectralClipEdits(); break;
        case commands::crossfadeClips:  crossfadeClipsInSelection(); break;
        case commands::truncateSilence: showTruncateSilenceDialog(); break;
        case commands::autoDuck:        showAutoDuckDialog(); break;
        case commands::repeatSelection: showRepeatDialog(); break;
        case commands::changeTempo:     showChangeTempoDialog(); break;
        case commands::paulstretch:     showPaulstretchDialog(); break;
        case commands::applyEffects:    showApplyEffectsDialog(); break;
        case commands::copyClip:        copyClip(); break;
        case commands::pasteClip:       pasteClip(); break;
        case commands::duplicateClip:   duplicateClip(); break;
        case commands::splitAtPlayhead: splitClipsAtPlayhead(); break;
        case commands::joinClips:       joinArrangementClips(); break;
        case commands::duplicateSelection: duplicateTimeSelection(); break;
        case commands::detachAtSilences: showDetachAtSilencesDialog(); break;
        case commands::findZeroCrossings: snapTimeSelectionToZeroCrossings(); break;

        case commands::deleteClip:
        case commands::deleteSelectedClip:
            deleteSelectedClip();
            break;

        case commands::copyTrack:       copyTrack(); break;
        case commands::pasteTrack:      pasteTrack(); break;
        case commands::duplicateTrack:  duplicateTrackAt(selectedTrackIndex_); break;
        case commands::splitStereoToMono: splitSelectedTrackToMono(); break;
        case commands::swapChannels:    swapSelectedTrackChannels(); break;
        case commands::makeStereoTrack: makeStereoTrack(); break;
        case commands::measureLoudness: measureLoudnessOfSelection(); break;
        case commands::plotSpectrum:    analyseSelection(); break;
        case commands::amplitudeStatistics: showAmplitudeStatistics(); break;
        case commands::findClipping:    findClipping(); break;
        case commands::labelSounds:     showLabelSoundsDialog(); break;
        case commands::beatFinder:      showBeatFinderDialog(); break;
        case commands::contrastBackground: setContrastBackground(); break;
        case commands::contrast:        measureContrast(); break;
        case commands::generateTone:    showGenerateDialog(engine::GeneratorKind::Tone); break;
        case commands::generateChirp:   showGenerateDialog(engine::GeneratorKind::Chirp); break;
        case commands::generateNoise:   showGenerateDialog(engine::GeneratorKind::Noise); break;
        case commands::generateSilence: showGenerateDialog(engine::GeneratorKind::Silence); break;
        case commands::generateDtmf:    showGenerateDialog(engine::GeneratorKind::Dtmf); break;
        case commands::generateRhythm:  showGenerateDialog(engine::GeneratorKind::Rhythm); break;
        case commands::generatePluck:   showGenerateDialog(engine::GeneratorKind::Pluck); break;
        case commands::generateRoomTone: showGenerateDialog(engine::GeneratorKind::RoomTone); break;
        case commands::captureRoomTone: captureRoomTone(); break;
        case commands::normalizeLoudness: showNormalizeLoudnessDialog(); break;
        case commands::matchEqReference:  setMatchEqReference(); break;
        case commands::matchEq:           matchEqToReference(); break;
        case commands::resampleTrack:   showResampleTrackDialog(); break;
        case commands::mixAndRender:    mixAndRenderToNewTrack(); break;
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
        case commands::playFaster:      setPlaySpeed(engine::Varispeed::steppedSpeed(engine_.playSpeed(), 1)); break;
        case commands::playSlower:      setPlaySpeed(engine::Varispeed::steppedSpeed(engine_.playSpeed(), -1)); break;
        case commands::playNormalSpeed: setPlaySpeed(1.0); break;
        case commands::goToStart:     firstFrameButton.triggerClick(); break;
        case commands::goToEnd:       lastFrameButton.triggerClick(); break;
        case commands::backOneBar:    previousFrameButton.triggerClick(); break;
        case commands::forwardOneBar: nextFrameButton.triggerClick(); break;
        case commands::record:        recordButton.triggerClick(); break;
        case commands::loop:          loopButton.triggerClick(); break;

        case commands::addMarker:              addMarkerAtPlayhead(); break;
        case commands::addMarkerFromSelection: addMarkerFromAudioSelection(); break;
        case commands::previousMarker:         jumpToMarker(false); break;
        case commands::nextMarker:             jumpToMarker(true); break;
        case commands::importMarkers:          importMarkersDialog(); break;
        case commands::exportMarkers:          exportMarkersDialog(); break;
        case commands::deleteAllMarkers:       deleteAllMarkers(); break;

        // A preference, not an edit: it changes what time is counted in, not
        // the song, so it's saved with the app settings and isn't undoable.
        case commands::timeFormatBarsBeats:
        case commands::timeFormatMinutesSeconds:
        case commands::timeFormatSamples:
        case commands::timeFormatTimecode:
        {
            const auto id = invocation.commandID;
            timeDisplay_.format = id == commands::timeFormatMinutesSeconds ? app::TimeFormat::MinutesSeconds
                                : id == commands::timeFormatSamples        ? app::TimeFormat::Samples
                                : id == commands::timeFormatTimecode       ? app::TimeFormat::Timecode
                                                                           : app::TimeFormat::BarsBeats;
            arrangementView_.setTimeDisplay(timeDisplay_);
            settings_.setValue("timeFormat", (int) timeDisplay_.format);
            settings_.saveIfNeeded();
            break;
        }

        case commands::timecode24:
        case commands::timecode25:
        case commands::timecode30:
            timeDisplay_.fps = invocation.commandID == commands::timecode24 ? 24
                             : invocation.commandID == commands::timecode25 ? 25
                                                                             : 30;
            arrangementView_.setTimeDisplay(timeDisplay_);
            settings_.setValue("timecodeFps", timeDisplay_.fps);
            settings_.saveIfNeeded();
            break;

        case commands::zoomIn:  setTimelineZoom(arrangementView_.zoom() * 1.25f); break;
        case commands::zoomOut: setTimelineZoom(arrangementView_.zoom() / 1.25f); break;
        case commands::zoomToSelection: zoomToTimeSelection(); break;
        case commands::fitProject:      fitProjectInView(); break;
        case commands::fitVertically:   fitTracksVertically(); break;

        case commands::snapToGrid:
        {
            const bool snap = ! arrangementView_.snapsToGrid();
            arrangementView_.setSnapToGrid(snap);
            settings_.setValue("snapClipsToGrid", snap ? "1" : "0");
            settings_.saveIfNeeded();
            showStatus(snap ? "Clips snap to the grid" : "Clips move freely");
            break;
        }

        case commands::snapToMarkers:
        {
            const bool snap = ! arrangementView_.snapsToMarkers();
            arrangementView_.setSnapToMarkers(snap);
            settings_.setValue("snapToMarkers", snap ? "1" : "0");
            settings_.saveIfNeeded();
            showStatus(snap ? "Clips snap to markers and the playhead" : "Clips no longer snap to markers");
            break;
        }

        case commands::snapToClipEdges:
        {
            const bool snap = ! arrangementView_.snapsToClipEdges();
            arrangementView_.setSnapToClipEdges(snap);
            settings_.setValue("snapToClipEdges", snap ? "1" : "0");
            settings_.saveIfNeeded();
            showStatus(snap ? "Clips snap to each other's edges" : "Clips no longer snap to each other");
            break;
        }

        // A view preference, like the snap settings: saved with the app, not
        // the project, and not undoable.
        case commands::trackSpectrograms:
        {
            const bool show = ! arrangementView_.showsSpectrograms();
            arrangementView_.setShowSpectrograms(show);
            settings_.setValue("trackSpectrograms", show ? "1" : "0");
            settings_.saveIfNeeded();
            break;
        }

        case commands::showClipEnvelopes:
        {
            const bool show = ! arrangementView_.showsEnvelopes();
            arrangementView_.setShowEnvelopes(show);
            settings_.setValue("showClipEnvelopes", show ? "1" : "0");
            settings_.saveIfNeeded();
            showStatus(show ? "Volume curves shown - click a clip to add a point, drag to move, Alt-click to remove"
                            : "Volume curves hidden");
            break;
        }

        case commands::nextOpenFile:      stepOpenFile(1); break;
        case commands::previousOpenFile:  stepOpenFile(-1); break;
        case commands::closeOpenFile:
            if (const auto* clip = selectedAudioClip())
                closeOpenFile(clip->id);
            break;
        case commands::closeAllOpenFiles: closeAllOpenFiles(); break;

        case commands::spectrogramLog:
        case commands::spectrogramLinear:
        case commands::spectrogramMel:
        {
            const auto scale = invocation.commandID == commands::spectrogramLinear ? spectrogramimage::Scale::Linear
                             : invocation.commandID == commands::spectrogramMel    ? spectrogramimage::Scale::Mel
                                                                              : spectrogramimage::Scale::Logarithmic;
            audioEditor_.setSpectrogramScale(scale);
            arrangementView_.setSpectrogramStyle(scale, audioEditor_.spectrogramDisplay());
            settings_.setValue("spectrogramScale", (int) scale);
            settings_.saveIfNeeded();
            break;
        }

        case commands::spectrogramSettings:
            showSpectrogramSettingsDialog();
            break;

        case commands::spectrogramView:
        case commands::spectrogramSplit:
        {
            // Each is a view of its own: choosing the one showing goes back
            // to the waveform alone, choosing the other switches to it.
            const bool split   = invocation.commandID == commands::spectrogramSplit;
            const bool showing = audioEditor_.showsSpectrogram() && audioEditor_.showsSplitView() == split;
            const bool on      = ! showing;
            audioEditor_.setSpectrogramView(on);
            if (on)
                audioEditor_.setSplitView(split);
            settings_.setValue("spectrogramView", on ? "1" : "0");
            settings_.setValue("spectrogramSplit", audioEditor_.showsSplitView() ? "1" : "0");
            settings_.saveIfNeeded();
            refreshAudioEditorForSelected(); // builds it for the clip on show
            break;
        }

        case commands::waveformDbScale:
        {
            const bool db = ! audioEditor_.showsDbScale();
            audioEditor_.setDbScale(db);
            settings_.setValue("waveformDbScale", db ? "1" : "0");
            settings_.saveIfNeeded();
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
    return { "File", "Edit", "View", "Markers", "Transport", "Generate", "Analyze" };
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
        add(commands::importRawData);
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
        add(commands::studioFadeOut);
        add(commands::changeTempo);
        add(commands::paulstretch);
        add(commands::reverseAudio);
        add(commands::applyEffects);
        menu.addSeparator();
        add(commands::repairAudio);
        add(commands::clickRemoval);
        add(commands::clipFix);
        add(commands::humRemoval);
        add(commands::decrackle);
        add(commands::adaptiveNoiseReduction);
        add(commands::vocalReduction);
        {
            // All of these act on a box dragged on the spectrogram.
            juce::PopupMenu spectral;
            for (auto id : { commands::spectralRepair, commands::spectralDelete, commands::spectralGain,
                             commands::spectralEq, commands::spectralShelf, commands::spectralClipEdit,
                             commands::spectralClipEditsRemove })
                spectral.addCommandItem(&commandManager_, id);
            menu.addSubMenu("Spectral", spectral);
        }
        menu.addSeparator();
        add(commands::normalizeLoudness);
        add(commands::matchEqReference);
        add(commands::matchEq);
        menu.addSeparator();
        add(commands::copyClip);
        add(commands::pasteClip);
        add(commands::duplicateClip);
        menu.addSeparator();
        add(commands::splitAtPlayhead);
        add(commands::joinClips);
        add(commands::crossfadeClips);
        add(commands::duplicateSelection);
        add(commands::repeatSelection);
        add(commands::detachAtSilences);
        add(commands::truncateSilence);
        add(commands::autoDuck);
        add(commands::findZeroCrossings);
        menu.addSeparator();
        add(commands::copyTrack);
        add(commands::pasteTrack);
        add(commands::duplicateTrack);
        add(commands::splitStereoToMono);
        add(commands::swapChannels);
        add(commands::makeStereoTrack);
        add(commands::resampleTrack);
        add(commands::mixAndRender);
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

        juce::PopupMenu timeMenu;
        for (auto id : { commands::timeFormatBarsBeats, commands::timeFormatMinutesSeconds,
                         commands::timeFormatSamples, commands::timeFormatTimecode })
            timeMenu.addCommandItem(&commandManager_, id);
        timeMenu.addSeparator();
        for (auto id : { commands::timecode24, commands::timecode25, commands::timecode30 })
            timeMenu.addCommandItem(&commandManager_, id);
        menu.addSubMenu("Time Format", timeMenu);
        menu.addSeparator();
        add(commands::zoomIn);
        add(commands::zoomOut);
        add(commands::zoomToSelection);
        add(commands::fitProject);
        add(commands::fitVertically);
        add(commands::showClipEnvelopes);
        add(commands::waveformDbScale);
        add(commands::spectrogramView);
        add(commands::spectrogramSplit);
        {
            juce::PopupMenu scaleMenu;
            for (auto id : { commands::spectrogramLog, commands::spectrogramLinear, commands::spectrogramMel })
                scaleMenu.addCommandItem(&commandManager_, id);
            menu.addSubMenu("Spectrogram Scale", scaleMenu);
        }
        add(commands::spectrogramSettings);
        add(commands::trackSpectrograms);
        menu.addSeparator();
        add(commands::nextOpenFile);
        add(commands::previousOpenFile);
        add(commands::closeOpenFile);
        add(commands::closeAllOpenFiles);
        menu.addSeparator();
        add(commands::snapToGrid);
        add(commands::snapToMarkers);
        add(commands::snapToClipEdges);
        add(commands::resetLayout);
    }
    else if (topLevelMenuIndex == 3) // Markers
    {
        add(commands::addMarker);
        add(commands::addMarkerFromSelection);
        menu.addSeparator();
        add(commands::previousMarker);
        add(commands::nextMarker);
        menu.addSeparator();
        add(commands::importMarkers);
        add(commands::exportMarkers);
        menu.addSeparator();
        add(commands::deleteAllMarkers);
    }
    else if (topLevelMenuIndex == 4) // Transport
    {
        add(commands::playPause);
        add(commands::loop);
        add(commands::record);
        menu.addSeparator();
        add(commands::goToStart);
        add(commands::goToEnd);
        add(commands::backOneBar);
        add(commands::forwardOneBar);
        menu.addSeparator();
        add(commands::playFaster);
        add(commands::playSlower);
        add(commands::playNormalSpeed);
    }
    else if (topLevelMenuIndex == 5) // Generate
    {
        add(commands::generateTone);
        add(commands::generateChirp);
        add(commands::generateNoise);
        add(commands::generateDtmf);
        add(commands::generateRhythm);
        add(commands::generatePluck);
        menu.addSeparator();
        add(commands::captureRoomTone);
        add(commands::generateRoomTone);
        menu.addSeparator();
        add(commands::generateSilence);
    }
    else if (topLevelMenuIndex == 6) // Analyze
    {
        add(commands::plotSpectrum);
        add(commands::measureLoudness);
        add(commands::amplitudeStatistics);
        menu.addSeparator();
        add(commands::findClipping);
        add(commands::labelSounds);
        add(commands::beatFinder);
        menu.addSeparator();
        add(commands::contrastBackground);
        add(commands::contrast);
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
