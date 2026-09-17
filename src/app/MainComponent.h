#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "engine/AudioEngine.h"
#include "engine/MasteringPreset.h"
#include "engine/MidiCapture.h"
#include "app/RecordSourceChoice.h"
#include "app/MicrophonePermission.h"
#include "engine/AudioEdits.h"
#include "engine/SampleSequence.h"
#include "engine/AudioExport.h"
#include "engine/TimeStretch.h"
#include "engine/NoiseReduction.h"
#include "engine/RawPcm.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/Song.h"
#include "model/TimeSelection.h"

#include "OfflineRenderJob.h"

#include "ArrangementView.h"
#include "DockWorkspace.h"
#include "EffectChainPanel.h"
#include "EqCurveView.h"
#include "AnalyserPane.h"
#include "AutomationPane.h"
#include "ApplyEffectsDialog.h"
#include "AudioEditorPane.h"
#include "MasteringPane.h"
#include "WorkspaceLayouts.h"
#include "FileBrowserPanel.h"
#include "LevelMeter.h"
#include "MixerStrip.h"
#include "PianoRoll.h"
#include "PluginEditorWindow.h"
#include "ApplyEffectsDialog.h"
#include "Autosave.h"
#include "TimeFormat.h"
#include "ClipWindow.h"
#include "ProjectMedia.h"
#include "DragCommit.h"
#include "TrackSelection.h"
#include "SessionView.h"
#include "TrackColours.h"
#include "StatusBanner.h"

namespace soundsplice
{
/**
    A generic tab-content component that forwards resized() to a callback. Used
    for the mixer tab, whose children (channel strips, master strip) need
    repositioning whenever JUCE assigns it new bounds — on the initial layout, a
    window resize, or when the TabbedComponent switches to it.
*/
class CallbackComponent final : public juce::Component
{
public:
    std::function<void()> onResized;
    void resized() override { if (onResized) onResized(); }
};

/**
    Phase 3 UI. Owns the project document (a Song under an undo History) and a
    headless AudioEngine. The document may hold several instrument tracks; the
    piano roll edits the selected one, and the mixer tab shows a channel strip per
    track. All edits go through the history (undo/redo) and are mirrored into the
    engine's fixed track pool.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ApplicationCommandTarget,
                            private juce::ChangeListener,
                            private juce::MenuBarModel,
                            public juce::DragAndDropContainer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** True while the document differs from the file it came from. */
    bool hasUnsavedChanges() const;

    /** Runs @p onProceed once it's safe to discard the current document,
        offering to save first if there's anything to lose. Public because
        quitting has to ask too — see SoundSpliceApplication::systemRequestedQuit
        — and every destructive path must ask the same question the same way. */
    void confirmDiscardChanges(std::function<void()> onProceed);

    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    void timerCallback() override;

    // juce::ApplicationCommandTarget: every menu command and shortcut, listed
    // in CommandTable.h and performed in MainComponent_Commands.cpp.
    ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands(juce::Array<juce::CommandID>& ids) override;
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& info) override;
    bool perform(const juce::ApplicationCommandTarget::InvocationInfo& invocation) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void logAudioDeviceStatus();
    /** Switches to the system's default output when it changes — see
        AudioEngine::followSystemDefaultOutput. Guarded against re-entry
        because re-opening a device itself broadcasts a change. */
    void followSystemOutputIfEnabled();
    void updateLoopRegion();
    double loopEndBeats() const;
    void stopAtEndOfArrangement();
    void seekToBeat(double beat);
    void stepByBars(int bars);
    double songEndBeats() const;
    double playheadBeat() const;
    void   setProjectTempo(double bpm);
    void   pushTempoMap();
    void chooseFile();
    void showStatus(const juce::String& message);
    void showError(const juce::String& message);
    void showBusy(const juce::String& message);

    /** Turns a plain juce::Slider into one whose drags are undoable, the same
        "rewind to where the drag started, commit the final value as one edit"
        technique the mixer faders use — but written generically so a slider
        that lives directly in MainComponent (the master panel's) doesn't need
        its own Fader-style enum and dedicated begin/end methods the way a
        reusable component like MixerStrip does. @p read/@p write are the get
        and set for whichever model::Song field the slider controls. */
    void wireUndoableSlider(juce::Slider& slider, juce::String label,
                            std::function<float(const model::Song&)> read,
                            std::function<void(model::Song&, float)> write);
    void post(engine::EngineCommand::Type type, double a = 0.0, double b = 0.0);

    void                   editPattern(const engine::Pattern& pattern);
    void                   refreshFromModel();
    const engine::Pattern& currentPattern() const;
    void                   newProject();
    void                   createEmptyProject();
    void                   saveProject(std::function<void(bool saved)> onDone = {});
    void                   saveProjectAs(std::function<void(bool saved)> onDone = {});
    bool                   writeProjectTo(const juce::File& file);
    void                   openProject();
    void                   chooseProjectToOpen();
    void                   updateWindowTitle();
    void                   loadSongIntoEditor(const model::Song& song);
    juce::File             autosaveFile() const;
    void                   autosaveIfDue();
    void                   discardAutosave();
    void                   offerAutosaveRecovery();
    static bool            isSilentAudioFile(const juce::File& file);
    void                   exportAudioDialog();
    struct ExportTask;
    void                   exportProject(const engine::ExportOptions& options);
    std::vector<ExportTask> buildExportTasks(const juce::File& masterFile,
                                             const engine::ExportOptions& options,
                                             bool& folderFailed);
    void                   startExport(const std::vector<ExportTask>& tasks,
                                       const juce::File& masterFile);
    void                   showAudioSettings();
    void                   importAudioToNewTrack();
    void                   importAudioFileAtBeat(const juce::File& file, double startBeats,
                                                 int targetTrackIndex = -1);
    void                   previewAudioFile(const juce::File& file);
    void                   importMidiFileDialog();
    /** Import Raw Data: asks how a headerless file's samples are stored, then
        converts it to a WAV in the project's audio folder on a new track. */
    void                   importRawDataDialog();
    void                   importRawData(const juce::File& source, const engine::RawPcmFormat& format);
    void                   exportMidiFileDialog();
    void                   setProjectRootFolderDialog();

    void                   refreshAutomationPaneForSelected();
    /** Commits an edited lane for the selected track as one undo step. */
    void                   applyEditedAutomationLane(model::TrackParam param,
                                                     const model::AutomationLane& lane);
    void                   toggleRecording();
    void                   finishRecordingIfReady();

    /**
        Decides what pressing Record captures, from the armed track's type
        *and* what is actually plugged in.

        The armed track's type alone is not enough, which is exactly how this
        first shipped broken: with the default Instrument track selected,
        Record chose MIDI and captured nothing on a machine with only a
        microphone. What a track *can* hold and what there is to record *from*
        are two different questions, and both have to be asked.

        @p explanation is filled in whenever the answer is worth saying out
        loud — why nothing can be recorded, or why a take is going somewhere
        other than the armed track.
    */
    app::RecordSource      chooseRecordSource(int trackIndex, juce::String& explanation) const;

    /**
        Makes sure the OS microphone permission is settled before an audio take
        starts, prompting for it if it has never been asked and offering System
        Settings if it was refused.

        Returns true if recording can go ahead right now. False means this has
        taken over: either a prompt is up (and Record retries itself once the
        answer arrives) or the user has been told why it cannot proceed. Never
        called for a MIDI take — a controller needs no microphone, and
        prompting for one would be a non-sequitur.
    */
    bool                   ensureMicrophoneAccess();

    /** Guards the one automatic retry after a permission prompt, so a grant
        that still leaves no usable input reports that instead of prompting in
        a loop. */
    bool                   retryingAfterMicPermission_ = false;
    void                   toggleMidiRecording();
    void                   finishMidiRecordingIfReady();
    /** Turns the drained take into a clip on the target track. Split out of
        finishMidiRecordingIfReady so the beats conversion and the commit can
        be read (and reasoned about) apart from the take's lifecycle. */
    void                   commitMidiTake(int targetTrack, int64_t startSample, int64_t endSample);
    juce::File             recordingsDirectory() const;
    /** Where a new recording or edit should be written: the saved project's
        audio folder, or @p scratchDirectory for a project not saved yet. */
    juce::File             audioDirectoryFor(const juce::File& scratchDirectory) const;
    void                   collectProjectAudio(const juce::File& projectFile);
    void                   cleanUpProjectAudio();
    static model::Song     makeEmptySong();
    void                   selectTrackAndRefreshAll(int newTrackIndex);
    void                   addTrack();
    double                 beatsPerBar() const;
    void                   syncEngineTracks();
    void                   refreshPianoRollForSelected();
    void                   refreshAudioEditorForSelected();
    void                   updateMasteringControls();
    void                   setMasteringSettings(const model::MasteringSettings& settings);
    void                   beginMasteringDrag();
    void                   endMasteringDrag();
    void                   applyMasteringPreset(engine::MasteringPreset preset);
    /** The selected clip if it's an Audio clip with a file, else nullptr. */
    const model::Clip*     selectedAudioClip() const;
    void                   setSelectedClipGainDb(float gainDb);
    void                   beginClipGainDrag();
    void                   endClipGainDrag();
    void                   normaliseSelectedClip();
    // The audio-editor edit actions. Each resolves the selection, transforms
    // the samples and goes through replaceClipAudio.
    /** Runs @p transform over the selected samples, reading only those, and
        puts whatever it leaves in their place. The one path the destructive
        selection edits share. */
    bool                   editSelection(
                               const juce::String& label, bool snapToZeroCrossings,
                               const std::function<void(std::vector<std::vector<float>>& selection,
                                                        double sampleRate)>& transform);

    // The arrangement's time selection across tracks — see
    // MainComponent_TimeSelection.cpp and model/TimeSelection.h. The edit
    // commands act on it while it has length (or, for Paste, while it has
    // tracks and something was cut or copied from one); otherwise on the
    // audio editor's selection as before.
    model::TimeSelection   timeSelection_;
    model::RangeClipboard  rangeClipboard_;
    void                   setTimeSelection(const model::TimeSelection& selection);
    /** Copies and/or removes the time selection (see the .cpp). False when
        there's no time selection to act on. */
    bool                   editTimeSelection(const juce::String& label, bool copy, bool remove, bool closeGap);
    bool                   pasteAtTimeSelection();
    void                   refreshAfterArrangementEdit();

    // Edits on the arrangement itself (model/ArrangementEdits.h), on the
    // time selection's tracks, or the selected track when there isn't one.
    std::vector<int>       arrangementEditTracks() const;
    void                   splitClipsAtPlayhead();
    void                   joinArrangementClips();
    void                   duplicateTimeSelection();
    void                   showDetachAtSilencesDialog();
    void                   detachAtSilences(float thresholdDb, double minSilenceSeconds);
    void                   snapTimeSelectionToZeroCrossings();
    void                   applyEffectsToTimeSelection(const std::vector<model::EffectSlot>& chain);
    void                   previewEffectsOnTimeSelection(const std::vector<model::EffectSlot>& chain);

    void                   cutAudioSelection();
    void                   copyAudioSelection();
    void                   pasteAudioAtSelection();
    void                   deleteAudioSelection();
    void                   trimToAudioSelection();
    void                   splitClipAtSelection();
    void                   silenceAudioSelection();
    void                   fadeInAudioSelection();
    void                   fadeOutAudioSelection();
    void                   reverseAudioSelection();

    void                   showApplyEffectsDialog();
    void                   promptToSaveEffectPreset(const model::EffectSlot& slot);
    void                   deleteUserEffectPreset(const std::string& effectId, const std::string& name);
    void                   storeUserEffectPresets();
    void                   previewEffectsOnSelection(const std::vector<model::EffectSlot>& chain);
    void                   openScratchPluginEditor(int slotIndex, const model::EffectSlot& slot);
    std::vector<model::EffectSlot> withScratchPluginStates(std::vector<model::EffectSlot> chain) const;
    void                   closeScratchPluginEditors();

    // Markers — see MainComponent_Markers.cpp and model/Markers.h.
    void                   addMarkerAtPlayhead();
    void                   addMarkerFromAudioSelection();
    void                   jumpToMarker(bool forward);
    void                   renameMarkerPrompt(int markerId);
    void                   deleteMarker(int markerId);
    void                   moveMarkerTo(int markerId, double startBeats);

    /** A clip's volume curve as edited in the arrangement, one undo step. */
    void                   setClipEnvelope(int trackIndex, int clipIndex, const engine::ClipEnvelope& envelope);

    // Track channel operations — see model/TrackChannels.h.
    void                   splitSelectedTrackToMono();
    void                   swapSelectedTrackChannels();
    /** Joins the selected audio track and the one below it into one stereo
        track: back into one without rendering if they are split halves,
        otherwise by rendering each to a side of a new file. */
    void                   makeStereoTrack();
    /** Renders the edit's tracks (see arrangementEditTracks) over the time
        selection, or everything arranged, onto a new audio track. */
    void                   mixAndRenderToNewTrack();

    // Zooming the timeline onto a span of it — see app/TimelineZoom.h.
    void                   zoomTimelineToSpan(double startBeats, double lengthBeats);
    void                   zoomToTimeSelection();
    void                   fitProjectInView();
    void                   fitTracksVertically();
    void                   deleteAllMarkers();
    void                   showMarkerMenu(int markerId);
    void                   exportMarkersDialog();
    void                   importMarkersDialog();
    void                   showSpeedPitchDialog();
    void                   analyseSelection();
    void                   applySpeedAndPitch(double speedFactor, double semitones);
    void                   applyEffectsToSelection(const std::vector<model::EffectSlot>& chain);

    /** Converts between the audio editor's seconds (from the selected clip's
        start) and song beats — the one place that mapping lives. */
    double                 songBeatForClipSeconds(double secondsIntoFile) const;
    double                 clipSecondsForSongBeat(double beat) const;

    void                   captureNoisePrint();
    void                   reduceNoiseOnSelectedClip(float amountDb, float floorDb);

    /** Where destructive edits write their output. */
    juce::File             editsDirectory() const;

    /** The selected clip's audio as a sample sequence (engine/SampleSequence.h)
        and the samples of it the clip plays. Opening one reads no samples. */
    struct ClipAudio
    {
        juce::File                       file;
        engine::sequence::SampleSequence sequence;
        SampleWindow                     window;
    };
    bool                   openSelectedClipAudio(ClipAudio& out) const;

    /** Samples [from, to) counted from the clip's start, one vector per
        channel; only those samples are read. Empty if they can't be. */
    std::vector<std::vector<float>> readClipAudio(const ClipAudio& audio, int from, int to) const;

    /** Answers the audio editor's onSampleDetailNeeded: the selected clip's
        samples over [fromSeconds, toSeconds), for drawing it zoomed right in. */
    void                   sendSampleDetailToEditor(double fromSeconds, double toSeconds);
    /** Writes a stroke of the audio editor's draw tool into the selected clip. */
    void                   drawSamplesOnSelectedClip(int channel, long firstSample, const std::vector<float>& values);

    /** Samples [from, to) of the selected clip become @p replacement, of any
        length, in one undo step: new blocks for the replacement and a new
        sequence file around them, with nothing else rewritten. The single path
        every destructive edit goes through, so none of them can forget to
        update lengthBeats or to invalidate the caches. @p label names the undo
        step. */
    bool                   replaceClipAudio(const juce::String& label, const ClipAudio& audio, int from, int to,
                                            const std::vector<std::vector<float>>& replacement);

    /** The writing half of replaceClipAudio, for any clip's audio: a new
        sequence file with frames [from, to) replaced. Nothing if it failed,
        having said why. */
    std::optional<juce::File> writeEditedSequence(const juce::File& source,
                                                  const engine::sequence::SampleSequence& sequence,
                                                  std::int64_t from, std::int64_t to,
                                                  const std::vector<std::vector<float>>& replacement);

    /** Runs @p transform over every sample the selected clip plays, handed
        all channels at once and the sample rate, for the edits that change
        the whole clip (speed, pitch, noise reduction). */
    bool                   editWholeClip(
                               const juce::String& label,
                               const std::function<void(std::vector<std::vector<float>>&, double sampleRate)>& transform);

    /** The selection in the audio editor as samples from @p audio's clip
        start, or false when there isn't one. @p snapToZeroCrossings moves the
        boundaries to the nearest zero crossing, which is what stops a cut
        clicking. */
    bool                   selectedClipRange(const ClipAudio& audio, int& fromOut, int& toOut,
                                             bool snapToZeroCrossings) const;
    /** The nearest zero crossing to sample @p at of the clip. */
    int                    zeroCrossingNear(const ClipAudio& audio, int at) const;
    /** @p clipSeconds, each moved to the nearest zero crossing in the selected clip. */
    std::vector<double>    zeroCrossingsNear(std::vector<double> clipSeconds) const;
    void                   refreshEffectChainForSelected();
    void                   addEffectSlot(model::EffectKind kind, const model::PluginRef& plugin);
    void                   removeEffectSlot(int slotIndex);
    void                   moveEffectSlot(int slotIndex, int delta);
    void                   setEffectSlotBypass(int slotIndex, bool enabled);
    void                   setEffectSlotParams(const model::EffectSlot& slot, int slotIndex);
    void                   scanForPlugins();
    void                   openPluginEditor(int slotIndex);
    void                   closePluginEditors();
    void                   refreshSessionView();
    void                   addSessionScene();
    void                   deleteSessionScene(int sceneIndex);
    void                   captureClipIntoSession(int trackIndex, int sceneIndex);
    void                   previewNote(int noteNumber);
    void                   updateDelayControls();
    void                   updateFilterControls();
    void                   updateReverbControls();
    void                   updateEqControls();
    void                   updateMixerStrips();
    void                   beginFaderDrag(int trackIndex, MixerStrip::Fader fader);
    void                   endFaderDrag(int trackIndex, MixerStrip::Fader fader);
    void                   beginEffectSlotParamsDrag(int slotIndex);
    void                   endEffectSlotParamsDrag(int slotIndex);
    void                   setTrackGain(int index, float gainDb);
    void                   setTrackMuted(int index, bool muted);
    void                   setTrackSolo(int index, bool solo);
    void                   setTrackPan(int index, float pan);
    void                   selectTrack(int index);
    void                   selectTrackAndClip(int trackIndex, int clipIndex);
    void                   addClipToSelectedTrack();
    void                   setClipLength(int trackIndex, int clipIndex, double newLengthBeats);
    void                   trimClipStartTo(int trackIndex, int clipIndex, double newStartBeats);
    void                   slipClipTo(int trackIndex, int clipIndex, double newOffsetSeconds);
    void                   setClipFades(int trackIndex, int clipIndex, const engine::ClipFades& fades,
                                        const juce::String& label);
    void                   showClipMenu(int trackIndex, int clipIndex);
    void                   copyNotes();
    void                   pasteNotes();
    void                   copyClip();
    void                   pasteClip();
    void                   copyTrack();
    void                   pasteTrack();
    void                   duplicateTrackAt(int trackIndex);
    void                   duplicateClip();
    bool                   hasSelectedClip() const;
    void                   deleteSelectedClip();
    void                   deleteSelectedTrack();
    void                   deleteTrackAt(int trackIndex);
    void                   renameSelectedTrack();
    void                   renameTrackAt(int trackIndex);
    void                   showTrackSettingsMenu(int trackIndex);
    void                   setTrackColour(int trackIndex, unsigned int argb);
    void                   moveClipToTrack(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats);
    void                   quantizeNotes(double swingAmount);
    void                   setPatternBars(int bars);
    void                   setTimeSignature(int numerator, int denominator);
    void                   updateTimeSignatureControls();
    void                   updateBarsControl();
    void                   updateEditingLabel();
    void                   layoutLeftPane();
    void                   applyTransportCollapse();
    int                    panelMenuIndex(const juce::String& name) const;
    void                   togglePanel(int index);
    void                   buildDefaultDockLayout();
    /** Switches to @p workspace, saving the arrangement being left into its
        own slot first so each layout remembers your edits to it. */
    void                   applyWorkspaceLayout(layouts::Workspace workspace);
    /** Writes the current arrangement into the active layout's slot. */
    void                   saveActiveWorkspaceLayout();
    juce::String           settingsKeyForWorkspace(layouts::Workspace workspace) const;
    void                   loadDockLayout();
    void                   saveDockLayout();
    void                   layoutMixerView();
    void                   layoutMasterPanel();
    void                   setUpZoomControls(juce::Component& parent, juce::DrawableButton& icon,
                                             juce::Slider& slider, juce::Slider& box,
                                             double minZoom, double maxZoom,
                                             const juce::String& tooltip,
                                             std::function<void(float)> onZoom);
    void                   setKeysZoom(float zoom);
    void                   updateKeysZoomControls();
    void                   setKeysTimeZoom(float zoom);
    void                   followKeysPlayhead();
    void                   updateKeysTimeZoomControls();
    void                   setTimelineZoom(float zoom);
    void                   updateZoomControls();
    void                   layoutArrangeTab();
    void                   layoutEditTab();
    int                    trackCount() const;

    engine::AudioEngine         engine_;
    model::History<model::Song> history_;
    int                         selectedTrackIndex_ = 0;
    int                         selectedClipIndex_  = 0;
    bool                        recordAutomation_   = false;
    bool                        awaitingRecordedTake_ = false;

    // Chosen when the take is armed, not when it ends: the destination has to
    // exist before a note is played now that recording streams to it, and the
    // target track is whatever was selected then rather than whatever happens
    // to be selected by the time the user hits stop.
    // Non-null while an export is rendering. Owned here rather than
    // self-deleting so that quitting mid-export can stop the thread before the
    // engine it is rendering through is destroyed.
    std::unique_ptr<app::OfflineRenderJob> renderJob_;

    // Set while that job owns the engine — see timerCallback.
    bool                        offlineRenderInProgress_ = false;

    juce::File                  recordingFile_;
    int                         recordingTargetTrack_ = -1; // -1 = a new track

    // A MIDI take in progress. Separate flags from the audio take's rather
    // than one shared "recording" flag: the two takes finish through different
    // engine calls, and a single flag would make "which recorder do I ask" a
    // question with two possible answers at the moment it matters most.
    bool                        awaitingMidiTake_       = false;
    int                         midiRecordingTargetTrack_ = -1;

    // Timer ticks since MIDI inputs were last re-enumerated — see
    // timerCallback for why this is throttled rather than done every tick.
    int                         midiRescanTicks_ = 0;

    // Drained from the engine's ring on every timer tick, not only at the end
    // of the take — which is what keeps the ring small and the take unbounded
    // (see engine::MidiRecorder).
    std::vector<engine::RecordedMidiEvent> midiTakeEvents_;

    // App-level preferences (not project data): which panel lives in which
    // dock region, and the file browser's user bookmarks. Saved on the
    // panel-move/bookmark-change that produces them, not the project.
    juce::PropertiesFile settings_;

    // The file this document came from and will Save over — empty until it has
    // been saved once — plus the state id that was last written, which is what
    // hasUnsavedChanges() compares against. windowTitle_ caches what the title
    // bar already says, so the 30Hz timer only touches it on a real change.
    juce::File         projectFile_;
    unsigned long long savedStateId_ = 0;
    juce::String       windowTitle_;

    // Crash recovery (see autosaveIfDue): the state last written to the
    // autosave file (0 while none of ours is on disk), when that was, and
    // whether a recovery offer is still waiting for an answer, during which
    // the file on disk must be left alone.
    unsigned long long autosavedStateId_ = 0;
    double             lastAutosaveMs_   = 0.0;
    bool               recoveryPending_  = false;

    // How time is counted (bars and beats, a clock, samples or timecode) and
    // the timecode frame rate: app preferences, not project data (see the
    // View menu). Its sample rate follows the audio device.
    app::TimeDisplay   timeDisplay_;

    // Where a fader was grabbed, so the whole drag can be committed as one
    // undo step when it is released rather than one step per pixel.
    bool               faderDragging_  = false;
    int                faderDragTrack_ = -1;
    MixerStrip::Fader  faderDragWhich_ = MixerStrip::Fader::Gain;
    float              faderDragFrom_  = 0.0f;

    // Where an effect slot's parameters were before a drag on one of its
    // controls started, so the whole gesture can commit as one undo step —
    // same reasoning as the fader-drag members above, but for a whole
    // model::EffectSlot rather than one float (see commitStructDrag).
    bool              effectSlotDragging_ = false;
    int               effectSlotDragTrack_ = -1;
    int               effectSlotDragIndex_ = -1;
    model::EffectSlot effectSlotDragFrom_;

    // Every command, for the menus and the keyboard alike (see CommandTable.h).
    // Declared before the menu bar that watches it, so it outlives it.
    juce::ApplicationCommandManager commandManager_;

    juce::MenuBarComponent          menuBar_;

    // Every tooltip in the app was dead text until this existed: JUCE only
    // shows them while some TooltipWindow is alive to draw them.
    juce::TooltipWindow             tooltips_;

    // Transient messages. A child of this component rather than of any pane,
    // so collapsing or closing a pane can't hide what the app is telling you.
    StatusBanner                    status_;

    // The whole dockable workspace: a tree of tab groups the user arranges by
    // dragging tabs (onto a region's middle to add a tab there, onto an edge
    // to split it). See DockWorkspace; the default arrangement this app ships
    // with is built in buildDefaultDockLayout().
    DockWorkspace      workspace_;
    FileBrowserPanel   fileBrowser_;
    CallbackComponent  leftPane_; // the transport controls (Play/Stop/...), a panel like any other

    // Playback transport, left to right. Play/pause is one toggle rather than
    // two buttons; the old separate Stop is gone, since pausing and returning
    // to the start are now distinct controls (pause, and first-frame).
    juce::DrawableButton firstFrameButton    { "First",    juce::DrawableButton::ImageFitted };
    juce::DrawableButton previousFrameButton { "Previous", juce::DrawableButton::ImageFitted };
    juce::DrawableButton playPauseButton     { "PlayPause", juce::DrawableButton::ImageFitted };
    juce::DrawableButton nextFrameButton     { "Next",     juce::DrawableButton::ImageFitted };
    juce::DrawableButton lastFrameButton     { "Last",     juce::DrawableButton::ImageFitted };
    juce::DrawableButton recordButton { "Record", juce::DrawableButton::ImageFitted };
    juce::TextButton   addTrackButton { "Add Track" };
    juce::ToggleButton loopButton      { "Loop" };
    // Collapses the transport pane to its first row, so the pane can be
    // dragged down to a single strip when the readouts aren't wanted.
    juce::TextButton   collapseTransportButton_;
    bool               transportCollapsed_ = false;
    juce::ToggleButton metronomeButton { "Click" };
    juce::ToggleButton monitorButton { "Monitor" };
    juce::ComboBox     countInBox_;
    juce::ComboBox     timeSigBox_;
    juce::Label        timeSigLabel_;

    juce::Slider       tempoSlider, masterSlider;
    juce::ToggleButton filterButton { "Filter" };
    juce::ComboBox     filterModeBox_;
    juce::Slider       filterCutoffSlider, filterResoSlider;
    juce::ToggleButton delayButton { "Delay" };
    juce::Slider       delayTimeSlider, delayFbSlider, delayMixSlider;
    juce::ToggleButton reverbButton { "Reverb" };
    juce::Slider       reverbRoomSlider, reverbDampSlider, reverbMixSlider;
    juce::ToggleButton eqButton { "EQ" };
    juce::Slider       eqBassSlider, eqMidSlider, eqTrebleSlider;
    EqCurveView        eqCurveView_;
    juce::ToggleButton autoRecButton   { "Rec Auto" };
    juce::TextButton   autoClearButton { "Clr Auto" };
    juce::Label        tempoLabel  { {}, "Tempo" };
    juce::Label        masterLabel { {}, "Master" };
    juce::Label  positionLabel, clipLabel;

    juce::MidiKeyboardComponent        keyboard_ { engine_.keyboardState(),
                                                   juce::MidiKeyboardComponent::horizontalKeyboard };
    LevelMeter                         meter_;

    CallbackComponent                  editTab_;
    juce::Label                        editingLabel_;
    juce::Label                        barsLabel_ { {}, "Bars" };
    juce::ComboBox                     barsBox_; // pattern length of the open clip
    PianoRoll                          pianoRoll_;

    EffectChainPanel                   effectChain_;
    juce::OwnedArray<PluginEditorWindow> pluginWindows_;
    SessionView                        sessionView_;
    AudioEditorPane                    audioEditor_; // its own dock panel — see refreshAudioEditorForSelected
    MasteringPane                      masteringPane_; // ditto — see updateMasteringControls
    AnalyserPane                       analyserPane_;
    AutomationPane                     automationPane_;
    bool                               masteringDragging_ = false;
    model::MasteringSettings           masteringDragFrom_;
    // Follows the system's default output (headphones being plugged in,
    // say) rather than holding whichever device was default at launch.
    // Persisted, and switchable off for anyone deliberately running a fixed
    // interface — see the View menu.
    layouts::Workspace                 activeWorkspace_ = layouts::Workspace::MusicCreation;
    bool                               followSystemOutput_ = true;
    bool                               switchingDevice_    = false;

    bool                               clipGainDragging_ = false;
    int                                clipGainDragTrack_ = -1;
    int                                clipGainDragClip_  = -1;
    float                              clipGainDragFrom_  = 0.0f;

    // The captured noise print, one profile per channel, plus the file it
    // was measured from — a print is only meaningful for the recording it
    // came from, so it's dropped when the selection moves to another.
    // Audio clipboard, deliberately separate from the note/clip/track ones
    // above: Cmd-C already means Copy Notes, and this app's rule is that a
    // command means one thing rather than depending on focus.
    std::vector<std::vector<float>>    audioClipboard_;
    double                             audioClipboardSampleRate_ = 0.0;

    std::vector<engine::NoiseProfile>  noiseProfiles_;
    juce::File                         noiseProfileFile_;

    // The peaks the audio editor draws, and the clip window they came from.
    // refreshAudioEditorForSelected() runs on ~26 unrelated edits, so this
    // is cached by file path, offset and length — rebuilding would re-read
    // the file every time anything in the app changed.
    WaveformPeaks                      waveformPeaks_;
    juce::String                       waveformPeaksKey_;
    double                             waveformPeaksSampleRate_ = 0.0;

    // The user's saved effect presets (kept in the app settings, see
    // storeUserEffectPresets), and the Apply Effects dialog while one is
    // open, so a preset saved in it appears in it straight away.
    std::vector<model::UserEffectPreset>              userEffectPresets_;
    juce::Component::SafePointer<ApplyEffectsDialog> applyEffectsDialog_;

    // Plugin editors opened from that dialog, each on an instance of its own
    // (see openScratchPluginEditor). The window is declared after the
    // instance, so it is destroyed first: an editor must never outlive the
    // plugin it draws.
    struct ScratchPluginEditor
    {
        int                                        slotIndex = -1;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        std::unique_ptr<PluginEditorWindow>        window;
    };
    std::vector<std::unique_ptr<ScratchPluginEditor>> scratchPluginEditors_;

    CallbackComponent                  arrangeTab_;
    juce::Viewport                     arrangementViewport_;
    ArrangementView                    arrangementView_;
    // Timeline zoom: a magnifying glass labelling a slider, with an editable
    // multiplier beside it. Replaced a pair of unlabelled +/- buttons.
    juce::DrawableButton               zoomIcon_ { "Zoom", juce::DrawableButton::ImageFitted };
    juce::Slider                       zoomSlider_;
    juce::Slider                       zoomBox_;

    // The same control for the keys pane, zooming the pitch axis.
    juce::DrawableButton               keysZoomIcon_ { "Zoom", juce::DrawableButton::ImageFitted };
    juce::Slider                       keysZoomSlider_;
    juce::Slider                       keysZoomBox_;

    // ...and again for the time axis, which scrolls in this viewport once the
    // grid is wider than the pane.
    juce::DrawableButton               keysTimeZoomIcon_ { "Zoom", juce::DrawableButton::ImageFitted };
    juce::Slider                       keysTimeZoomSlider_;
    juce::Slider                       keysTimeZoomBox_;
    juce::Viewport                     keysViewport_;
    juce::ToggleButton                 keysFollowButton_;
    juce::TextButton                   addClipButton_       { "Add Clip" };

    CallbackComponent                  mixerView_;
    CallbackComponent                  masterPanel_; // own top-level dock tab; see layoutMasterPanel()
    juce::OwnedArray<MixerStrip>       trackStrips_;
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_;

    // An app-level clipboard holding model values, deliberately not the system
    // clipboard: pasting between two running copies of the app isn't worth a
    // serialization format yet. Notes and clips are kept apart so the Edit
    // menu's commands can say exactly what they act on, rather than depending
    // on which pane happens to have focus.
    std::vector<engine::Note> noteClipboard_;
    std::vector<model::Clip>  clipClipboard_;

    // Its own buffer rather than sharing the clip one: pasting a track when a
    // clip was copied, or the reverse, is the kind of guess that loses work.
    std::optional<model::Track> trackClipboard_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace soundsplice
