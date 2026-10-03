#pragma once

#include <set>
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
#include "engine/HqStretch.h"
#include "engine/NoiseReduction.h"
#include "engine/RawPcm.h"
#include "engine/Generators.h"
#include "engine/RoomTone.h"
#include "engine/CenterChannel.h"
#include "engine/SilenceDetection.h"
#include "engine/Loudness.h"
#include "engine/MatchEq.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/AutomationWriter.h"
#include "model/Song.h"
#include "model/TimeSelection.h"

#include "OfflineRenderJob.h"

#include "ArrangementView.h"
#include "DockWorkspace.h"
#include "EffectChainPanel.h"
#include "EqCurveView.h"
#include "AnalyserPane.h"
#include "BatchProcess.h"
#include "model/Favorites.h"
#include "DiagnosticsPane.h"
#include "EssentialSoundPane.h"
#include "CommandPalette.h"
#include "KeyboardShortcutsDialog.h"
#include "MacrosDialog.h"
#include "ScriptPane.h"
#include "ExportAudioDialog.h"
#include "FormDialog.h"
#include "ProjectInfoDialog.h"
#include "RenderQueueDialog.h"
#include "DeliveryPane.h"
#include "HistoryPane.h"
#include "TranscriptPane.h"
#include "PreviewStrip.h"
#include "VideoPane.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "Screensets.h"
#include "LoudnessMatch.h"
#include "AutomationPane.h"
#include "ApplyEffectsDialog.h"
#include "AudioEditorPane.h"
#include "MasteringPane.h"
#include "WorkspaceLayouts.h"
#include "FileBrowserPanel.h"
#include "LevelMeter.h"
#include "LoudnessReadout.h"
#include "StereoScopeView.h"
#include "MixerStrip.h"
#include "OpenFiles.h"
#include "OpenFilesPane.h"
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
    /** @p headless: for a render with no window (renderHeadless) - no audio
        device is opened, nothing is autosaved or offered for recovery, and
        the window layout isn't saved. */
    explicit MainComponent(bool headless = false);
    ~MainComponent() override;

    /** `SoundSplice --render`, which soundsplice-cli runs: opens @p project
        and exports it to @p out exactly as File > Export Audio would - the
        same tasks and the same renderer - on this thread. False on failure;
        @p report says what was written, or why not. */
    bool renderHeadless(const juce::File& project, const juce::File& out,
                        const engine::ExportOptions& options, juce::String& report);
    bool renderHeadless(const juce::File& project, const juce::File& out,
                        const app::ExportChoice& choice, juce::String& report);

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

public:
    /** Reads a clip's audio as it plays, on the render thread; see scanClipAudio. */
    struct ClipScan
    {
        virtual ~ClipScan() = default;
        virtual void prepare(double sampleRate) = 0;
        /** Two outputs, @p frames long. */
        virtual void process(const float* const* outputs, int frames) = 0;
    };

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
    /** Play-at-speed: sets how fast the song plays and says so. */
    void setPlaySpeed(double speed);
    bool scrubStartedPlayback_ = false; // a ruler scrub started the transport, so stops it
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
    void                   exportAudioDialog(std::optional<app::ExportChoice> initial = std::nullopt);
    struct ExportTask;
    void                   exportProject(const app::ExportChoice& choice);
    std::vector<ExportTask> tasksForChoice(const juce::File& chosenFile, const app::ExportChoice& choice, bool& folderFailed);
    void                   addReportDetails(std::vector<ExportTask>& tasks) const;
    struct TaskOutcome;
    TaskOutcome            renderAndWrite(const ExportTask& task, std::optional<double>& mixGainDb);
    void                   queueExport(app::ExportChoice choice);
    void                   promptSaveRenderPreset(const app::ExportChoice& choice);
    void                   saveRenderQueue();
    void                   showRenderQueue();
    void                   runDeliveryCheck(int spec);
    void                   exportToDeliverySpec(int spec);
    void                   refreshHistoryPane(bool force = false);
    void                   loadReferenceTrack();
    void                   measureMixForReference(std::function<void()> then);
    void                   setReferenceComparing(bool on);
    void                   switchReferenceAB();
    void                   refreshTranscriptPane(bool force = false);
    void                   transcribeSelectedTrack();
    void                   deleteTranscriptRanges(const std::vector<std::pair<double, double>>& ranges);
    void                   chooseTranscriptionModel();
    /** A dialog remembering its fields in settings_ (see FormDialog.h);
        previewedDialog's has a Preview strip as well. */
    FormDialog             dialog(const juce::String& title, const juce::String& message = {});
    FormDialog             previewedDialog(const juce::String& title, const juce::String& message = {});
    void                   addPreviewStrip(juce::AlertWindow* window, std::function<void()> run);
    void                   previewEffect(const std::function<void()>& run, preview::Mode mode);
    void                   playPreview(preview::Mode mode);
    void                   endPreview();
    void                   loadVideo();
    bool                   capturePreview(const std::vector<std::vector<float>>& original,
                                          const std::vector<std::vector<float>>& processed, double sampleRate);
    engine::ExportTags     exportTagsFor(double startBeats, double lengthBeats, app::ExportTagging tagging) const;
    void                   showProjectInfo();
    void                   exportCdImage();
    std::vector<ExportTask> buildExportTasks(const juce::File& masterFile,
                                             const engine::ExportOptions& options,
                                             bool& folderFailed,
                                             double startBeats = 0.0,
                                             double lengthBeats = -1.0);
    std::vector<ExportTask> buildRangeExportTasks(const juce::File& chosenFile,
                                                  const engine::ExportOptions& options,
                                                  app::ExportRange range, const juce::String& namePattern,
                                                  bool& folderFailed,
                                                  double selectionStartBeats = 0.0,
                                                  double selectionLengthBeats = 0.0);
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
    void                   applyEditedAutomationLane(const AutomationTarget& target,
                                                     const model::AutomationLane& lane);
    void                   toggleRecording();
    void                   finishRecordingIfReady();
    int                    makeLoopTakesFromRecording(const juce::File& file, int64_t startedAt, int latencySamples);
    int                    recordingLatencySamples();
    void                   compensateRecordingLatency(const juce::File& file, int latencySamples);
    void                   showRecordingLatencyDialog();
    void                   measureRecordingLatency();
    void                   finishLatencyMeasurementIfReady();
    bool                   measuringLatency_ = false;
    void                   showRecordingFormatDialog();
    engine::AudioRecorder::Format savedRecordFormat();
    engine::AudioRecorder::Format recordFormatFor(int trackIndex);
    void                   setTrackArmed(int trackIndex, bool armed);
    void                   joinOrLeaveTake(int trackIndex, bool armed);
    void                   chooseTrackInput(int trackIndex);
    bool                   punchRecordedClip(const juce::File& file);
    void                   saveRecentInput();
    void                   recordAtEndOfTrack();
    void                   applySoundTrigger();
    void                   showSoundActivatedDialog();
    void                   showTimerRecordDialog();
    void                   tickTimerRecord();
    static constexpr double kRecentInputSeconds = 120.0;

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

    // The Open Files list — see app/OpenFiles.h.
    /** Prunes the list and redraws the pane. */
    void                   updateOpenFilesPane();
    /** Selects open file @p clipId, so the Audio editor shows it. */
    void                   showOpenFile(int clipId);
    /** Removes @p clipId from the list; if it was showing, the editor moves to
        the next open file, or to nothing. */
    void                   closeOpenFile(int clipId);
    void                   closeAllOpenFiles();
    void                   stepOpenFile(int direction);
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
    void                   normaliseSelectedClipTo(float targetPeak);
    void                   showNormalizeDialog();
    void                   normalizeWithOptions(float targetPeak, bool removeDc, bool independently);
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
    bool                   editRazorAreas(const juce::String& label, bool copy, bool remove);
    bool                   pasteRazorClipboard();
    void                   setRazorAreas(const model::RazorAreas& areas);
    void                   moveRazorAreas(double deltaBeats, int deltaTracks);

    // Razor areas (model/RazorEdits.h), what Copy last took from them, and
    // whether that was the last copy made, so Paste puts back the right one.
    model::RazorAreas      razorAreas_;
    model::RazorClipboard  razorClipboard_;
    bool                   razorClipboardIsLatest_ = false;
    void                   refreshAfterArrangementEdit();

    // Edits on the arrangement itself (model/ArrangementEdits.h), on the
    // time selection's tracks, or the selected track when there isn't one.
    std::vector<int>       arrangementEditTracks() const;
    void                   splitClipsAtPlayhead();
    void                   joinArrangementClips();
    void                   duplicateTimeSelection();
    void                   showDetachAtSilencesDialog();
    void                   detachAtSilences(float thresholdDb, double minSilenceSeconds);
    std::optional<std::vector<std::pair<double, double>>> silencesInClip(const model::Clip& clip, float thresholdDb,
                                                                         double minSilenceSeconds) const;
    void                   showAutoDuckDialog();
    void                   autoDuck(float thresholdDb, double duckDb, double fadeSeconds, double pauseSeconds);
    void                   showTruncateSilenceDialog();
    void                   truncateSilence(float thresholdDb, double minSilenceSeconds, double keepSeconds);
    void                   showRepeatDialog();
    void                   repeatTimeSelection(int times);
    void                   showChangeTempoDialog();
    void                   changeTempoOfSelectedClip(double percent);
    void                   showPaulstretchDialog();
    void                   paulstretchSelectedClip(double stretch, double windowSeconds);
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
    void                   studioFadeOutAudioSelection();

    // Restoration — see MainComponent_Repair.cpp.
    bool                   editSelectionInContext(
        const juce::String& label, int contextFrames,
        const std::function<bool(std::vector<std::vector<float>>&, int from, int to, double sampleRate)>& transform);
    void                   repairAudioSelection();
    void                   showClickRemovalDialog();
    void                   removeClicksInSelection(double sensitivity, double maxWidthMs);
    void                   showClipFixDialog();
    void                   fixClippingInSelection(double thresholdPercent, double reduceDb);
    void                   showHumRemovalDialog();
    void                   removeHumInSelection(double fundamentalHz, int harmonics, double q);
    void                   scaleSpectralSelection(const juce::String& label, float gain);
    void                   showSpectralGainDialog();
    void                   repairSpectralSelection();
    void                   showVocalReductionDialog();
    void                   showCrossfadeTracksDialog();
    void                   showPitchCorrectionDialog();
    void                   detectPitch();
    void                   showAdaptiveNoiseReductionDialog();
    void                   showSpeechEnhancementDialog();
    void                   showDecrackleDialog();
    void                   showDereverbDialog();
    void                   reduceVocals(const engine::centre::Settings& settings);
    void                   showSpectralClipEditDialog();
    void                   addSpectralClipEdit(float gainDb);
    void                   removeSpectralClipEdits();
    void                   showSpectrogramSettingsDialog();
    void                   loadSpectrogramSettings();
    void                   repairPaintedSpectrum(const spectrogramimage::Brush& brush);
    void                   applySpectralEdit(const juce::String& label,
                                             const std::function<bool(std::vector<float>&, double rate, double lowHz,
                                                                      double highHz)>& edit);
    void                   showSpectralEqDialog();
    void                   showSpectralShelfDialog();
    void                   crossfadeClipsInSelection();

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
    /** Asks for a sample rate, then resampleSelectedTrack. */
    void                   showResampleTrackDialog();
    /** Converts every audio file the selected track plays to @p sampleRate,
        on the render thread, and points its clips at the copies. */
    void                   resampleSelectedTrack(double sampleRate);
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
    void                   applySpeedAndPitch(double speedFactor, double semitones, bool keepFormants);
    void                   showSlidingStretchDialog();
    void                   applySlidingStretch(const engine::hqstretch::Slide& slide);
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
    bool                   openClipAudio(const model::Clip& clip, ClipAudio& out) const;
    void                   showMatchLoudnessDialog();
    void                   runDiagnostics();
    void                   selectDiagnostic(const DiagnosticsPane::Row& row);
    void                   fixDiagnostic(const DiagnosticsPane::Row& row);
    void                   fixAllDiagnostics(engine::diagnostics::Kind kind);
    bool                   fixDiagnosticRange(engine::diagnostics::Kind kind, AudioRange range);
    void                   removeDcOffsetInSelection();
    void                   startBatchProcess(std::optional<std::vector<model::EffectSlot>> chain = std::nullopt);
    void                   applyFavorite(int index);
    void                   showCommandPalette();
    std::vector<palette::Entry> paletteEntries();
    void                   refreshEssentialSoundForSelected();
    void                   setEssentialRole(model::SoundRole role);
    void                   setEssentialAmount(const std::string& task, float amount);
    void                   endEssentialDrag();
    void                   matchLoudnessForRole(model::SoundRole role, double lufs);
    void                   duckUnderDialogue(model::SoundRole role, float depthDb);
    void                   promptSaveFavorite(std::vector<model::EffectSlot> chain);
    void                   removeFavorite(int index);
    std::vector<model::Favorite> favorites_; // the Favorites menu, kept in the app's settings
    void                   chooseBatchChain(std::vector<juce::File> inputs);
    void                   chooseBatchOptions(std::vector<juce::File> inputs, std::vector<model::EffectSlot> chain);
    void                   runBatch(std::vector<juce::File> inputs, batch::Settings settings);
    void                   matchLoudness(const std::vector<app::MatchedClip>& clips, double targetLufs, bool limitTruePeak);

    /** Samples [from, to) counted from the clip's start, one vector per
        channel; only those samples are read. Empty if they can't be. */
    std::vector<std::vector<float>> readClipAudio(const ClipAudio& audio, int from, int to) const;

    // The Analyze menu — see MainComponent_Analyze.cpp.
    void                   scanClipAudio(const juce::String& title, const juce::String& activity, const ClipAudio& audio,
                                         int from, int to, std::shared_ptr<ClipScan> scan,
                                         std::function<void(double sampleRate)> onScanned);
    bool                   selectedScanRange(ClipAudio& audio, int& from, int& to, bool& whole);
    void                   showAmplitudeStatistics();
    void                   findClipping();
    void                   showLabelSoundsDialog();
    void                   labelSounds(float thresholdDb, double minSilenceSeconds, double minSoundSeconds);
    void                   showBeatFinderDialog();
    void                   findBeats(double sensitivity, double minGapSeconds);
    std::optional<double>  selectionRmsDb();
    void                   setContrastBackground();
    void                   measureContrast();
    void                   addMarkerRangesInClip(int clipId, const std::vector<engine::silence::FrameRange>& runs,
                                                 int offset, double sampleRate, const juce::String& name,
                                                 bool numbered, const juce::String& label);

    // Loudness — see MainComponent_Loudness.cpp.
    void                   measureClipLoudness(const juce::String& title, const ClipAudio& audio, int from, int to,
                                               std::function<void(const engine::LoudnessReport&)> onMeasured);
    void                   measureLoudnessOfSelection();
    void                   captureRoomTone();
    void                   chooseImpulseResponse(int slotIndex, bool browse);
    void                   setImpulseResponse(int slotIndex, const juce::File& file);
    void                   setMatchEqReference();
    void                   matchEqToReference();
    std::optional<engine::SpectrumAverager> measureSpectrumOfSelection();
    void                   showNormalizeLoudnessDialog();

    // The Generate menu — see MainComponent_Generate.cpp.
    std::vector<int>       generateTargetTracks() const;
    void                   showGenerateDialog(engine::GeneratorKind kind);
    void                   generateAudio(const engine::GeneratorSpec& spec);
    void                   normalizeSelectedClipLoudness(double targetLufs, bool limitTruePeak);

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
    void                   useClipTake(int trackIndex, int clipId, double fromBeats, double toBeats, int take);
    void                   combineOverlappingClipsIntoTakes(int trackIndex, int clipIndex);
    void                   swipeCompTake(int trackIndex, int take, double fromBeats, double toBeats);
    void                   syncOpenPluginStates();
    void                   closePluginEditor(PluginEditorWindow* window);
    static model::EffectSlot* pluginSlotFor(model::Song& song, const PluginSlotAddress& at);
    engine::PluginNode*    pluginNodeFor(const PluginEditorWindow& window);
    void                   notePluginStateToEngine(const PluginSlotAddress& at, const std::string& state);
    int                    pluginStateSyncTicks_ = 0;
    void                   showPluginManager();
    void                   showKeyboardShortcuts();
    void                   startProject(const model::Song& song);
    void                   newFromTemplate(model::Song song);
    juce::File             templatesFolder() const;
    std::vector<juce::File> userTemplates() const;
    void                   saveAsTemplate();
    void                   showPreferences(int tab = 0);
    void                   applyTheme();
    void                   announce(const juce::String& text, bool important = false);
    juce::String           describeTrack(int index) const;
    juce::String           describeClip(int trackIndex, int clipIndex) const;
    juce::String           describeSelection() const;
    void                   selectAdjacentTrack(int delta);
    void                   selectAdjacentClip(int delta);
    void                   nudgeSelectedClip(double beats);
    void                   setSelectionEdgeAtPlayhead(bool start);
    void                   announceWhereAmI();
    void                   applyScreenset(int index);
    void                   promptSaveScreenset();
    void                   removeScreenset(int index);
    void                   chooseCustomAccent(juce::Component& near);
    std::vector<prefs::Page> preferencePages();
    bool                   applyChainToSelection(const std::vector<model::EffectSlot>& chain, const juce::String& what);
    void                   toggleMacroRecording();
    void                   noteMacroCommand(juce::CommandID id);
    void                   noteMacroEffects(const std::vector<model::EffectSlot>& chain);
    bool                   runMacro(int index);
    scripting::Host        makeScriptHost();
    void                   runScript(const juce::String& code, const juce::String& name);
    void                   chooseScriptToRun();
    void                   chooseMacroForFiles();
    void                   runMacroOnFiles(int index);
    void                   showMacros();
    void                   saveMacros();
    void                   editMacroEffects(int macro, int step, std::vector<model::EffectSlot> current);
    void                   pluginListsChanged();

    // The plugin manager while it's open, to refresh after a scan.
    juce::Component::SafePointer<class PluginManagerDialog> pluginManager_;
    juce::Component::SafePointer<KeyboardShortcutsDialog>   shortcutsDialog_;
    juce::Component::SafePointer<MacrosDialog>              macrosDialog_;
    juce::Component::SafePointer<PreferencesDialog>         preferencesDialog_;

    // Macros (Tools menu), kept in the app's settings. While one is being
    // recorded, recordingMacro_ gathers the commands and effects used.
    struct CommandSpy final : juce::ApplicationCommandManagerListener
    {
        std::function<void(juce::CommandID)> onInvoked;
        void applicationCommandInvoked(const juce::ApplicationCommandTarget::InvocationInfo& info) override
        {
            if (onInvoked)
                onInvoked(info.commandID);
        }
        void applicationCommandListChanged() override {}
    };
    std::vector<macros::Macro>     macros_;
    std::optional<macros::Macro>   recordingMacro_;
    bool                           runningMacro_ = false;
    bool                           headless_     = false;
    std::vector<screensets::Screenset> screensets_; // View > Layout, kept in the app's settings
    std::vector<app::exportchoices::Preset> renderPresets_; // Export Audio's Preset box
    std::vector<app::exportchoices::Job>    renderQueue_;   // File > Render Queue
    juce::Component::SafePointer<RenderQueueDialog> renderQueueDialog_;
    CommandSpy                     commandSpy_;
    void                   setTrackEditGroup(int trackIndex, int group);
    void                   addBusTrack();
    void                   toggleClipWarp(int trackIndex, int clipId);
    void                   detectClipTempo(int trackIndex, int clipId);
    void                   askClipTempo(int trackIndex, int clipId);
    void                   songTempoFromClip(int trackIndex, int clipId);
    double                 tempoAtPlayhead() const;
    void                   afterTempoEdit();
    void                   editTempoChangeAt(double beat);
    void                   removeTempoChangeAt(double beat);
    void                   toggleTempoRamp(double beat);
    void                   moveTempoChange(double from, double to);

    // The tempo map last handed to the engine, so it's only sent again when
    // it has changed.
    std::vector<engine::TempoChange> pushedTempoMap_;
    void                   chooseTrackOutput(int trackIndex);
    void                   showSendsMenu(int trackIndex);
    void                   setSendLevel(int trackIndex, int send, float levelDb);
    void                   beginSendDrag(int trackIndex, int send);
    void                   endSendDrag(int trackIndex, int send);
    void                   pushTrackRouting(int trackIndex);
    void                   chooseSidechain(int slotIndex);

    // A send level being dragged on a strip, by (track, send), committed as
    // one undo step.
    PendingDrag<std::pair<int, int>, float> sendDrag_;
    void                   toggleFolder(int trackIndex);
    void                   indentTrack(int trackIndex);
    void                   outdentTrack(int trackIndex);
    std::vector<int>       linkedTracks(int trackIndex) const;
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

    // ---- recording automation (MainComponent_AutomationWrite.cpp) ----
    /** Which lane a control writes: the master's (track -1), a track's volume
        or pan (slot -1, param "gain" or "pan"), or an effect parameter. */
    struct AutomationWriteKey
    {
        int               track = -1;
        int               slot  = -1;
        model::EffectKind kind  = model::EffectKind::Filter;
        std::string       param;

        bool operator==(const AutomationWriteKey& other) const;
        static AutomationWriteKey master() { return {}; }
        static AutomationWriteKey trackParam(int track, model::TrackParam param);
        static AutomationWriteKey effect(int track, int slot, model::EffectKind kind, std::string param)
        {
            return { track, slot, kind, std::move(param) };
        }
    };

    struct AutomationWrite
    {
        AutomationWriteKey key;
        model::LaneWriter  writer;
        float              value    = 0.0f;
        bool               touching = false;
    };

    model::AutomationLane*  automationLaneFor(model::Song& song, const AutomationWriteKey& key);
    bool                    isWritingAutomation(const AutomationWriteKey& key) const;
    engine::TrackAutomation engineAutomationFor(int trackIndex, const model::Track& track) const;
    void                    openAutomationPass();
    void                    automationControlMoved(const AutomationWriteKey& key, float value, bool touching);
    void                    automationControlReleased(const AutomationWriteKey& key);
    void                    tickAutomationWrites();
    void                    closeAutomationPass();
    void                    setAutomationMode(model::AutomationMode mode);
    void                    chooseTrackAutomationMode(int trackIndex);
    model::AutomationMode   automationModeFor(int trackIndex) const;
    bool                    anyTrackInWriteMode() const;

    model::AutomationMode        automationMode_     = model::AutomationMode::Read;
    std::vector<AutomationWrite> automationWrites_;
    bool                         automationPassOpen_ = false;
    model::Song                  automationPassBefore_;
    double                       automationPassBeat_ = 0.0;
    bool                        awaitingRecordedTake_ = false;

    // A take recorded round the loop (Loop on, and a time selection to loop):
    // its passes become the takes of one clip. See MainComponent_Recording.cpp.
    bool   loopRecording_       = false;
    bool   punchRecording_      = false; // over loopRecordFrom/ToBeats_, when not looping

    // Timer record: when the take starts, and, if it has one, when it stops.
    bool       timerRecordPending_ = false;

    // Tracks armed to record, by id - session state, not part of the song.
    // With any armed, a take records each from its own input; the first is
    // the main take, the rest extras (AudioEngine::beginExtraRecording).
    std::set<int> armedTrackIds_;
    struct ExtraTake
    {
        int trackIndex = -1;
        int slot       = -1;
    };
    std::vector<ExtraTake> extraTakes_;
    juce::Time timerRecordStart_, timerRecordStop_;
    double loopRecordFromBeats_ = 0.0;
    double loopRecordToBeats_   = 0.0;

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
    // Also the song as the drag began: its edit group's faders move from it.
    struct FaderDragStart
    {
        float       value = 0.0f;
        model::Song song;
    };
    PendingDrag<std::pair<int, MixerStrip::Fader>, FaderDragStart> faderDrag_;
    const model::Song* faderDragBaseFor(int trackIndex, MixerStrip::Fader fader) const;

    // Where an effect slot's parameters were before a drag on one of its
    // controls started, so the whole gesture can commit as one undo step —
    // same reasoning as the fader-drag members above, but for a whole
    // model::EffectSlot rather than one float (see commitStructDrag).
    /** Which effect chain the effects panel edits: a track's own (clip -1)
        or one of its clips'. By index, so it can be found again in whichever
        copy of the song an undoable edit is working on. */
    struct EffectChainRef
    {
        int  track = -1;
        int  clip  = -1;
        bool isClip() const noexcept { return clip >= 0; }
        bool operator==(const EffectChainRef&) const = default;
    };

    EffectChainRef                         editedChainRef() const;
    static std::vector<model::EffectSlot>* chainAt(model::Song& song, const EffectChainRef& ref);
    const std::vector<model::EffectSlot>*  editedChain() const;
    void pushEffectSlotToEngine(const EffectChainRef& ref, int slotIndex, const model::EffectSlot& slot);

    PendingDrag<std::pair<EffectChainRef, int>, model::EffectSlot> effectSlotDrag_; // by (chain, slot)

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
    CommandPalette                  palette_;

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
    juce::TextButton   addBusButton { "Add Bus" };
    juce::ToggleButton loopButton      { "Loop" };
    // Collapses the transport pane to its first row, so the pane can be
    // dragged down to a single strip when the readouts aren't wanted.
    juce::TextButton   collapseTransportButton_;
    bool               transportCollapsed_ = false;
    juce::ToggleButton metronomeButton { "Click" };
    juce::ToggleButton monitorButton { "Monitor" };
    juce::ComboBox     countInBox_;
    LevelMeter         inputMeter_; // what's coming in, with a clip light
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
    juce::ComboBox     autoModeBox;
    juce::TextButton   autoClearButton { "Clr Auto" };
    juce::Label        tempoLabel  { {}, "Tempo" };
    juce::Label        masterLabel { {}, "Master" };
    juce::Label  positionLabel, clipLabel;

    juce::MidiKeyboardComponent        keyboard_ { engine_.keyboardState(),
                                                   juce::MidiKeyboardComponent::horizontalKeyboard };
    LevelMeter                         meter_;
    LoudnessReadout                    loudnessReadout_;
    StereoScopeView                    stereoScope_;
    std::vector<std::pair<float, float>> scopePairs_; // reused each tick

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
    DiagnosticsPane                    diagnosticsPane_;
    EssentialSoundPane                 essentialSoundPane_;
    ScriptPane                         scriptPane_;
    DeliveryPane                       deliveryPane_;
    HistoryPane                        historyPane_;
    TranscriptPane                     transcriptPane_;
    VideoPane                          videoPane_;

    // Preview before apply (PreviewStrip.h): while previewing_, the edit
    // functions run their transform on up to kPreviewSeconds of the
    // selection and keep the before and after here instead of committing.
    static constexpr double            kPreviewSeconds = 10.0;
    bool                               previewing_ = false;
    std::vector<std::vector<float>>    previewOriginal_, previewProcessed_;
    double                             previewRate_ = 0.0;
    std::unique_ptr<PreviewStrip>      previewStrip_;
    unsigned long long                 transcriptShown_ = 0; // the state and track the pane last showed
    unsigned long long                 historyShown_ = 0; // what the History pane last showed (refreshHistoryPane)

    // A/B against a reference (Transport menu): its name and loudness, and the
    // mix's as last measured, with the document state it was measured at.
    juce::String                       referenceName_;
    double                             referenceLufs_       = 0.0;
    double                             mixLufs_             = 0.0;
    unsigned long long                 mixMeasuredAtState_  = 0;
    bool                               mixMeasured_         = false;
    PendingDrag<std::monostate, model::EssentialSettings> essentialDrag_; // the selected clip's, as a task slider was grabbed
    int                                diagnosedClipId_ = 0; // the clip diagnosticsPane_'s rows are for
    app::OpenFiles                     openFiles_;
    OpenFilesPane                      openFilesPane_;
    AutomationPane                     automationPane_;
    PendingDrag<std::monostate, model::MasteringSettings> masteringDrag_;
    // Follows the system's default output (headphones being plugged in,
    // say) rather than holding whichever device was default at launch.
    // Persisted, and switchable off for anyone deliberately running a fixed
    // interface — see the View menu.
    layouts::Workspace                 activeWorkspace_ = layouts::Workspace::MusicCreation;
    bool                               followSystemOutput_ = true;
    bool                               switchingDevice_    = false;

    PendingDrag<std::pair<int, int>, float> clipGainDrag_; // by (track, clip)

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
    engine::SpectrogramSettings        spectrogramSettings_;
    std::optional<std::array<double, engine::ThirdOctaveEq::kBands>> matchEqReference_; // Match EQ's aim
    juce::String                       matchEqReferenceName_;
    std::shared_ptr<const engine::RoomToneProfile> roomTone_; // Generate > Room Tone's source
    std::optional<double>              contrastBackgroundDb_;   // Analyze > Contrast's background level
    bool                               autoCrossfades_ = true;  // overlapping clips crossfade as they're moved
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
