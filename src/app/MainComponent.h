#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "engine/AudioEngine.h"
#include "engine/DrumKitStyle.h"
#include "engine/GuitarTone.h"
#include "engine/MasteringPreset.h"
#include "engine/MidiCapture.h"
#include "app/RecordSourceChoice.h"
#include "app/MicrophonePermission.h"
#include "engine/AudioEdits.h"
#include "engine/AudioExport.h"
#include "engine/TimeStretch.h"
#include "engine/NoiseReduction.h"
#include "engine/SynthTone.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/PresetSerialization.h"
#include "model/Song.h"

#include "OfflineRenderJob.h"

#include "ArrangementView.h"
#include "DockWorkspace.h"
#include "DrumsPane.h"
#include "EffectChainPanel.h"
#include "EqCurveView.h"
#include "AnalyserPane.h"
#include "AutomationPane.h"
#include "ApplyEffectsDialog.h"
#include "AudioEditorPane.h"
#include "MasteringPane.h"
#include "WorkspaceLayouts.h"
#include "FretboardPane.h"
#include "FileBrowserPanel.h"
#include "LevelMeter.h"
#include "MixerStrip.h"
#include "PianoRoll.h"
#include "PluginEditorWindow.h"
#include "ChordStamp.h"
#include "ClipLengthRepair.h"
#include "DragCommit.h"
#include "TrackSelection.h"
#include "SessionView.h"
#include "TrackColours.h"
#include "StatusBanner.h"
#include "SynthEditor.h"

namespace looper
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
                            private juce::ChangeListener,
                            private juce::MenuBarModel,
                            public juce::DragAndDropContainer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /** True while the document differs from the file it came from. */
    bool hasUnsavedChanges() const;

    /** Runs @p onProceed once it's safe to discard the current document,
        offering to save first if there's anything to lose. Public because
        quitting has to ask too — see LooperAudioApplication::systemRequestedQuit
        — and every destructive path must ask the same question the same way. */
    void confirmDiscardChanges(std::function<void()> onProceed);

    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    void timerCallback() override;
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
    void   setTempoAtPlayhead(double bpm);
    void   editTempoChangeAt(double beat);
    void   applyTempoChange(double beat, double bpm);
    void   toggleTempoRamp(double beat);
    void   moveTempoChange(double fromBeat, double toBeat);
    void   removeTempoChangeAt(double beat);
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
    /** @p isRecordedTake suppresses tempo detection: a take was just played
        against this project's own click, so it is at the project tempo by
        definition. Detecting a tempo for it could only ever agree (pointless)
        or disagree (wrong, and on a confident mis-detection it would stretch
        the performance the user just gave). */
    void                   importAudioFileAtBeat(const juce::File& file, double startBeats,
                                                 int targetTrackIndex = -1,
                                                 bool isRecordedTake = false);
    void                   previewAudioFile(const juce::File& file);
    void                   importMidiFileDialog();
    void                   exportMidiFileDialog();
    void                   setProjectRootFolderDialog();
    void                   repairRecordedClipLengths();

    // --- Tempo-aware audio clips (see docs/PLAN.md §29).
    /** The time-stretch factor a clip needs to sit at the project tempo, or
        1.0 when it isn't warped or its own tempo isn't known. */
    /** The engine-pool index of the track with @p trackId, or -1. The bridge
        between the document's stable ids and the engine's positional pool. */
    int                    trackIndexForId(int trackId) const;

    double                 warpFactorFor(const model::Clip& clip) const;
    engine::TempoEstimate  detectTempoForClip(const model::Clip& clip);
    void                   addBusTrack();
    void                   refreshAutomationPaneForSelected();
    /** Commits an edited lane for the selected track as one undo step. */
    void                   applyEditedAutomationLane(model::TrackParam param,
                                                     const model::AutomationLane& lane);
    void                   setTrackOutputBus(int index, int busTrackId);
    void                   toggleClipWarp();
    void                   detectSelectedClipTempo();
    void                   setProjectTempoFromClip();
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
    juce::File             presetsDirectory() const;
    void                   refreshPresetList();
    void                   savePresetDialog();
    void                   applyPreset(int index);
    void                   deletePresetAt(int index);
    void                   seedFactoryPresets();
    juce::File             factoryDrumKitDirectory() const;
    void                   seedFactoryDrumKit();
    model::DrumKit         defaultDrumKitWithFactorySamples() const;
    model::Song            makeStarterSong() const;
    void                   selectTrackAndRefreshAll(int newTrackIndex);
    void                   addTrack();
    void                   addDrumTrack();
    void                   addGuitarTrack();
    void                   refreshFretboardForSelected();
    void                   setTrackGuitarSettings(const model::GuitarSettings& settings);
    void                   stampChord(const engine::ChordShape& shape, int fretOffset,
                                      const engine::StrumSettings& strum);
    void                   playChordAtFret(engine::MovableShape shape, int rootString, int fret,
                                           const engine::StrumSettings& strum, bool writeToClip);
    const model::Track*    guitarTrackForChords();
    double                 beatsPerBar() const;
    bool                   commitStampedNotes(const std::vector<engine::Note>& notes,
                                              const juce::String& what, double atBeats);
    void                   assignDrumSample(int padIndex, const juce::File& file);
    void                   setDrumPadMix(int padIndex, const model::DrumPad& pad);
    void                   addDrumPad();
    void                   removeDrumPad(int padIndex);
    int                    selectedDrumTrackIndex() const;
    void                   syncEngineTracks();
    void                   refreshPianoRollForSelected();
    void                   refreshSynthEditorForSelected();
    void                   refreshDrumsPaneForSelected();
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
    // the samples and goes through applyDestructiveEdit.
    /** Runs @p transform over the selected range, reading the clip once.
        The one path the destructive selection edits share. */
    bool                   editSelection(
                               const juce::String& label, bool snapToZeroCrossings,
                               const std::function<void(std::vector<std::vector<float>>&,
                                                        int from, int to, double sampleRate)>& transform);

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
    void                   showSpeedPitchDialog();
    void                   analyseSelection();
    void                   applySpeedAndPitch(double speedFactor, double semitones);
    void                   applyEffectsToSelection(const std::vector<model::EffectSlot>& chain);

    /** Converts between the audio editor's file-seconds and song beats —
        the one place that mapping lives. */
    double                 songBeatForClipSeconds(double secondsIntoFile) const;
    double                 clipSecondsForSongBeat(double beat) const;

    void                   captureNoisePrint();
    void                   reduceNoiseOnSelectedClip(float amountDb, float floorDb);

    /** Where destructive edits write their output. */
    juce::File             editsDirectory() const;

    /** Runs @p transform over every channel of the selected clip's audio,
        writes the result to a new file and repoints the clip at it in one
        undo step. The single path every destructive edit goes through, so
        none of them can forget to update lengthBeats or to invalidate the
        caches. @p label names the undo step. */
    bool                   applyDestructiveEdit(
                               const juce::String& label,
                               const std::function<std::vector<float>(const std::vector<float>&, int channel)>& transform);

    /** As above, but handed every channel at once and the file's sample
        rate. Effects are stereo processors — a reverb's width and a
        compressor's linked detector both need both channels together — so
        the per-channel signature above can't express them. */
    bool                   applyDestructiveEditToAllChannels(
                               const juce::String& label,
                               const std::function<void(std::vector<std::vector<float>>&, double sampleRate)>& transform);

    /** The selection in the audio editor as sample indices into @p clip's
        file, or false when there isn't one. @p snapToZeroCrossings moves the
        boundaries to the nearest zero crossing, which is what stops a cut
        clicking. */
    bool                   selectedSampleRange(int& fromOut, int& toOut, int& lengthOut,
                                               double& sampleRateOut,
                                               std::vector<std::vector<float>>& channelsOut,
                                               bool snapToZeroCrossings) const;
    /** Reads @p file fully into per-channel float vectors, or an empty
        result if it can't be read. Message thread; used by the audio
        editor's offline operations. */
    std::vector<std::vector<float>> readAudioFileChannels(const juce::File& file,
                                                          double& sampleRateOut) const;
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
    void                   setTrackSynthSettings(const model::SynthSettings& settings);
    void                   previewNote(int noteNumber);
    void                   previewChord(const std::vector<engine::Note>& notes);
    void                   updateDelayControls();
    void                   updateFilterControls();
    void                   updateReverbControls();
    void                   updateEqControls();
    void                   updateSendBusControls();
    void                   updateSendBusEffectVisibility();
    void                   updateMixerStrips();
    void                   beginFaderDrag(int trackIndex, MixerStrip::Fader fader);
    void                   endFaderDrag(int trackIndex, MixerStrip::Fader fader);
    void                   beginEffectSlotParamsDrag(int slotIndex);
    void                   endEffectSlotParamsDrag(int slotIndex);
    void                   beginSynthSettingsDrag();
    void                   endSynthSettingsDrag();
    void                   beginGuitarSettingsDrag();
    void                   endGuitarSettingsDrag();
    void                   setTrackGain(int index, float gainDb);
    void                   setTrackMuted(int index, bool muted);
    void                   setTrackSolo(int index, bool solo);
    void                   setTrackPan(int index, float pan);
    void                   setTrackSendLevel(int index, float level);
    void                   selectTrack(int index);
    void                   selectTrackAndClip(int trackIndex, int clipIndex);
    void                   addClipToSelectedTrack();
    void                   showGenerateLoopDialog();
    void                   setClipLength(int trackIndex, int clipIndex, double newLengthBeats);
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
    void                   setTrackType(int trackIndex, model::TrackType newType);
    void                   moveClipToTrack(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats);
    void                   applyGuitarTone(engine::GuitarTone tone);
    void                   applySynthTone(engine::SynthTone tone);
    void                   applyDrumKitStyle(engine::DrumKitStyle style);
    model::DrumKit         kitForDrumKitStyle(engine::DrumKitStyle style) const;
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

    // Same technique again, for the Synth pane's settings — see
    // beginSynthSettingsDrag/endSynthSettingsDrag.
    bool                  synthSettingsDragging_ = false;
    int                   synthSettingsDragTrack_ = -1;
    model::SynthSettings  synthSettingsDragFrom_;

    // Same technique again, for the fretboard's settings — see
    // beginGuitarSettingsDrag/endGuitarSettingsDrag.
    bool                   guitarSettingsDragging_ = false;
    int                    guitarSettingsDragTrack_ = -1;
    model::GuitarSettings  guitarSettingsDragFrom_;

    // The preset list SynthEditor is currently showing, in the same order —
    // presetBox_'s indices are indices into this. Reloaded from disk by
    // refreshPresetList() whenever a preset is saved or deleted.
    std::vector<juce::File> presetFiles_;

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
    juce::TextButton   addDrumTrackButton_ { "Add Drum" };
    juce::TextButton   addGuitarTrackButton_ { "Add Guitar" };
    juce::TextButton   addBusTrackButton_ { "Add Bus" };
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
    juce::ToggleButton sendBusButton { "Send FX" };
    juce::ComboBox     sendEffectTypeBox_;
    juce::Slider       sendRoomSlider, sendDampSlider; // shown when the send bus effect is Reverb
    juce::Slider       sendDelayTimeSlider, sendDelayFbSlider; // shown when it's Delay
    juce::Slider       sendReturnSlider;
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

    SynthEditor                        synthEditor_; // its own dock panel — see refreshSynthEditorForSelected
    DrumsPane                          drumsPane_;   // ditto — see refreshDrumsPaneForSelected
    EffectChainPanel                   effectChain_;
    juce::OwnedArray<PluginEditorWindow> pluginWindows_;
    SessionView                        sessionView_;
    FretboardPane                      fretboard_; // ditto — see refreshTrackEffectsForSelected
    AudioEditorPane                    audioEditor_; // ditto — see refreshAudioEditorForSelected
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

    // The peaks the audio editor draws, and the file they came from.
    // refreshAudioEditorForSelected() runs on ~26 unrelated edits, so this
    // is cached by path — rebuilding would re-read the file every time
    // anything in the app changed.
    WaveformPeaks                      waveformPeaks_;
    juce::File                         waveformPeaksFile_;
    double                             waveformPeaksSampleRate_ = 0.0;

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
    juce::TextButton                   generateLoopButton_  { "Generate Loop..." };

    CallbackComponent                  mixerView_;
    CallbackComponent                  masterPanel_; // own top-level dock tab; see layoutMasterPanel()
    juce::OwnedArray<MixerStrip>       trackStrips_;
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_;

    // Advanced per stamp so two identical chords humanise differently —
    // a repeated strum that lands identically is the thing humanising is
    // meant to avoid.
    unsigned int chordStampSeed_ = 1;

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

} // namespace looper
