#include "MainComponent.h"

#include "ScrollFollow.h"
#include "Shortcuts.h"

#include "Icons.h"

#include "engine/ClipSlot.h"
#include "engine/DefaultContent.h"
#include "engine/DrumSynth.h"
#include "app/ExportAudioDialog.h"
#include "app/RowWrapLayout.h"
#include "app/OfflineRenderJob.h"
#include "app/RenderProgress.h"
#include "app/StemNaming.h"
#include "engine/EffectSlotFactory.h"
#include "engine/GenerativeLoop.h"
#include "engine/GuitarChords.h"
#include "engine/NoteOps.h"
#include "engine/MidiFileIO.h"
#include "engine/OfflineRenderer.h"
#include "model/GenrePresets.h"
#include "model/GuitarTonePresets.h"
#include "model/MasteringPresets.h"
#include "model/Serialization.h"
#include "model/SynthTonePresets.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace looper
{
/** View-menu ids for panels start well clear of the fixed commands, so adding
    a pane can never collide with one. */
static constexpr int kFirstPanelMenuId = 100;

/** Layout entries in the View menu. Between the fixed commands (1..32) and
    the panel toggles (kFirstPanelMenuId upward), which is the only free
    range — the panel list is unbounded above, so this can't sit past it. */
static constexpr int kFirstLayoutMenuId = 40;

/** Colour entries in the per-track gear menu, clear of that menu's own
    fixed items. */
static constexpr int kFirstColourMenuId = 200;

/** Instrument-type entries in the per-track gear menu, clear of both the
    fixed items and the colour range above (8 colours today, comfortable
    headroom either way). Encoded as kFirstTrackTypeMenuId + the
    model::TrackType enum value, so the callback needs no separate lookup
    table to get back from a menu id to a type. */
static constexpr int kFirstTrackTypeMenuId = 300;

/** How close to the edge the playhead gets before the keys grid pages. Small,
    so almost the whole width is travelled before each jump. */
static constexpr int kKeysFollowMargin = 24;

/** Extra beats rendered past the last clip when bouncing, so reverb and delay
    tails decay into the file instead of being chopped off at the final beat.
    Two bars at 4/4 — comfortably longer than the master reverb's tail at its
    largest room size. */
static constexpr double kBounceTailBeats = 8.0;

/** What Normalize aims the loudest sample at. Just under full scale rather
    than at it: a clip normalised to exactly 1.0 has no headroom left for the
    track fader, pan law, or any effect that can overshoot, so it would be
    the first thing to clip the master bus. */
static constexpr float kNormaliseTargetPeak = 0.98f;

/** The time signatures offered. A fixed list because these are the ones
    people write in; a free numerator and denominator invites 4/7, which the
    rest of the app would have to have an opinion about. */
struct TimeSignatureOption { int numerator, denominator; };

static constexpr TimeSignatureOption kTimeSignatures[] = {
    { 4, 4 }, { 3, 4 }, { 2, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 12, 8 },
};

static constexpr int kNumTimeSignatures = (int) (sizeof(kTimeSignatures) / sizeof(kTimeSignatures[0]));

using Cmd = engine::EngineCommand::Type;



namespace
{
    /** Adds a menu item that advertises its shortcut. PopupMenu's plain
        addItem overload has nowhere to put one, and an undiscoverable
        shortcut may as well not exist. */
    void addItem(juce::PopupMenu& menu, int id, const juce::String& text,
                 const juce::KeyPress& shortcut, bool enabled = true)
    {
        juce::PopupMenu::Item item(text);
        item.itemID                = id;
        item.isEnabled             = enabled;
        item.shortcutKeyDescription = shortcut.getTextDescriptionWithIcons();
        menu.addItem(std::move(item));
    }

    /** The choices showGenerateLoopDialog's root-note/scale combo boxes
        offer, and what each selected index maps to. Kept together (rather
        than as magic indices scattered through the dialog code) so the
        combo box order and the enum/semitone mapping can't drift apart. */
    constexpr const char* kGenerateLoopRootNoteNames[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    constexpr const char* kGenerateLoopScaleNames[7] =
        { "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic", "Dorian", "Mixolydian",
          "Phrygian" };
    constexpr engine::ScaleType kGenerateLoopScaleTypes[7] =
        { engine::ScaleType::Major, engine::ScaleType::NaturalMinor, engine::ScaleType::MajorPentatonic,
          engine::ScaleType::MinorPentatonic, engine::ScaleType::Dorian, engine::ScaleType::Mixolydian,
          engine::ScaleType::Phrygian };

    /** The Genre combo's options, index 0 = "None" (today's exact behaviour:
        Density drives density, no swing, no synth-preset change) followed by
        one entry per engine::Genre. Index 1..N maps to kGenerateLoopGenres
        [index - 1], the same offset-by-one scheme everywhere a combo box has
        a "none of these" option ahead of a fixed enum list. */
    constexpr engine::Genre kGenerateLoopGenres[8] =
        { engine::Genre::House, engine::Genre::Techno, engine::Genre::HipHop,
          engine::Genre::Trap, engine::Genre::Ambient, engine::Genre::LoFi,
          engine::Genre::Synthwave, engine::Genre::Cyberpunk };

    /** "Undo Delete track" rather than a bare "Undo". Every edit already
        records what it was; not showing it left the user to remember what
        they'd done, which is the one thing undo exists to spare them. */
    juce::String withAction(const char* verb, bool available, const std::string& action)
    {
        juce::String text(verb);
        if (available && ! action.empty())
            text += " " + juce::String(action);
        return text;
    }

    /** "Play / pause  (space)" — a control with no menu entry has nowhere
        else to say what its shortcut is. */
    juce::String withShortcut(const juce::String& text, const juce::KeyPress& key)
    {
        return text + "  (" + key.getTextDescriptionWithIcons() + ")";
    }

    juce::PropertiesFile::Options makeSettingsOptions()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Looper-Audio";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Looper-Audio";
        opts.osxLibrarySubFolder = "Application Support";
        return opts;
    }
}

MainComponent::MainComponent()
    : settings_(makeSettingsOptions())
{
    menuBar_.setModel(this);
    addAndMakeVisible(menuBar_);

    // Dockable workspace: a tree of tab groups, arranged entirely by dragging
    // tabs (see DockWorkspace). The panel registry below is the one place
    // that maps a panel's name to the Component behind it — the workspace
    // moves panels around by name from then on, including when restoring a
    // saved layout.
    addAndMakeVisible(workspace_);
    workspace_.onLayoutChanged = [this] { saveDockLayout(); };

    // ---- document: a starter synth track, a drum track with a programmed
    // loop and real sounds, and a guitar track with a riff and a full pedal
    // chain — so a fresh launch is audible immediately rather than opening
    // on silence. See makeStarterSong ----
    {
        seedFactoryDrumKit();
        history_.reset(makeStarterSong());
    }

    // ---- transport ----
    // Play/pause is one control: pausing leaves the playhead where it is, and
    // returning to the start is first-frame's job. That's why the old separate
    // Stop button is gone rather than kept alongside.
    playPauseButton.onClick = [this]
    {
        if (engine_.isPlaying())
        {
            post(Cmd::SetPlaying, 0.0);
            if (awaitingRecordedTake_)
                engine_.stopRecording(); // the transport stopping would also end
                                         // the take, but this makes it explicit
        }
        else
        {
            // At the end of the arrangement, play starts it again rather than
            // resuming into silence and stopping immediately — which is what
            // it would otherwise do, and would read as a dead button.
            if (engine::shouldRestartFromStart(
                    uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), songEndBeats()))
            {
                seekToBeat(0.0);
            }

            post(Cmd::SetPlaying, 1.0);
        }
    };

    firstFrameButton.onClick    = [this] { seekToBeat(0.0); };
    previousFrameButton.onClick = [this] { stepByBars(-1); };
    nextFrameButton.onClick     = [this] { stepByBars(+1); };
    lastFrameButton.onClick     = [this] { seekToBeat(songEndBeats()); };

    {
        auto play  = icons::fromSvg(icons::kPlay);
        auto pause = icons::fromSvg(icons::kPause);
        playPauseButton.setImages(play.get(), nullptr, nullptr, nullptr, pause.get());
    }
    {
        auto first = icons::fromSvg(icons::kFirstFrame);
        firstFrameButton.setImages(first.get());
    }
    {
        auto previous = icons::fromSvg(icons::kPreviousFrame);
        previousFrameButton.setImages(previous.get());
    }
    {
        auto next = icons::fromSvg(icons::kNextFrame);
        nextFrameButton.setImages(next.get());
    }
    {
        auto last = icons::fromSvg(icons::kLastFrame);
        lastFrameButton.setImages(last.get());
    }

    for (auto* button : { &firstFrameButton, &previousFrameButton, &playPauseButton,
                          &nextFrameButton, &lastFrameButton })
    {
        // The glyphs are the control; a button background would only box them in.
        button->setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
        button->setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    }

    // The transport has no menu to advertise its shortcuts from, so its
    // tooltips carry them — built from the same KeyPress the app listens for.
    firstFrameButton.setTooltip(withShortcut("Go to start", keys::toStart));
    previousFrameButton.setTooltip(withShortcut("Back one bar", keys::backOneBar));
    playPauseButton.setTooltip(withShortcut("Play / pause", keys::playPause));
    nextFrameButton.setTooltip(withShortcut("Forward one bar", keys::onOneBar));
    lastFrameButton.setTooltip(withShortcut("Go to end", keys::toEnd));
    loopButton.onClick = [this]
    {
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        updateLoopRegion();
    };
    loopButton.setTooltip(withShortcut("Loop over what's arranged", keys::loop));
    recordButton.onClick = [this] { toggleRecording(); };
    {
        // One control, two states: the disc arms, the square stops. They're
        // this button's normal and "on" images, so which one shows follows
        // getToggleState() — see toggleRecording, which sets it.
        auto record = icons::fromSvg(icons::kRecordButton);
        auto stop   = icons::fromSvg(icons::kRecordStopButton);
        recordButton.setImages(record.get(), nullptr, nullptr, nullptr, stop.get());
    }
    // The icons carry their own ring, so a button background would only box
    // them in.
    recordButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    recordButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    recordButton.setTooltip(withShortcut("Record", keys::record));
    // Collapse toggle: hides everything below the button row, leaving just the
    // transport controls. The dock region's height is the user's to set by
    // dragging its divider — this is what makes a one-row pane worth dragging
    // down to, rather than resizing the region from under them.
    followSystemOutput_ = settings_.getValue("followSystemOutput", "1") != "0";
    arrangementView_.setSnapToGrid(settings_.getValue("snapClipsToGrid", "1") != "0");
    transportCollapsed_ = settings_.getValue("transportCollapsed", "0") != "0";
    collapseTransportButton_.onClick = [this]
    {
        transportCollapsed_ = ! transportCollapsed_;
        settings_.setValue("transportCollapsed", transportCollapsed_ ? "1" : "0");
        settings_.saveIfNeeded();
        applyTransportCollapse();
    };
    leftPane_.addAndMakeVisible(collapseTransportButton_);

    leftPane_.addAndMakeVisible(firstFrameButton);
    leftPane_.addAndMakeVisible(previousFrameButton);
    leftPane_.addAndMakeVisible(playPauseButton);
    leftPane_.addAndMakeVisible(nextFrameButton);
    leftPane_.addAndMakeVisible(lastFrameButton);
    leftPane_.addAndMakeVisible(recordButton);
    leftPane_.addAndMakeVisible(loopButton);

    // Click + count-in. Both are app preferences rather than project data —
    // how you like to record, not part of the song — so they persist through
    // settings_ alongside the dock layout.
    metronomeButton.onClick = [this]
    {
        engine_.setMetronomeEnabled(metronomeButton.getToggleState());
        settings_.setValue("metronomeEnabled", metronomeButton.getToggleState());
        settings_.saveIfNeeded();
    };
    metronomeButton.setToggleState(settings_.getBoolValue("metronomeEnabled", false),
                                   juce::dontSendNotification);
    engine_.setMetronomeEnabled(metronomeButton.getToggleState());
    leftPane_.addAndMakeVisible(metronomeButton);

    // Input monitoring. Off by default and it stays that way unless asked:
    // monitoring a laptop's built-in microphone through its speakers is a
    // feedback loop, and one that starts the moment the button is pressed.
    monitorButton.setTooltip("Hear the audio input while you play. "
                             "Use headphones - monitoring a built-in microphone "
                             "through speakers will feed back.");
    monitorButton.onClick = [this]
    {
        engine_.setInputMonitoring(monitorButton.getToggleState());
        settings_.setValue("inputMonitoring", monitorButton.getToggleState());
        settings_.saveIfNeeded();
    };
    monitorButton.setToggleState(settings_.getBoolValue("inputMonitoring", false),
                                 juce::dontSendNotification);
    engine_.setInputMonitoring(monitorButton.getToggleState());
    leftPane_.addAndMakeVisible(monitorButton);

    countInBox_.addItem("No count-in", 1);
    countInBox_.addItem("1 bar", 2);
    countInBox_.addItem("2 bars", 3);
    countInBox_.onChange = [this]
    {
        const int bars = juce::jmax(0, countInBox_.getSelectedId() - 1);
        engine_.setCountInBars(bars);
        settings_.setValue("countInBars", bars);
        settings_.saveIfNeeded();
    };
    countInBox_.setSelectedId(juce::jlimit(0, 2, settings_.getIntValue("countInBars", 0)) + 1,
                              juce::dontSendNotification);
    engine_.setCountInBars(juce::jmax(0, countInBox_.getSelectedId() - 1));
    leftPane_.addAndMakeVisible(countInBox_);

    leftPane_.onResized = [this] { layoutLeftPane(); };
    applyTransportCollapse(); // apply whatever state was restored above

    // ---- sliders ----
    tempoSlider.setRange(40.0, 240.0, 0.1);
    tempoSlider.setValue(120.0, juce::dontSendNotification);
    tempoSlider.setTextValueSuffix(" bpm");
    // Time signature. A fixed list rather than two spin boxes: these are the
    // ones anyone actually writes in, and a free numerator invites 4/7.
    for (int i = 0; i < kNumTimeSignatures; ++i)
    {
        const auto& sig = kTimeSignatures[i];
        timeSigBox_.addItem(juce::String(sig.numerator) + "/" + juce::String(sig.denominator), i + 1);
    }
    timeSigBox_.setTooltip("Time signature - sets the bar length, and the grid in the Tracks and Keys panes");
    timeSigBox_.onChange = [this]
    {
        const int index = timeSigBox_.getSelectedId() - 1;
        if (index >= 0 && index < kNumTimeSignatures)
            setTimeSignature(kTimeSignatures[index].numerator, kTimeSignatures[index].denominator);
    };
    leftPane_.addAndMakeVisible(timeSigBox_);
    timeSigLabel_.setText("Time", juce::dontSendNotification);
    timeSigLabel_.attachToComponent(&timeSigBox_, true);

    tempoSlider.onValueChange = [this]
    {
        // Edits whichever tempo is in force at the playhead, not always the
        // one at beat 0 — with a tempo map, "the tempo" is a position-dependent
        // question, and a slider that always wrote beat 0 would silently edit
        // a different part of the song from the one being listened to.
        setTempoAtPlayhead(tempoSlider.getValue());
    };
    leftPane_.addAndMakeVisible(tempoSlider);
    tempoLabel.attachToComponent(&tempoSlider, true);

    positionLabel.setFont(juce::Font(juce::FontOptions(20.0f)));
    positionLabel.setText("Bar 1  Beat 1   |   0.00 s   |   STOPPED", juce::dontSendNotification);
    leftPane_.addAndMakeVisible(positionLabel);

    clipLabel.setText("No clip loaded", juce::dontSendNotification);
    leftPane_.addAndMakeVisible(clipLabel);

    // ==== everything below lives in the Mixer tab ====

    addTrackButton.onClick = [this] { addTrack(); };
    mixerView_.addAndMakeVisible(addTrackButton);

    addDrumTrackButton_.onClick = [this] { addDrumTrack(); };
    addGuitarTrackButton_.onClick = [this] { addGuitarTrack(); };
    addBusTrackButton_.onClick = [this] { addBusTrack(); };
    mixerView_.addAndMakeVisible(addDrumTrackButton_);
    mixerView_.addAndMakeVisible(addGuitarTrackButton_);
    mixerView_.addAndMakeVisible(addBusTrackButton_);

    // ---- master panel: its own dock tab (see workspace_.registerPanel
    // below), not a pull-out inside Mixer — it applies to the whole song,
    // not to any one track, so it doesn't belong nested inside the pane
    // that's specifically about per-track strips. ----
    masterPanel_.onResized = [this] { layoutMasterPanel(); };

    masterSlider.setRange(-60.0, 6.0, 0.1);
    masterSlider.setValue(0.0, juce::dontSendNotification);
    masterSlider.setTextValueSuffix(" dB");
    masterSlider.onValueChange = [this]
    {
        const float db = (float) masterSlider.getValue();
        post(Cmd::SetMasterGainDb, db);
        if (recordAutomation_ && engine_.isPlaying())
        {
            const double beat = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
            history_.mutableCurrent().masterGainDb.addPoint(beat, db);
        }
    };
    masterPanel_.addAndMakeVisible(masterSlider);
    masterLabel.attachToComponent(&masterSlider, true);

    // ---- master filter (stored in the document) ----
    filterButton.onClick = [this]
    {
        const bool on = filterButton.getToggleState();
        history_.edit(on ? "Enable master filter" : "Disable master filter",
                      [on](model::Song& s) { s.filter.enabled = on; });
        engine_.setMasterFilterEnabled(on);
    };
    masterPanel_.addAndMakeVisible(filterButton);

    filterModeBox_.addItem("Low-pass", 1);
    filterModeBox_.addItem("High-pass", 2);
    filterModeBox_.addItem("Band-pass", 3);
    filterModeBox_.setSelectedId(1, juce::dontSendNotification);
    filterModeBox_.onChange = [this]
    {
        const int mode = juce::jmax(0, filterModeBox_.getSelectedId() - 1);
        history_.edit("Set master filter mode", [mode](model::Song& s) { s.filter.mode = mode; });
        engine_.setMasterFilterMode(mode);
    };
    masterPanel_.addAndMakeVisible(filterModeBox_);

    filterCutoffSlider.setRange(20.0, 18000.0, 1.0);
    filterCutoffSlider.setSkewFactorFromMidPoint(1000.0);
    filterCutoffSlider.setValue(1000.0, juce::dontSendNotification);
    filterCutoffSlider.setTextValueSuffix(" Hz");
    filterCutoffSlider.onValueChange = [this]
    {
        const float hz = (float) filterCutoffSlider.getValue();
        history_.mutableCurrent().filter.cutoff = hz;
        engine_.setMasterFilterCutoff(hz);
    };
    wireUndoableSlider(filterCutoffSlider, "Set master filter cutoff",
                       [](const model::Song& s) { return s.filter.cutoff; },
                       [](model::Song& s, float v) { s.filter.cutoff = v; });
    masterPanel_.addAndMakeVisible(filterCutoffSlider);

    filterResoSlider.setRange(0.1, 5.0, 0.01);
    filterResoSlider.setValue(0.707, juce::dontSendNotification);
    filterResoSlider.setTextValueSuffix(" Q");
    filterResoSlider.onValueChange = [this]
    {
        const float q = (float) filterResoSlider.getValue();
        history_.mutableCurrent().filter.resonance = q;
        engine_.setMasterFilterResonance(q);
    };
    wireUndoableSlider(filterResoSlider, "Set master filter resonance",
                       [](const model::Song& s) { return s.filter.resonance; },
                       [](model::Song& s, float v) { s.filter.resonance = v; });
    masterPanel_.addAndMakeVisible(filterResoSlider);

    // ---- master delay (stored in the document, so it saves + restores) ----
    delayButton.onClick = [this]
    {
        const bool on = delayButton.getToggleState();
        history_.edit(on ? "Enable master delay" : "Disable master delay",
                      [on](model::Song& s) { s.delay.enabled = on; });
        engine_.setMasterDelayEnabled(on);
    };
    masterPanel_.addAndMakeVisible(delayButton);

    delayTimeSlider.setRange(20.0, 1000.0, 1.0);
    delayTimeSlider.setValue(300.0, juce::dontSendNotification);
    delayTimeSlider.setTextValueSuffix(" ms");
    delayTimeSlider.onValueChange = [this]
    {
        const float ms = (float) delayTimeSlider.getValue();
        history_.mutableCurrent().delay.timeMs = ms;
        engine_.setMasterDelayTimeMs(ms);
    };
    wireUndoableSlider(delayTimeSlider, "Set master delay time",
                       [](const model::Song& s) { return s.delay.timeMs; },
                       [](model::Song& s, float v) { s.delay.timeMs = v; });
    masterPanel_.addAndMakeVisible(delayTimeSlider);

    delayFbSlider.setRange(0.0, 95.0, 1.0);
    delayFbSlider.setValue(35.0, juce::dontSendNotification);
    delayFbSlider.setTextValueSuffix(" %");
    delayFbSlider.onValueChange = [this]
    {
        const float fb = (float) (delayFbSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.feedback = fb;
        engine_.setMasterDelayFeedback(fb);
    };
    wireUndoableSlider(delayFbSlider, "Set master delay feedback",
                       [](const model::Song& s) { return s.delay.feedback; },
                       [](model::Song& s, float v) { s.delay.feedback = v; });
    masterPanel_.addAndMakeVisible(delayFbSlider);

    delayMixSlider.setRange(0.0, 100.0, 1.0);
    delayMixSlider.setValue(30.0, juce::dontSendNotification);
    delayMixSlider.setTextValueSuffix(" %");
    delayMixSlider.onValueChange = [this]
    {
        const float mix = (float) (delayMixSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.mix = mix;
        engine_.setMasterDelayMix(mix);
    };
    wireUndoableSlider(delayMixSlider, "Set master delay mix",
                       [](const model::Song& s) { return s.delay.mix; },
                       [](model::Song& s, float v) { s.delay.mix = v; });
    masterPanel_.addAndMakeVisible(delayMixSlider);

    // ---- master reverb (stored in the document) ----
    reverbButton.onClick = [this]
    {
        const bool on = reverbButton.getToggleState();
        history_.edit(on ? "Enable master reverb" : "Disable master reverb",
                      [on](model::Song& s) { s.reverb.enabled = on; });
        engine_.setMasterReverbEnabled(on);
    };
    masterPanel_.addAndMakeVisible(reverbButton);

    reverbRoomSlider.setRange(0.0, 100.0, 1.0);
    reverbRoomSlider.setValue(50.0, juce::dontSendNotification);
    reverbRoomSlider.setTextValueSuffix(" room");
    reverbRoomSlider.onValueChange = [this]
    {
        const float v = (float) (reverbRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.roomSize = v;
        engine_.setMasterReverbRoomSize(v);
    };
    wireUndoableSlider(reverbRoomSlider, "Set master reverb room size",
                       [](const model::Song& s) { return s.reverb.roomSize; },
                       [](model::Song& s, float v) { s.reverb.roomSize = v; });
    masterPanel_.addAndMakeVisible(reverbRoomSlider);

    reverbDampSlider.setRange(0.0, 100.0, 1.0);
    reverbDampSlider.setValue(50.0, juce::dontSendNotification);
    reverbDampSlider.setTextValueSuffix(" damp");
    reverbDampSlider.onValueChange = [this]
    {
        const float v = (float) (reverbDampSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.damping = v;
        engine_.setMasterReverbDamping(v);
    };
    wireUndoableSlider(reverbDampSlider, "Set master reverb damping",
                       [](const model::Song& s) { return s.reverb.damping; },
                       [](model::Song& s, float v) { s.reverb.damping = v; });
    masterPanel_.addAndMakeVisible(reverbDampSlider);

    reverbMixSlider.setRange(0.0, 100.0, 1.0);
    reverbMixSlider.setValue(30.0, juce::dontSendNotification);
    reverbMixSlider.setTextValueSuffix(" %");
    reverbMixSlider.onValueChange = [this]
    {
        const float v = (float) (reverbMixSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.mix = v;
        engine_.setMasterReverbMix(v);
    };
    wireUndoableSlider(reverbMixSlider, "Set master reverb mix",
                       [](const model::Song& s) { return s.reverb.mix; },
                       [](model::Song& s, float v) { s.reverb.mix = v; });
    masterPanel_.addAndMakeVisible(reverbMixSlider);

    // ---- master EQ: fixed-band bass/mid/treble, the whole song's tone shape
    // (stored in the document) ----
    eqButton.onClick = [this]
    {
        const bool on = eqButton.getToggleState();
        history_.edit(on ? "Enable master EQ" : "Disable master EQ",
                      [on](model::Song& s) { s.eq.enabled = on; });
        engine_.setMasterEqEnabled(on);
        eqCurveView_.setSettings(history_.current().eq);
    };
    masterPanel_.addAndMakeVisible(eqButton);

    eqBassSlider.setRange(-18.0, 18.0, 0.1);
    eqBassSlider.setValue(0.0, juce::dontSendNotification);
    eqBassSlider.setTextValueSuffix(" dB bass");
    eqBassSlider.onValueChange = [this]
    {
        const float v = (float) eqBassSlider.getValue();
        history_.mutableCurrent().eq.bassDb = v;
        engine_.setMasterEqBassDb(v);
        eqCurveView_.setSettings(history_.current().eq);
    };
    wireUndoableSlider(eqBassSlider, "Set master EQ bass",
                       [](const model::Song& s) { return s.eq.bassDb; },
                       [](model::Song& s, float v) { s.eq.bassDb = v; });
    masterPanel_.addAndMakeVisible(eqBassSlider);

    eqMidSlider.setRange(-18.0, 18.0, 0.1);
    eqMidSlider.setValue(0.0, juce::dontSendNotification);
    eqMidSlider.setTextValueSuffix(" dB mid");
    eqMidSlider.onValueChange = [this]
    {
        const float v = (float) eqMidSlider.getValue();
        history_.mutableCurrent().eq.midDb = v;
        engine_.setMasterEqMidDb(v);
        eqCurveView_.setSettings(history_.current().eq);
    };
    wireUndoableSlider(eqMidSlider, "Set master EQ mid",
                       [](const model::Song& s) { return s.eq.midDb; },
                       [](model::Song& s, float v) { s.eq.midDb = v; });
    masterPanel_.addAndMakeVisible(eqMidSlider);

    eqTrebleSlider.setRange(-18.0, 18.0, 0.1);
    eqTrebleSlider.setValue(0.0, juce::dontSendNotification);
    eqTrebleSlider.setTextValueSuffix(" dB treble");
    eqTrebleSlider.onValueChange = [this]
    {
        const float v = (float) eqTrebleSlider.getValue();
        history_.mutableCurrent().eq.trebleDb = v;
        engine_.setMasterEqTrebleDb(v);
        eqCurveView_.setSettings(history_.current().eq);
    };
    wireUndoableSlider(eqTrebleSlider, "Set master EQ treble",
                       [](const model::Song& s) { return s.eq.trebleDb; },
                       [](model::Song& s, float v) { s.eq.trebleDb = v; });
    masterPanel_.addAndMakeVisible(eqTrebleSlider);
    masterPanel_.addAndMakeVisible(eqCurveView_);

    // ---- send bus: a shared reverb-or-delay every track can send into (stored in the document) ----
    sendBusButton.onClick = [this]
    {
        const bool on = sendBusButton.getToggleState();
        history_.edit(on ? "Enable send bus" : "Disable send bus",
                      [on](model::Song& s) { s.sendBus.enabled = on; });
        engine_.setSendBusEnabled(on);
    };
    masterPanel_.addAndMakeVisible(sendBusButton);

    sendEffectTypeBox_.addItem("Reverb", 1);
    sendEffectTypeBox_.addItem("Delay", 2);
    sendEffectTypeBox_.setSelectedId(1, juce::dontSendNotification);
    sendEffectTypeBox_.onChange = [this]
    {
        const auto type = sendEffectTypeBox_.getSelectedId() == 2 ? model::SendBusEffectType::Delay
                                                                  : model::SendBusEffectType::Reverb;
        history_.edit("Set send bus type", [type](model::Song& s) { s.sendBus.effectType = type; });
        engine_.setSendBusEffectType((int) type);
        updateSendBusEffectVisibility();
    };
    masterPanel_.addAndMakeVisible(sendEffectTypeBox_);

    sendRoomSlider.setRange(0.0, 100.0, 1.0);
    sendRoomSlider.setValue(60.0, juce::dontSendNotification);
    sendRoomSlider.setTextValueSuffix(" room");
    sendRoomSlider.onValueChange = [this]
    {
        const float v = (float) (sendRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.roomSize = v;
        engine_.setSendBusRoomSize(v);
    };
    wireUndoableSlider(sendRoomSlider, "Set send bus room size",
                       [](const model::Song& s) { return s.sendBus.roomSize; },
                       [](model::Song& s, float v) { s.sendBus.roomSize = v; });
    masterPanel_.addAndMakeVisible(sendRoomSlider);

    sendDampSlider.setRange(0.0, 100.0, 1.0);
    sendDampSlider.setValue(40.0, juce::dontSendNotification);
    sendDampSlider.setTextValueSuffix(" damp");
    sendDampSlider.onValueChange = [this]
    {
        const float v = (float) (sendDampSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.damping = v;
        engine_.setSendBusDamping(v);
    };
    wireUndoableSlider(sendDampSlider, "Set send bus damping",
                       [](const model::Song& s) { return s.sendBus.damping; },
                       [](model::Song& s, float v) { s.sendBus.damping = v; });
    masterPanel_.addAndMakeVisible(sendDampSlider);

    sendDelayTimeSlider.setRange(20.0, 1000.0, 1.0);
    sendDelayTimeSlider.setValue(300.0, juce::dontSendNotification);
    sendDelayTimeSlider.setTextValueSuffix(" ms");
    sendDelayTimeSlider.onValueChange = [this]
    {
        const float ms = (float) sendDelayTimeSlider.getValue();
        history_.mutableCurrent().sendBus.delayTimeMs = ms;
        engine_.setSendBusDelayTimeMs(ms);
    };
    wireUndoableSlider(sendDelayTimeSlider, "Set send bus delay time",
                       [](const model::Song& s) { return s.sendBus.delayTimeMs; },
                       [](model::Song& s, float v) { s.sendBus.delayTimeMs = v; });
    masterPanel_.addAndMakeVisible(sendDelayTimeSlider);

    sendDelayFbSlider.setRange(0.0, 95.0, 1.0);
    sendDelayFbSlider.setValue(35.0, juce::dontSendNotification);
    sendDelayFbSlider.setTextValueSuffix(" %");
    sendDelayFbSlider.onValueChange = [this]
    {
        const float fb = (float) (sendDelayFbSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.delayFeedback = fb;
        engine_.setSendBusDelayFeedback(fb);
    };
    wireUndoableSlider(sendDelayFbSlider, "Set send bus delay feedback",
                       [](const model::Song& s) { return s.sendBus.delayFeedback; },
                       [](model::Song& s, float v) { s.sendBus.delayFeedback = v; });
    masterPanel_.addAndMakeVisible(sendDelayFbSlider);

    sendReturnSlider.setRange(0.0, 100.0, 1.0);
    sendReturnSlider.setValue(50.0, juce::dontSendNotification);
    sendReturnSlider.setTextValueSuffix(" ret");
    sendReturnSlider.onValueChange = [this]
    {
        const float v = (float) (sendReturnSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.returnLevel = v;
        engine_.setSendBusReturnLevel(v);
    };
    wireUndoableSlider(sendReturnSlider, "Set send bus return level",
                       [](const model::Song& s) { return s.sendBus.returnLevel; },
                       [](model::Song& s, float v) { s.sendBus.returnLevel = v; });
    masterPanel_.addAndMakeVisible(sendReturnSlider);

    // ---- gain automation: arm, then move the master fader or a track's fader
    // while playing (Rec Auto arms both; Clr Auto clears both, the master lane
    // and the currently selected track's) ----
    autoRecButton.onClick   = [this] { recordAutomation_ = autoRecButton.getToggleState(); };
    autoClearButton.onClick = [this]
    {
        auto& song = history_.mutableCurrent();
        song.masterGainDb.clear();
        if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
            song.tracks[(size_t) selectedTrackIndex_].automation.clear(); // every parameter, not just gain
    };
    masterPanel_.addAndMakeVisible(autoRecButton);
    masterPanel_.addAndMakeVisible(autoClearButton);

    masterPanel_.addAndMakeVisible(meter_);

    // ---- per-track channel strips ----
    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip = new MixerStrip();
        strip->onGainChange = [this, i](float db) { setTrackGain(i, db); };
        strip->onFaderDragStart = [this, i](MixerStrip::Fader f) { beginFaderDrag(i, f); };
        strip->onFaderDragEnd   = [this, i](MixerStrip::Fader f) { endFaderDrag(i, f); };
        strip->onMuteChange = [this, i](bool m)   { setTrackMuted(i, m); };
        strip->onSoloChange = [this, i](bool s)   { setTrackSolo(i, s); };
        strip->onSendChange = [this, i](float lv) { setTrackSendLevel(i, lv); };
        strip->onPanChange  = [this, i](float p)  { setTrackPan(i, p); };
        strip->onSelect     = [this, i]           { selectTrack(i); };
        strip->onOutputBusChange = [this, i](int busId) { setTrackOutputBus(i, busId); };
        trackStrips_.add(strip);
        mixerView_.addAndMakeVisible(strip);
    }

    mixerView_.onResized = [this] { layoutMixerView(); };

    pianoRoll_.onChange = [this](const engine::Pattern& p) { editPattern(p); };
    pianoRoll_.onNotePreview = [this](int noteNumber) { previewNote(noteNumber); };
    pianoRoll_.onNotesDeleted = [this](int count)
    {
        // Delete in the keys pane always means notes, so it reports even when
        // nothing was selected — otherwise a user who expected the track to
        // go, or who forgot to select, gets silence and no idea which.
        if (count > 0)
            showStatus("Deleted " + juce::String(count) + (count == 1 ? " note" : " notes"));
        else
            showStatus("Select notes first - shift-click, or shift-drag a box");
    };

    // ---- edit tab: a header showing which track/clip is open, and the piano
    // roll (which switches to pad-per-row drum mode for a Drum track — see
    // refreshPianoRollForSelected). Editing the kit itself lives in the
    // Drums pane instead. ----
    editingLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    editTab_.addAndMakeVisible(editingLabel_);

    // Pattern length of the open clip, in bars — how long the content loops
    // over, as opposed to the clip's window on the timeline (which the
    // arrangement's resize handle sets). Deliberately separate controls: they
    // are separate concepts, and resizing the window shouldn't silently
    // re-loop the notes inside it.
    barsLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
    barsLabel_.setJustificationType(juce::Justification::centredRight);
    editTab_.addAndMakeVisible(barsLabel_);

    for (int bars : { 1, 2, 4 })
        barsBox_.addItem(juce::String(bars), bars);
    barsBox_.onChange = [this] { setPatternBars(barsBox_.getSelectedId()); };
    editTab_.addAndMakeVisible(barsBox_);

    // Not added to editTab_ directly: keysViewport_ takes it as its viewed
    // component below, and adding it here as well would reparent it straight
    // back out of the viewport.
    editTab_.onResized = [this] { layoutEditTab(); };

    // ---- drums pane: the kit's sounds on the left, its rhythm on the right ----
    drumsPane_.connectCallbacks();
    drumsPane_.onSampleAssigned = [this](int padIndex, const juce::File& file) { assignDrumSample(padIndex, file); };
    drumsPane_.onPadMixChanged  = [this](int padIndex, const model::DrumPad& pad) { setDrumPadMix(padIndex, pad); };
    drumsPane_.onPadAdded       = [this] { addDrumPad(); };
    drumsPane_.onPadRemoved     = [this](int padIndex) { removeDrumPad(padIndex); };
    drumsPane_.onPatternChanged = [this](const engine::Pattern& p) { editPattern(p); };
    drumsPane_.onNotePreview    = [this](int noteNumber) { previewNote(noteNumber); };
    drumsPane_.onKitStyleRequested = [this](engine::DrumKitStyle s) { applyDrumKitStyle(s); };

    // ---- arrange tab: a zoomable/scrollable timeline, click to seek ----
    arrangementViewport_.setViewedComponent(&arrangementView_, false);
    arrangeTab_.addAndMakeVisible(arrangementViewport_);

    // A magnifying glass, a slider and an editable multiplier. The icon says
    // what the control is without spending width on the word; the slider
    // makes the whole range reachable in one gesture; the box shows the exact
    // figure and takes one typed in. Both panes get the same control from one
    // definition — two copies of this would be two things to keep in step.
    setUpZoomControls(arrangeTab_, zoomIcon_, zoomSlider_, zoomBox_,
                      ArrangementView::kMinZoom, ArrangementView::kMaxZoom,
                      withShortcut("Timeline zoom", keys::zoomIn),
                      [this](float zoom) { setTimelineZoom(zoom); });

    setUpZoomControls(editTab_, keysZoomIcon_, keysZoomSlider_, keysZoomBox_,
                      PianoRoll::kMinPitchZoom, PianoRoll::kMaxPitchZoom,
                      "Pitch zoom - how many notes the grid shows (cmd-scroll)",
                      [this](float zoom) { setKeysZoom(zoom); });

    setUpZoomControls(editTab_, keysTimeZoomIcon_, keysTimeZoomSlider_, keysTimeZoomBox_,
                      PianoRoll::kMinTimeZoom, PianoRoll::kMaxTimeZoom,
                      "Time zoom - how wide each step is (shift-scroll)",
                      [this](float zoom) { setKeysTimeZoom(zoom); });

    // The roll scrolls horizontally once it's wider than its pane, exactly as
    // the arrangement does.
    keysViewport_.setViewedComponent(&pianoRoll_, false);
    keysViewport_.setScrollBarsShown(false, true); // horizontal only: rows fill the height
    editTab_.addAndMakeVisible(keysViewport_);

    pianoRoll_.onTimeZoomChanged = [this] { updateKeysTimeZoomControls(); layoutEditTab(); };

    // On by default: a scrolled grid that doesn't follow playback means the
    // playhead simply leaves the screen. Off is for editing one bar while the
    // rest of the pattern plays, where the view jumping is the annoyance.
    keysFollowButton_.setButtonText("Follow");
    keysFollowButton_.setTooltip("Scroll the grid to keep the playhead in view");
    keysFollowButton_.setToggleState(true, juce::dontSendNotification);
    editTab_.addAndMakeVisible(keysFollowButton_);

    // The wheel zooms the roll too, so the control follows it rather than
    // drifting from what's on screen.
    pianoRoll_.onPitchZoomChanged = [this] { updateKeysZoomControls(); };
    addClipButton_.onClick = [this] { addClipToSelectedTrack(); };
    generateLoopButton_.onClick = [this] { showGenerateLoopDialog(); };

    arrangeTab_.addAndMakeVisible(addClipButton_);
    arrangeTab_.addAndMakeVisible(generateLoopButton_);
    arrangeTab_.onResized = [this] { layoutArrangeTab(); };

    arrangementView_.onSeek = [this](double beat)
    {
        const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
        uiTempoMap_.setSampleRate(sampleRate);
        post(Cmd::Seek, (double) uiTempoMap_.samplesFromPpq(juce::jmax(0.0, beat)));
    };

    // Muting from the tracks pane goes through the same setTrackMuted the
    // mixer strip uses, so the two views can't disagree about a track's state.
    arrangementView_.onTrackMuteToggled = [this](int trackIndex)
    {
        const auto& tracks = history_.current().tracks;
        if (trackIndex < 0 || trackIndex >= (int) tracks.size())
            return;

        // Both read before the change. setTrackMuted is an undoable edit now,
        // and committing one move-assigns the document — which leaves any
        // reference into the old one dangling. Copying the name out first is
        // what keeps this from reading freed memory a line later.
        const bool nowMuted = ! tracks[(size_t) trackIndex].muted;
        const auto name     = juce::String(tracks[(size_t) trackIndex].name);

        setTrackMuted(trackIndex, nowMuted);

        showStatus((nowMuted ? "Muted " : "Unmuted ") + (name.isEmpty()
                       ? "track " + juce::String(trackIndex + 1) : "\"" + name + "\""));
    };

    arrangementView_.onTrackSettingsRequested = [this](int trackIndex)
    {
        showTrackSettingsMenu(trackIndex);
    };

    arrangementView_.onTrackDuplicateRequested = [this](int trackIndex)
    {
        duplicateTrackAt(trackIndex);
    };

    arrangementView_.onClipSelected = [this](int trackIndex, int clipIndex)
    {
        selectTrackAndClip(trackIndex, clipIndex);
    };

    arrangementView_.onClipMoved = [this](int trackIndex, int clipIndex, double newStartBeats)
    {
        history_.edit("Move clip", [trackIndex, clipIndex, newStartBeats](model::Song& s)
        {
            if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
                return;
            auto& clips = s.tracks[(size_t) trackIndex].clips;
            if (clipIndex >= 0 && clipIndex < (int) clips.size())
                clips[(size_t) clipIndex].startBeats = juce::jmax(0.0, newStartBeats);
        });

        arrangementView_.setSong(history_.current());
        syncEngineTracks(); // pushes every track's whole clip list, including this move
    };

    arrangementView_.onClipMovedToTrack = [this](int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats)
    {
        moveClipToTrack(srcTrackIndex, clipIndex, destTrackIndex, newStartBeats);
    };

    synthEditor_.onSettingsChanged   = [this](const model::SynthSettings& s) { setTrackSynthSettings(s); };
    synthEditor_.onSettingsDragStart = [this] { beginSynthSettingsDrag(); };
    synthEditor_.onSettingsDragEnd   = [this] { endSynthSettingsDrag(); };
    synthEditor_.onPresetSelected        = [this](int i) { applyPreset(i); };
    synthEditor_.onSavePresetRequested   = [this] { savePresetDialog(); };
    synthEditor_.onDeletePresetRequested = [this](int i) { deletePresetAt(i); };
    synthEditor_.onSynthToneRequested    = [this](engine::SynthTone t) { applySynthTone(t); };
    sessionView_.onLaunchClip  = [this](int track, int scene)
    {
        engine_.launchSessionSlot(track, scene);
        if (! engine_.isPlaying())
            post(Cmd::SetPlaying, 1.0); // launching implies you want to hear it
    };
    sessionView_.onLaunchScene = [this](int scene)
    {
        engine_.launchScene(scene);
        if (! engine_.isPlaying())
            post(Cmd::SetPlaying, 1.0);
    };
    sessionView_.onStopTrack   = [this](int track) { engine_.stopSessionSlot(track); };
    sessionView_.onStopAll     = [this] { engine_.stopAllSessionSlots(); };
    sessionView_.onAddScene    = [this] { addSessionScene(); };
    sessionView_.onDeleteScene = [this](int scene) { deleteSessionScene(scene); };
    sessionView_.onClipSelected = [this](int track, int scene) { captureClipIntoSession(track, scene); };

    // Clicking a fret sounds the note through the armed track, which for a
    // Guitar track is its GuitarNode — so the fretboard plays the same
    // instrument the sequencer does, including the one-note-per-string cut.
    fretboard_.onFretPlayed      = [this](int note) { previewNote(note); };
    fretboard_.onSettingsChanged   = [this](const model::GuitarSettings& s) { setTrackGuitarSettings(s); };
    fretboard_.onSettingsDragStart = [this] { beginGuitarSettingsDrag(); };
    fretboard_.onSettingsDragEnd   = [this] { endGuitarSettingsDrag(); };
    fretboard_.onChordStamped = [this](const engine::ChordShape& shape, int fretOffset,
                                       const engine::StrumSettings& strum)
    {
        stampChord(shape, fretOffset, strum);
    };
    fretboard_.onChordAtFret = [this](engine::MovableShape shape, int rootString, int fret,
                                      const engine::StrumSettings& strum, bool writeToClip)
    {
        playChordAtFret(shape, rootString, fret, strum, writeToClip);
    };
    fretboard_.onGuitarToneRequested = [this](engine::GuitarTone tone) { applyGuitarTone(tone); };

    audioEditor_.onGainChanged   = [this](float gainDb) { setSelectedClipGainDb(gainDb); };
    audioEditor_.onGainDragStart = [this] { beginClipGainDrag(); };
    audioEditor_.onGainDragEnd   = [this] { endClipGainDrag(); };
    audioEditor_.onNormaliseRequested = [this] { normaliseSelectedClip(); };
    // One timeline: the audio editor seeks and starts the *song* transport,
    // exactly as clicking the Tracks ruler does. A second, independent clock
    // for the editor meant two playheads that disagreed about "now".
    audioEditor_.onSeekRequested = [this](double secondsIntoFile)
    {
        seekToBeat(songBeatForClipSeconds(secondsIntoFile));
    };
    audioEditor_.onPlayRequested = [this](double fromSeconds)
    {
        seekToBeat(songBeatForClipSeconds(fromSeconds));
        post(Cmd::SetPlaying, 1.0);
    };
    audioEditor_.onStopRequested = [this] { post(Cmd::SetPlaying, 0.0); };
    audioEditor_.onFilesDropped = [this](const juce::Array<juce::File>& files)
    {
        // Each on its own new track, at the start. importAudioFileAtBeat
        // selects what it creates, so the last one dropped is the one left
        // open in the editor — which is where the file was dropped, and so
        // where it's expected to appear.
        for (const auto& file : files)
            importAudioFileAtBeat(file, 0.0, -1);
    };
    audioEditor_.onCutRequested     = [this] { cutAudioSelection(); };
    audioEditor_.onCopyRequested    = [this] { copyAudioSelection(); };
    audioEditor_.onPasteRequested   = [this] { pasteAudioAtSelection(); };
    audioEditor_.onDeleteRequested  = [this] { deleteAudioSelection(); };
    audioEditor_.onTrimRequested    = [this] { trimToAudioSelection(); };
    audioEditor_.onSplitRequested   = [this] { splitClipAtSelection(); };
    audioEditor_.onSilenceRequested = [this] { silenceAudioSelection(); };
    audioEditor_.onFadeInRequested  = [this] { fadeInAudioSelection(); };
    audioEditor_.onFadeOutRequested = [this] { fadeOutAudioSelection(); };
    audioEditor_.onReverseRequested = [this] { reverseAudioSelection(); };
    audioEditor_.onApplyEffectsRequested = [this] { showApplyEffectsDialog(); };
    audioEditor_.onSpeedPitchRequested   = [this] { showSpeedPitchDialog(); };
    analyserPane_.onAnalyseRequested     = [this] { analyseSelection(); };

    automationPane_.onLaneEdited = [this](model::TrackParam param, const model::AutomationLane& lane)
    {
        applyEditedAutomationLane(param, lane);
    };
    // The parameter picker changing means a different lane entirely, so the
    // pane has to be handed the new one rather than keeping the old one on
    // screen under a new name.
    automationPane_.onParamChanged = [this](model::TrackParam) { refreshAutomationPaneForSelected(); };
    audioEditor_.onCaptureNoisePrintRequested = [this] { captureNoisePrint(); };
    audioEditor_.onReduceNoiseRequested = [this](float amountDb, float floorDb)
    {
        reduceNoiseOnSelectedClip(amountDb, floorDb);
    };

    masteringPane_.onSettingsChanged   = [this](const model::MasteringSettings& s) { setMasteringSettings(s); };
    masteringPane_.onSettingsDragStart = [this] { beginMasteringDrag(); };
    masteringPane_.onSettingsDragEnd   = [this] { endMasteringDrag(); };
    masteringPane_.onPresetRequested   = [this](engine::MasteringPreset p) { applyMasteringPreset(p); };
    masteringPane_.onFilesDropped      = [this](const juce::Array<juce::File>& files)
    {
        // Each on its own new track (index -1), at the start: arriving via
        // the mastering pane means "get this into the project", and there's
        // no drop position to honour the way the timeline has one.
        for (const auto& file : files)
            importAudioFileAtBeat(file, 0.0, -1);

        showStatus(files.size() == 1 ? "Imported " + files[0].getFileName()
                                     : "Imported " + juce::String(files.size()) + " files");
    };
    // The selection is read back off the pane when an action fires, so
    // nothing needs storing here — but a repaint keeps the arrangement's
    // idea of the selected clip honest if it ever draws one.
    audioEditor_.onSelectionChanged = [](AudioRange) {};

    effectChain_.onBuiltInAdded = [this](model::EffectKind kind) { addEffectSlot(kind, {}); };
    effectChain_.onPluginAdded  = [this](const engine::PluginEntry& entry)
    {
        model::PluginRef ref;
        ref.format     = entry.format == "VST3" ? model::PluginFormat::VST3
                       : entry.format == "AudioUnit" ? model::PluginFormat::AudioUnit
                                                     : model::PluginFormat::Unknown;
        ref.identifier = entry.identifier;
        ref.name       = entry.name;
        addEffectSlot(model::EffectKind::Plugin, ref);
    };
    effectChain_.onSlotRemoved          = [this](int slot) { removeEffectSlot(slot); };
    effectChain_.onSlotMoved            = [this](int slot, int delta) { moveEffectSlot(slot, delta); };
    effectChain_.onSlotBypassToggled    = [this](int slot, bool on) { setEffectSlotBypass(slot, on); };
    effectChain_.onSlotParamsChanged    = [this](const model::EffectSlot& s, int i) { setEffectSlotParams(s, i); };
    effectChain_.onSlotParamsDragStart  = [this](int i) { beginEffectSlotParamsDrag(i); };
    effectChain_.onSlotParamsDragEnd    = [this](int i) { endEffectSlotParamsDrag(i); };
    effectChain_.onScanRequested        = [this] { scanForPlugins(); };

    // A previous scan, so launching doesn't re-probe every plugin on the
    // machine — probing instantiates each one and is slow.
    engine_.pluginHost().restoreScanCache(settings_.getValue("pluginScanCache").toStdString());
    effectChain_.setAvailablePlugins(engine_.pluginHost().knownPlugins());
    effectChain_.onPluginEditorRequested = [this](int slot) { openPluginEditor(slot); };

    seedFactoryPresets();
    refreshPresetList();

    workspace_.registerPanel("Files", fileBrowser_);
    workspace_.registerPanel("Transport", leftPane_);
    workspace_.registerPanel("Tracks", arrangeTab_);
    workspace_.registerPanel("Keys", editTab_);
    workspace_.registerPanel("Synth", synthEditor_);
    workspace_.registerPanel("Drums", drumsPane_);
    workspace_.registerPanel("Guitar", fretboard_);
    workspace_.registerPanel("Audio", audioEditor_);
    workspace_.registerPanel("Mastering", masteringPane_);
    workspace_.registerPanel("Analyser", analyserPane_);
    workspace_.registerPanel("Automation", automationPane_);
    workspace_.registerPanel("Session", sessionView_);
    workspace_.registerPanel("Track FX", effectChain_);
    workspace_.registerPanel("Mixer", mixerView_);
    workspace_.registerPanel("Master", masterPanel_);
    workspace_.registerPanel("Keyboard", keyboard_);
    loadDockLayout(); // last session's arrangement, or the default one

    fileBrowser_.setRecordingsDirectory(recordingsDirectory());
    fileBrowser_.showDirectory(recordingsDirectory());
    // Double-clicking a file imports it rather than "previewing" it. The
    // preview loaded the file into the transport-slaved player, which only
    // sounds while the song is already rolling — so double-clicking a file
    // set a label and produced silence, which reads as the Files pane doing
    // nothing. Importing is visible, undoable, and lands the file where it
    // can then be auditioned.
    fileBrowser_.onFilePreview = [this](const juce::File& file)
    {
        if (audiofiles::isImportableAudioFile(file))
            importAudioFileAtBeat(file, 0.0, -1);
        else
            previewAudioFile(file);
    };

    {
        std::vector<juce::File> bookmarks;
        for (const auto& line : juce::StringArray::fromLines(settings_.getValue("fileBrowserBookmarks")))
            if (line.isNotEmpty())
                bookmarks.push_back(juce::File(line));
        fileBrowser_.setBookmarks(bookmarks);
    }
    fileBrowser_.onBookmarksChanged = [this]
    {
        juce::StringArray lines;
        for (const auto& dir : fileBrowser_.bookmarks())
            lines.add(dir.getFullPathName());
        settings_.setValue("fileBrowserBookmarks", lines.joinIntoString("\n"));
        settings_.saveIfNeeded();
    };

    {
        std::vector<juce::File> favorites;
        for (const auto& line : juce::StringArray::fromLines(settings_.getValue("fileBrowserFavorites")))
            if (line.isNotEmpty())
                favorites.push_back(juce::File(line));
        fileBrowser_.setFavorites(favorites);
    }
    fileBrowser_.onFavoritesChanged = [this]
    {
        juce::StringArray lines;
        for (const auto& f : fileBrowser_.favorites())
            lines.add(f.getFullPathName());
        settings_.setValue("fileBrowserFavorites", lines.joinIntoString("\n"));
        settings_.saveIfNeeded();
    };

    arrangementView_.onClipResized = [this](int trackIndex, int clipIndex, double newLengthBeats)
    {
        setClipLength(trackIndex, clipIndex, newLengthBeats);
    };

    arrangementView_.onTempoChangeRequested = [this](double beat) { editTempoChangeAt(beat); };
    arrangementView_.onTempoChangeRemoved    = [this](double beat) { removeTempoChangeAt(beat); };
    arrangementView_.onTempoRampToggled      = [this](double beat) { toggleTempoRamp(beat); };
    arrangementView_.onTempoChangeMoved      = [this](double from, double to)
    {
        moveTempoChange(from, to);
    };

    arrangementView_.onFileDropped = [this](const juce::File& file, double dropBeat, int trackIndex)
    {
        importAudioFileAtBeat(file, dropBeat, trackIndex);
    };

    // Mirror the initial document into the engine + UI.
    syncEngineTracks();
    engine_.setArmedTrack(0);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();
    updateEqControls();
    updateMasteringControls();
    updateEditingLabel();
    updateSendBusControls();

    engine_.deviceManager().addChangeListener(this);
    logAudioDeviceStatus();

    addChildComponent(status_);
    updateZoomControls();     // the readouts must say something before the first click
    updateKeysZoomControls();
    updateKeysTimeZoomControls();

    // The document the app opens with counts as saved, so an untouched session
    // doesn't prompt on quit. This has to come *after* all the control setup
    // above: several of those updateXxxControls calls write through
    // mutableCurrent(), which advances the state id by design, so a marker
    // taken any earlier is stale by the time construction finishes.
    savedStateId_ = history_.stateId();

    // Still worth having in a debug build: it fires at launch rather than
    // when someone presses the key. The tests are what cover the release
    // build — see tests/gui/ShortcutsTests.cpp.
    for ([[maybe_unused]] const auto& shortcut : keys::all())
        jassert(shortcut.key.isValid());

    setWantsKeyboardFocus(true);
    setSize(900, 800);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    // Before anything else: a render thread is inside the engine, and the
    // engine is about to go away.
    if (renderJob_ != nullptr)
    {
        renderJob_->signalThreadShouldExit();
        renderJob_->stopThread(5000);
        renderJob_.reset();
    }

    saveDockLayout();
    stopTimer();
    menuBar_.setModel(nullptr);
    engine_.deviceManager().removeChangeListener(this);
}

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
        menu.addItem(31, "Repair Recorded Clip Lengths...");
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

            // Warping, unlike everything above it, acts on the whole clip
            // rather than a selection — and is non-destructive, which is why
            // it is a tick rather than an action that rewrites samples the way
            // "Speed and pitch" does.
            menu.addSeparator();

            const auto* audioClip = selectedAudioClip();
            const bool  knowsTempo = audioClip != nullptr && audioClip->sourceBpm > 0.0;

            menu.addItem(60, knowsTempo
                                 ? "Warp Clip to Project Tempo   (clip is "
                                       + juce::String(audioClip->sourceBpm, 1) + " BPM)"
                                 : juce::String("Warp Clip to Project Tempo"),
                         knowsTempo, audioClip != nullptr && audioClip->warpEnabled);
            menu.addItem(61, "Detect Clip Tempo...", hasAudio, false);
            menu.addItem(62, "Set Project Tempo from Clip", knowsTempo, false);
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
        case 31: repairRecordedClipLengths(); break;
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

        case 60: toggleClipWarp(); break;
        case 61: detectSelectedClipTempo(); break;
        case 62: setProjectTempoFromClip(); break;

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

/** Discards the current document for an empty one, asking first. */
void MainComponent::newProject()
{
    confirmDiscardChanges([this] { createEmptyProject(); });
}

void MainComponent::createEmptyProject()
{
    model::Song song;
    const int id = model::addTrack(song, model::TrackType::Instrument, "Synth 1").id;
    model::Clip clip;
    clip.type                = model::ClipType::Instrument;
    clip.lengthBeats         = 4.0;
    clip.pattern.lengthBeats = 4.0;
    model::addClip(song, id, clip);

    history_.reset(song);
    selectedTrackIndex_ = 0;
    tempoSlider.setValue(song.bpm, juce::dontSendNotification);

    // The whole map, not just the starting tempo: a project that carries tempo
    // changes has to arrive in the engine with them, or it plays back at one
    // tempo while the document says otherwise.
    const auto tempoMap = model::tempoMapFor(song);
    uiTempoMap_.setTempoChanges(tempoMap);
    engine_.setTempoChanges(tempoMap);
    post(Cmd::SetTempo, song.bpm);
    refreshFromModel();
    clipLabel.setText("No clip loaded", juce::dontSendNotification);

    // A brand-new project has nothing worth saving yet, so it starts clean —
    // quitting straight after New shouldn't ask about it.
    projectFile_  = juce::File{};
    savedStateId_ = history_.stateId();
    updateWindowTitle();
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        engine_.deviceManager(), 0, 0, 0, 2, false, false, false, false);
    selector->setSize(500, 420);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle                  = "Audio Settings";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

void MainComponent::post(engine::EngineCommand::Type type, double a, double b)
{
    engine_.postCommand({ type, a, b });
}

int MainComponent::trackCount() const
{
    return (int) history_.current().tracks.size();
}

const engine::Pattern& MainComponent::currentPattern() const
{
    static const engine::Pattern empty;
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return empty;
    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size())
        return empty;
    return track.clips[(size_t) selectedClipIndex_].pattern;
}

void MainComponent::editPattern(const engine::Pattern& pattern)
{
    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;
    history_.edit("Edit notes", [&pattern, trackIdx, clipIdx](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx >= 0 && clipIdx < (int) clips.size())
            clips[(size_t) clipIdx].pattern = pattern;
    });
    syncEngineTracks(); // rebuilds every track's clip list, including this edit
}

void MainComponent::addTrack()
{
    if (trackCount() >= engine_.maxTracks())
        return;

    history_.edit("Add track", [](model::Song& s)
    {
        const auto name = "Synth " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Instrument, name.toStdString()).id;
        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        model::addClip(s, id, clip);
    });

    selectedTrackIndex_ = trackCount() - 1;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Same as addTrack(), but a Drum-type track (model::addTrack auto-populates
    its default Kick/Snare/Hat/Other pads — see model::makeDefaultDrumKit). */
void MainComponent::addDrumTrack()
{
    if (trackCount() >= engine_.maxTracks())
        return;

    // Real sounds by default rather than four silent pads — the same reason
    // makeStarterSong() does this for the track a fresh launch begins with.
    // The pattern stays empty, unlike the startup track's: this is a track
    // the user is deliberately adding, so it gets a blank canvas to program
    // rather than a copy of the demo loop.
    const auto kit = defaultDrumKitWithFactorySamples();
    history_.edit("Add drum track", [kit](model::Song& s)
    {
        const auto name = "Drums " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Drum, name.toStdString()).id;
        s.tracks.back().drumKit = kit;
        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        model::addClip(s, id, clip);
    });

    selectedTrackIndex_ = trackCount() - 1;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Writes a strummed chord into the open clip at the playhead.

    Real notes at real times, not a "strum" flag: the stagger between strings
    is most of what makes a chord sound like a hand rather than an organ, and
    putting it in the pattern keeps it visible and editable afterwards — the
    same choice §18's swing made, for the same reason. */
/** The selected track, if chords can go on it. Reports why not otherwise:
    every one of these used to be a silent return, which is indistinguishable
    from a broken button. */
const model::Track* MainComponent::guitarTrackForChords()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        showError("Select a guitar track first");
        return nullptr;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (track.type != model::TrackType::Guitar)
    {
        showError("\"" + juce::String(track.name) + "\" isn't a guitar track — chords need one");
        return nullptr;
    }

    return &track;
}

/** Bar length in beats, as the chord features measure it. */
double MainComponent::beatsPerBar() const
{
    return juce::jmax(1.0, uiTempoMap_.quartersPerBar());
}

/** Writes already-built notes into the selected guitar clip, at the bar the
    playhead is in. @p notes are positioned relative to the start of that bar,
    so callers don't need to know where it lands.

    Shared by the open-shape palette and by clicking the neck: those differ in
    which notes they produce, not in where the notes go or how that's
    reported. */
/** Adds already-placed notes to the selected clip and reports it.

    The notes arrive carrying their final positions — planChordStamp works out
    where the bar is, and this only commits. Splitting it that way is what
    makes the placement testable. */
bool MainComponent::commitStampedNotes(const std::vector<engine::Note>& notes,
                                       const juce::String& what, double atBeats)
{
    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;

    int added = 0;
    history_.edit("Add chord", [trackIdx, clipIdx, &notes, &added](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;

        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& pattern = clips[(size_t) clipIdx].pattern;
        for (const auto& note : notes)
        {
            pattern.notes.push_back(note);
            ++added;
        }
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();

    // Stamping writes notes into the clip; unless the transport happens to be
    // rolling over that bar, nothing moves and nothing sounds. Say what landed
    // and where, or a chord that worked looks exactly like one that didn't.
    if (added > 0)
        showStatus(what + " written at beat " + juce::String(atBeats + 1.0, 2));
    else
        showError("Couldn't write " + what + " — the clip went away");

    return added > 0;
}

/** Stamps one of the open shapes from the chord palette. */
/** Sounds a chord through the selected track's own instrument, struck the way
    it will be written.

    Takes the strummed notes rather than a set of pitches so the preview is
    the same thing that lands in the clip, stagger included. Six notes at one
    instant read as an organ, and the stagger is most of what makes a strum
    sound like a hand — a preview without it would undersell the very control
    the user is reaching for.

    Shared by the palette and by clicking the neck. They had drifted: the neck
    played what it wrote and the palette wrote silently, so with the transport
    stopped the palette buttons looked like they did nothing at all. */
void MainComponent::previewChord(const std::vector<engine::Note>& notes)
{
    const double msPerBeat = 60000.0 / juce::jmax(1.0, history_.current().bpm);

    for (const auto& note : notes)
    {
        const int delayMs = (int) std::lround(juce::jmax(0.0, note.startBeats) * msPerBeat);

        if (delayMs <= 0)
        {
            previewNote(note.noteNumber);
            continue;
        }

        // SafePointer for the same reason previewNote uses one: this fires
        // after the click, and closing the window in between would otherwise
        // run it against a destroyed engine.
        juce::Component::SafePointer<MainComponent> safeThis(this);
        const int noteNumber = note.noteNumber;

        juce::Timer::callAfterDelay(delayMs, [safeThis, noteNumber]
        {
            if (auto* self = safeThis.getComponent())
                self->previewNote(noteNumber);
        });
    }
}

void MainComponent::stampChord(const engine::ChordShape& shape, int fretOffset,
                               const engine::StrumSettings& strum)
{
    // Every guard and the placement arithmetic live in planChordStamp, which
    // is JUCE-free and tested. They were inside this function, where nothing
    // could reach them — which is most of why the reported "the chord buttons
    // don't do anything" took so long to place.
    const auto plan = planChordStamp(history_.current(), selectedTrackIndex_, selectedClipIndex_,
                                     shape, fretOffset, strum,
                                     uiTempoMap_.ppqFromSamples(engine_.playheadSamples()),
                                     beatsPerBar(), chordStampSeed_++ | 1u);

    if (! plan.ok)
    {
        showError(juce::String(plan.problem));
        return;
    }

    // Played as well as written. Stamping puts notes in the clip, which makes
    // no sound unless the transport happens to be rolling over that bar — so
    // without this a button that worked was indistinguishable from one that
    // didn't.
    previewChord(plan.notes);
    commitStampedNotes(plan.notes, juce::String(shape.name) + " chord",
                       plan.atBeats);
}

/** A click on the neck with a chord mode selected: the shape rooted there is
    played, and written into the clip as well if the Write toggle is on.

    Playing is the default because the ask was to *play* chords by clicking the
    neck — a click that silently edited the document instead would be a
    surprising thing for a fretboard to do. Writing is one explicit toggle
    rather than a modifier key, so nothing about it is hidden. */
void MainComponent::playChordAtFret(engine::MovableShape shape, int rootString, int fret,
                                    const engine::StrumSettings& strum, bool writeToClip)
{
    const auto* track = guitarTrackForChords();
    if (track == nullptr)
        return;

    const auto notes = engine::GuitarChords::notesForRoot(shape, track->guitarSettings.tuning.data(),
                                                          rootString, fret);
    if (notes.empty())
    {
        showError(juce::String(engine::movableShapeName(shape)) + " doesn't fit there on the neck");
        return;
    }

    const auto struck = engine::GuitarChords::strumRootedChord(
                            shape, track->guitarSettings.tuning.data(), rootString, fret,
                            0.0, beatsPerBar(), history_.current().bpm, strum,
                            (uint32_t) (chordStampSeed_++ | 1u));

    // Sounded through the same preview path a single fret click uses, so the
    // chord is played by the track's own GuitarNode — including its
    // one-note-per-string cut, which is what stops a chord from sounding like
    // six unrelated strings.
    previewChord(struck);

    const juce::String what = juce::String(engine::movableShapeName(shape)) + " on "
                            + juce::String(engine::midiNoteName(notes.front()));

    if (! writeToClip)
    {
        showStatus(what);
        return;
    }

    commitStampedNotes(struck, what, 0.0);
}

/** Same as addTrack(), but a Guitar-type track — six plucked strings in
    standard tuning (see model::GuitarSettings). */
/** Adds a group bus: a track that receives other tracks' output rather than
    generating any (see model::TrackType::Bus). No clip is created for it —
    a bus has nothing to play, and an empty clip on one would show up in the
    arrangement as something you could open and edit. */
void MainComponent::addBusTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    history_.edit("Add group bus", [](model::Song& s)
    {
        const auto name = "Bus " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Bus, name.toStdString());
    });

    selectedTrackIndex_ = trackCount() - 1;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    updateMixerStrips();
    updateEditingLabel();
    showStatus("Added a group bus - route tracks into it from their mixer strip");
}

void MainComponent::addGuitarTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    history_.edit("Add guitar track", [](model::Song& s)
    {
        const auto name = "Guitar " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Guitar, name.toStdString()).id;
        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        model::addClip(s, id, clip);
    });

    selectedTrackIndex_ = trackCount() - 1;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Shows the fretboard for the selected track, or a placeholder if it isn't a
    Guitar track — the same gating the Synth and Drums panes use. */
void MainComponent::refreshFretboardForSelected()
{
    const bool isGuitar = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                        && history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Guitar;

    if (isGuitar)
    {
        const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
        fretboard_.setSettings(track.guitarSettings);
        fretboard_.setTrackInfo(track.name, track.colour);
    }
    else
        fretboard_.setNoGuitarTrackSelected();
}

/** Live tweak from the fretboard — document in place, then the engine, same
    as the mixer faders and the Synth pane. Tuning goes through the same path,
    since retuning a string is just another parameter to the model. */
void MainComponent::setTrackGuitarSettings(const model::GuitarSettings& settings)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.mutableCurrent().tracks[(size_t) index].guitarSettings = settings;

    engine_.setTrackGuitarSettings(index, settings);
    engine_.setTrackGuitarTuning(index, settings.tuning);
}

/** Assigns @p file to pad @p padIndex of the currently selected track's drum
    kit (called from the drum-kit editor's Load... button or a file dropped
    onto one of its rows). A real document edit, so it goes through history_
    like any other content change. */
void MainComponent::assignDrumSample(int padIndex, const juce::File& file)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int  trackIdx = selectedTrackIndex_;
    const auto path      = file.getFullPathName().toStdString();

    history_.edit("Assign drum sample", [trackIdx, padIndex, path](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& pads = s.tracks[(size_t) trackIdx].drumKit.pads;
        if (padIndex >= 0 && padIndex < (int) pads.size())
            pads[(size_t) padIndex].samplePath = path;
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected(); // redraws the pad's row with its new sample name
}

/** Adds a new clip to the currently selected track, positioned 2 beats after
    its last existing clip (or at beat 0 if it has none), and selects it for
    editing. */
void MainComponent::addClipToSelectedTrack()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int trackIdx = selectedTrackIndex_;
    int       newClipIndex = -1;

    history_.edit("Add clip", [trackIdx, &newClipIndex](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& track = s.tracks[(size_t) trackIdx];

        double nextStart = 0.0;
        for (const auto& c : track.clips)
            nextStart = juce::jmax(nextStart, c.startBeats + c.lengthBeats);
        if (! track.clips.empty())
            nextStart += 2.0; // a small gap after the last clip

        model::Clip clip;
        clip.id                  = model::allocateId(s);
        clip.type                = model::ClipType::Instrument;
        clip.startBeats          = nextStart;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        track.clips.push_back(clip);
        newClipIndex = (int) track.clips.size() - 1;
    });

    if (newClipIndex < 0)
        return;

    selectedClipIndex_ = newClipIndex;
    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Opens a small dialog and, on confirmation, adds a new algorithmically
    generated loop to the selected track (see engine/GenerativeLoop.h): a
    scale-constrained melody for Instrument/Guitar tracks, a Euclidean-rhythm
    drum pattern — using the track's own assigned pad notes, not the factory
    defaults — for Drum tracks. Audio tracks have no MIDI pattern to generate
    into, so this is unreachable for one (generateLoopButton_ is disabled;
    see timerCallback).

    No seed field: a fresh random seed is picked on every "Generate" click.
    Getting a different take is Undo + click again, the same candidate
    workflow "Add Clip" already gives for a blank clip — building a
    multi-candidate preview here would be real scope on its own for a first
    slice. */
void MainComponent::showGenerateLoopDialog()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
    if (track.type == model::TrackType::Audio)
        return;

    const bool isDrum = track.type == model::TrackType::Drum;

    auto* window = new juce::AlertWindow(isDrum ? "Generate Drum Loop" : "Generate Melodic Loop", {},
                                         juce::MessageBoxIconType::NoIcon, this);

    if (! isDrum)
    {
        juce::StringArray roots;
        for (const char* name : kGenerateLoopRootNoteNames)
            roots.add(name);
        window->addComboBox("root", roots, "Root note:");
        window->getComboBoxComponent("root")->setSelectedItemIndex(0);

        juce::StringArray scales;
        for (const char* name : kGenerateLoopScaleNames)
            scales.add(name);
        window->addComboBox("scale", scales, "Scale:");
        window->getComboBoxComponent("scale")->setSelectedItemIndex(0);

        // Melodic only: there's no such thing as harmonising a drum onset.
        // Defaulted to "Some" rather than "None" so a generated part has
        // some vertical interest without having to be asked for it.
        window->addComboBox("harmony", { "None", "Some", "Lots" }, "Harmony:");
        window->getComboBoxComponent("harmony")->setSelectedItemIndex(1);
    }

    window->addComboBox("density", { "Low", "Medium", "High" }, "Density:");
    window->getComboBoxComponent("density")->setSelectedItemIndex(1);

    juce::StringArray genres { "None (use Density)" };
    for (engine::Genre genre : kGenerateLoopGenres)
        genres.add(engine::genreName(genre));
    window->addComboBox("genre", genres,
                        isDrum ? "Genre (overrides Density):"
                               : "Genre (overrides Density; also sets the synth sound):");
    window->getComboBoxComponent("genre")->setSelectedItemIndex(0);

    window->addButton("Generate", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<MainComponent>(this), window,
             trackIdx = selectedTrackIndex_, isDrum](int result)
            {
                if (self == nullptr || result != 1)
                    return;

                static const double kDensities[] = { 0.2, 0.5, 0.8 };
                const int    densityIndex = window->getComboBoxComponent("density")->getSelectedItemIndex();
                const auto   seed         = (unsigned) juce::Random::getSystemRandom().nextInt();

                // Index 0 is "None": density comes from the Density combo and
                // swing/preset stay off, i.e. exactly today's behaviour. Any
                // other index selects a genre, whose rhythm profile replaces
                // the Density combo's value outright rather than blending
                // with it - simplest correct behaviour, and it's what the
                // combo's own label says it does.
                const int  genreIndex = window->getComboBoxComponent("genre")->getSelectedItemIndex();
                const bool hasGenre   = genreIndex > 0;
                const engine::Genre genre = hasGenre
                    ? kGenerateLoopGenres[(size_t) juce::jlimit(1, (int) std::size(kGenerateLoopGenres), genreIndex) - 1]
                    : engine::Genre::House; // unused when hasGenre is false

                double density = kDensities[(size_t) juce::jlimit(0, 2, densityIndex)];
                double swing   = 0.0;
                if (hasGenre)
                {
                    const auto profile = engine::rhythmProfileForGenre(genre);
                    density = profile.density;
                    swing   = profile.swing;
                }

                engine::Pattern generated;
                if (isDrum)
                {
                    const auto& s = self->history_.current();
                    if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
                        return;
                    const auto& pads = s.tracks[(size_t) trackIdx].drumKit.pads;

                    // Named pads win; a kit whose pads were reordered but not
                    // renamed falls back to the default Kick/Snare/Hat slots.
                    auto noteForLabel = [&](const char* label, size_t fallbackIndex, int fallbackNote)
                    {
                        for (const auto& pad : pads)
                            if (pad.label == label)
                                return pad.noteNumber;
                        return fallbackIndex < pads.size() ? pads[fallbackIndex].noteNumber : fallbackNote;
                    };

                    engine::DrumLoopParams params;
                    params.density   = density;
                    params.swing     = swing;
                    params.seed      = seed;
                    params.kickNote  = noteForLabel("Kick", 0, engine::kDefaultKickNote);
                    params.snareNote = noteForLabel("Snare", 1, engine::kDefaultSnareNote);
                    params.hatNote   = noteForLabel("Hat", 2, engine::kDefaultHatNote);
                    generated        = engine::generateDrumLoop(params);
                }
                else
                {
                    const int rootIndex  = window->getComboBoxComponent("root")->getSelectedItemIndex();
                    const int scaleIndex = window->getComboBoxComponent("scale")->getSelectedItemIndex();

                    static const double kHarmonies[] = { 0.0, 0.3, 0.7 };
                    const int harmonyIndex = window->getComboBoxComponent("harmony")->getSelectedItemIndex();

                    engine::MelodicLoopParams params;
                    params.density     = density;
                    params.swing       = swing;
                    params.harmony     = kHarmonies[(size_t) juce::jlimit(0, 2, harmonyIndex)];
                    params.seed        = seed;
                    params.scale.type  = kGenerateLoopScaleTypes[(size_t) juce::jlimit(
                                             0, (int) std::size(kGenerateLoopScaleTypes) - 1, scaleIndex)];
                    params.scale.rootNote = 60 + juce::jlimit(0, 11, rootIndex); // octave 4, this project's convention
                    generated          = engine::generateMelodicLoop(params);
                }

                // Computed once, outside the edit lambda: applying it is
                // free (a struct copy), so there's no reason to build it
                // twice or defer it into the lambda body.
                const model::SynthPreset genrePreset = hasGenre ? model::presetForGenre(genre) : model::SynthPreset {};

                int newClipIndex = -1;
                self->history_.edit(isDrum ? "Generate drum loop" : "Generate melodic loop",
                    [trackIdx, &newClipIndex, &generated, hasGenre, &genrePreset](model::Song& s)
                    {
                        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
                            return;
                        auto& targetTrack = s.tracks[(size_t) trackIdx];

                        double nextStart = 0.0;
                        for (const auto& c : targetTrack.clips)
                            nextStart = juce::jmax(nextStart, c.startBeats + c.lengthBeats);
                        if (! targetTrack.clips.empty())
                            nextStart += 2.0; // a small gap after the last clip, matching Add Clip

                        model::Clip clip;
                        clip.id          = model::allocateId(s);
                        clip.type        = model::ClipType::Instrument;
                        clip.startBeats  = nextStart;
                        clip.lengthBeats = generated.lengthBeats;
                        clip.pattern     = generated;
                        targetTrack.clips.push_back(clip);
                        newClipIndex = (int) targetTrack.clips.size() - 1;

                        // The genre's synth sound is Instrument-only: Guitar
                        // tracks are driven by GuitarSettings, not
                        // SynthSettings, at all (see §21), and Drum tracks
                        // have no synth voice to retune. One undo step covers
                        // both the new clip and this, matching how loading a
                        // saved preset is already "one thing, not two" (see
                        // applyPreset).
                        if (hasGenre && targetTrack.type == model::TrackType::Instrument)
                        {
                            targetTrack.synthSettings = genrePreset.synth;
                            targetTrack.effectChain   = genrePreset.effectChain;
                        }
                    });

                if (newClipIndex < 0)
                    return;

                self->selectedClipIndex_ = newClipIndex;
                self->syncEngineTracks();
                self->refreshPianoRollForSelected();
                self->refreshSynthEditorForSelected();
                self->refreshDrumsPaneForSelected();
                self->refreshEffectChainForSelected();
                self->refreshFretboardForSelected();
                self->refreshAudioEditorForSelected();
                self->refreshSessionView();
                self->arrangementView_.setSong(self->history_.current());
                self->arrangementView_.setSelectedClip(self->selectedTrackIndex_, self->selectedClipIndex_);
                self->updateEditingLabel();
            }),
        true);
}

/** Redraws the session grid from the document. Which cells are *playing* is
    pushed separately from the engine each timer tick — see timerCallback —
    because a launch stays pending until the next bar line and the grid would
    otherwise light the wrong cell. */
void MainComponent::refreshSessionView()
{
    sessionView_.setSong(history_.current());
}

/** Adds a scene (a grid row), giving every track an empty slot in it. */
/** Removes a session row and every clip in it. Stops playback first: the
    slots the engine is holding are addressed by index, and the row below
    would inherit the index of the one that just went away. */
void MainComponent::deleteSessionScene(int sceneIndex)
{
    const auto& song = history_.current();
    if (sceneIndex < 0 || sceneIndex >= (int) song.scenes.size())
        return;

    const auto name = song.scenes[(size_t) sceneIndex].name;

    engine_.stopAllSessionSlots();

    history_.edit("Delete scene", [sceneIndex](model::Song& s) { model::removeScene(s, sceneIndex); });

    syncEngineTracks();
    refreshSessionView();

    // Bigger blast radius than a track deletion — every clip on every track
    // in the row — so it earns the same reassurance, not less.
    showStatus("Deleted \"" + juce::String(name) + "\" - undo to bring it back");
}

void MainComponent::addSessionScene()
{
    std::string name;
    history_.edit("Add scene", [&name](model::Song& s)
    {
        name = "Scene " + std::to_string(s.scenes.size() + 1);
        model::addScene(s, name);
    });

    syncEngineTracks();
    refreshSessionView();
    showStatus("Added \"" + juce::String(name) + "\"");
}

/** Clicking an empty cell fills it with a copy of the track's currently open
    clip — the quickest way to get material into the grid without a separate
    "new session clip" flow. Declines if there's nothing to copy. */
void MainComponent::captureClipIntoSession(int trackIndex, int sceneIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) trackIndex].clips;
    if (clips.empty())
    {
        showError("Nothing to capture - this track has no clips");
        return;
    }

    const int  sourceIndex = juce::jlimit(0, (int) clips.size() - 1,
                                          trackIndex == selectedTrackIndex_ ? selectedClipIndex_ : 0);
    const auto source      = clips[(size_t) sourceIndex];

    history_.edit("Add session clip", [trackIndex, sceneIndex, &source](model::Song& s)
    {
        model::setSessionClip(s, trackIndex, sceneIndex, source);
    });

    syncEngineTracks();
    refreshSessionView();
}

/** Shows the selected track's insert effects. Unlike the Synth and Drums
    panes this applies to *every* track type — an audio track wants a filter
    as much as an instrument one does. */
void MainComponent::refreshEffectChainForSelected()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
    {
        effectChain_.setNoTrackSelected();
        return;
    }

    // Populated before setChain, so that when setChain selects the stored
    // routing the item it names is already in the list — the other order
    // silently resets every sidechain to "this track" on load.
    const auto& tracks = history_.current().tracks;
    std::vector<std::pair<int, juce::String>> sources;
    sources.reserve(tracks.size());

    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        if (i == selectedTrackIndex_)
            continue; // a track ducking itself is just an ordinary compressor
        sources.emplace_back(tracks[(size_t) i].id, juce::String(tracks[(size_t) i].name));
    }

    effectChain_.setSidechainSources(sources);
    effectChain_.setChain(tracks[(size_t) selectedTrackIndex_].effectChain);
}

/** Live tweak from the Track FX pane — updates the document in place (not a
    separate undo step per knob notch) and mirrors it into the engine, the
    same pattern the mixer faders and the Synth pane use. */
/** One chain slot's parameters in the engine's terms. `enabled` is the
    slot's own bypass, not the per-settings flag — bypass has to mean the same
    thing for a hosted plugin as for a built-in. */
/** How long the blend back to the unprocessed audio takes at each edge of a
    rendered selection. Long enough to remove the step an effect's level
    change leaves, far too short to hear as a fade. */
static constexpr double kEffectEdgeFadeSeconds = 0.005;

// makeEffectNode and toSlotParams used to live here. They moved to
// engine/EffectSlotFactory.h when a *third* copy of the same mapping turned
// up in the bounce tool: the tool's copy had silently fallen behind the
// engine's, so the tool was measuring a different signal path from the one
// the app plays - which is the one thing it exists not to do.

/** Adds a slot to the end of the selected track's chain. Structural, so it
    goes through history_ — and adding a plugin rebuilds the engine chain,
    which is what instantiates it. */
void MainComponent::addEffectSlot(model::EffectKind kind, const model::PluginRef& plugin)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Add effect", [index, kind, &plugin](model::Song& s)
    {
        model::EffectSlot slot;
        slot.kind    = kind;
        slot.enabled = true; // added because you want to hear it
        slot.plugin  = plugin;
        s.tracks[(size_t) index].effectChain.push_back(std::move(slot));
    });

    // Any open editor belongs to a node the rebuild is about to delete.
    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

void MainComponent::removeEffectSlot(int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Remove effect", [index, slotIndex](model::Song& s)
    {
        auto& chain = s.tracks[(size_t) index].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) chain.size())
            chain.erase(chain.begin() + slotIndex);
    });

    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Moves a slot one place up or down. Order is the whole point of a chain, so
    this is a real document edit rather than a view-only sort. */
void MainComponent::moveEffectSlot(int slotIndex, int delta)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Reorder effects", [index, slotIndex, delta](model::Song& s)
    {
        auto&     chain  = s.tracks[(size_t) index].effectChain;
        const int target = slotIndex + delta;
        if (slotIndex < 0 || slotIndex >= (int) chain.size() || target < 0 || target >= (int) chain.size())
            return;
        std::swap(chain[(size_t) slotIndex], chain[(size_t) target]);
    });

    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Bypass. Not structural — the node stays in the chain — so this is a live
    tweak straight into the document and the engine, with no rebuild and no
    plugin reinstantiation. */
void MainComponent::setEffectSlotBypass(int slotIndex, bool enabled)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    const int index = selectedTrackIndex_;
    history_.edit(enabled ? "Enable effect" : "Bypass effect", [index, slotIndex, enabled](model::Song& s)
    {
        s.tracks[(size_t) index].effectChain[(size_t) slotIndex].enabled = enabled;
    });

    const auto& updatedChain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, engine::toSlotParams(updatedChain[(size_t) slotIndex]));
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** A knob turn on a built-in slot: live, non-undoable per notch, same as the
    mixer faders. */
void MainComponent::setEffectSlotParams(const model::EffectSlot& slot, int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    auto& chain = history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    chain[(size_t) slotIndex] = slot;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, engine::toSlotParams(slot));
}

/** Probes for plugins and caches the result, so the next launch doesn't
    re-probe everything. In-process, so a plugin that crashes on probe takes
    the app with it — the dead man's pedal means it's skipped next time (see
    engine::PluginHost, and §20 for what's still owed here). */
void MainComponent::scanForPlugins()
{
    showBusy("Scanning for plugins...");

    const auto pedal = recordingsDirectory().getParentDirectory().getChildFile("plugin-scan.tmp");

    for (const auto& format : engine_.pluginHost().availableFormats())
        engine_.pluginHost().scanFormat(format, pedal);

    settings_.setValue("pluginScanCache", juce::String(engine_.pluginHost().saveScanCache()));
    settings_.saveIfNeeded();

    effectChain_.setAvailablePlugins(engine_.pluginHost().knownPlugins());
    showStatus("Found " + juce::String((int) engine_.pluginHost().knownPlugins().size()) + " plugin(s)");
}

/** Opens a hosted plugin's own editor. */
void MainComponent::openPluginEditor(int slotIndex)
{
    auto* node = engine_.trackPluginNode(selectedTrackIndex_, slotIndex);
    if (node == nullptr || node->instance() == nullptr)
    {
        showError("That plugin isn't loaded on this machine");
        return;
    }

    // One window per plugin instance; re-opening focuses the existing one.
    for (auto* existing : pluginWindows_)
        if (existing->plugin() == node->instance())
        {
            existing->toFront(true);
            return;
        }

    auto* window = pluginWindows_.add(new PluginEditorWindow(node->instance()->getName(), *node->instance()));
    window->onCloseRequested = [this](PluginEditorWindow* w) { pluginWindows_.removeObject(w); };
}

/** Closes every plugin editor. Called before anything that rebuilds a chain,
    because the rebuild deletes the PluginNodes those editors are drawing —
    an editor outliving its processor is a crash, not a glitch. */
void MainComponent::closePluginEditors()
{
    pluginWindows_.clear();
}
/** Copies the piano roll's selected notes, or the whole pattern if nothing
    is selected — the same "no selection means everything" rule quantize
    uses, so both commands are useful before the selection gesture is
    discovered. */
void MainComponent::copyNotes()
{
    const auto& pattern   = currentPattern();
    const auto& selection = pianoRoll_.selectedNoteIndices();

    noteClipboard_.clear();
    if (selection.empty())
    {
        noteClipboard_ = pattern.notes;
    }
    else
    {
        for (int index : selection)
            if (index >= 0 && index < (int) pattern.notes.size())
                noteClipboard_.push_back(pattern.notes[(size_t) index]);
    }

    showStatus("Copied " + juce::String((int) noteClipboard_.size()) + " note(s)");
}

/** Pastes notes into the open clip at the positions they were copied from,
    which is what makes "copy this part into that clip" work. Anything past
    the destination pattern's end is dropped rather than pasted somewhere it
    can't be seen or heard. */
void MainComponent::pasteNotes()
{
    if (noteClipboard_.empty() || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;
    const auto notes   = noteClipboard_;

    history_.edit("Paste notes", [trackIdx, clipIdx, &notes](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& pattern = clips[(size_t) clipIdx].pattern;
        for (const auto& note : notes)
            if (note.startBeats < pattern.lengthBeats)
                pattern.notes.push_back(note);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
}

/** Copies the selected clip whole — pattern, length and all. */
void MainComponent::copyClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    clipClipboard_.assign(1, clips[(size_t) selectedClipIndex_]);
    showStatus("Copied clip");
}

/** Pastes onto the selected track at the playhead, snapped to a beat — the
    playhead is the one position the user can see, which makes where it lands
    predictable. */
void MainComponent::pasteClip()
{
    if (clipClipboard_.empty() || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const double dropBeat = std::round(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()));
    const int    trackIdx = selectedTrackIndex_;
    auto         pasted   = clipClipboard_.front();
    int          newIndex = -1;

    history_.edit("Paste clip", [trackIdx, dropBeat, &pasted, &newIndex](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& track  = s.tracks[(size_t) trackIdx];

        auto clip       = pasted;
        clip.id         = model::allocateId(s); // a paste is a new clip, not the same one twice
        clip.startBeats = juce::jmax(0.0, dropBeat);
        track.clips.push_back(std::move(clip));
        newIndex = (int) track.clips.size() - 1;
    });

    if (newIndex >= 0)
        selectedClipIndex_ = newIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Copy + paste in one step, landing the copy immediately after the original
    — the usual way to extend a part by a bar. */
/** Whether selectedClipIndex_ currently names a real clip on the selected
    track — the same bounds check deleteSelectedClip() itself needs, pulled
    out so the bare-delete-key dispatcher (see keyPressed) can ask "is there
    a clip to act on" without duplicating it. */
bool MainComponent::hasSelectedClip() const
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return false;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    return selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) track.clips.size();
}

/** Deletes the selected arrangement clip. No confirmation: undo is the safety
    net for editing actions, and a prompt on every delete is friction the user
    pays for on the many times they meant it. */
void MainComponent::deleteSelectedClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size())
        return;

    const int trackId   = track.id;
    const int clipIndex = selectedClipIndex_;

    history_.edit("Delete clip", [trackId, clipIndex](model::Song& s)
    {
        model::removeClip(s, trackId, clipIndex);
    });

    // The clip after the deleted one shuffles down into its index; selecting
    // it keeps the selection somewhere real, and clamps at the end.
    const auto& clips = history_.current().tracks[(size_t) selectedTrackIndex_].clips;
    selectedClipIndex_ = clips.empty() ? 0 : juce::jmin(clipIndex, (int) clips.size() - 1);

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();

    // Reachable by a bare key and the most-used delete in the app, so it's
    // the one most likely to be hit by accident — same reasoning as track
    // deletion below.
    showStatus("Deleted clip - undo to bring it back");
}

/** Deletes the selected track and everything on it. Undo covers it, as with
    clips — but the last track isn't deletable, because a song with no tracks
    has no pane that can do anything and no obvious way back. */
void MainComponent::deleteSelectedTrack()
{
    deleteTrackAt(selectedTrackIndex_);
}

/** Deletes one track by index, which is not necessarily the selected one —
    the gear menu acts on the track whose gear was clicked.

    That is why the selection is fixed up through selectionAfterTrackRemoved
    rather than merely clamped: removing a track above the selected one shifts
    it down, and getting that wrong doesn't crash, it quietly leaves a
    different track selected than the one that was highlighted, so the next
    edit lands somewhere the user didn't mean. */
void MainComponent::deleteTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        showError("No track to delete");
        return;
    }

    if (song.tracks.size() <= 1)
    {
        showError("The last track can't be deleted");
        return;
    }

    const auto& track   = song.tracks[(size_t) trackIndex];
    const int   trackId = track.id;

    // Copied before the edit: committing one move-assigns the document, which
    // leaves any reference into the old one dangling.
    const auto name = track.name.empty() ? ("track " + juce::String(trackIndex + 1))
                                         : ("\"" + juce::String(track.name) + "\"");

    history_.edit("Delete track", [trackId](model::Song& s) { model::removeTrack(s, trackId); });

    selectTrackAndRefreshAll(selectionAfterTrackRemoved(selectedTrackIndex_, trackIndex,
                                                        trackCount()));

    // Deleting is reachable by an unmodified key and by one menu click, so it
    // can be hit by accident. Saying what went and that undo will bring it
    // back is the difference between a recoverable slip and a mystery.
    showStatus("Deleted " + name + " - undo to bring it back");
}

/** Renames the selected track. Track names are the only label distinguishing
    one strip, row or tab from the next, and until now they were whatever the
    Add button happened to generate. */
void MainComponent::renameSelectedTrack()
{
    renameTrackAt(selectedTrackIndex_);
}

/** Copies a track and everything on it, putting the copy directly after it.

    The point of duplicating a track here is a second copy of a part to loop
    against the first, so the copy has to be complete — clips, instrument
    settings, effect chain and all — and it has to be genuinely separate,
    which is what model::duplicateTrack's reissuing of every id gives. */
void MainComponent::duplicateTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        showError("No track to duplicate");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const auto sourceName = juce::String(song.tracks[(size_t) trackIndex].name);

    history_.edit("Duplicate track", [trackIndex](model::Song& s)
    {
        model::duplicateTrack(s, trackIndex);
    });

    selectTrackAndRefreshAll(trackIndex + 1); // the copy, so it can be worked on straight away
    showStatus("Duplicated " + (sourceName.isEmpty() ? juce::String("track") : "\"" + sourceName + "\""));
}

/** Copies the selected track for later pasting. Deliberately its own
    clipboard rather than sharing the clip one: pasting a track when a clip
    was copied, or the reverse, is the kind of guess that loses work. */
void MainComponent::copyTrack()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        showError("No track selected");
        return;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    trackClipboard_   = track;

    showStatus("Copied " + (track.name.empty() ? juce::String("track")
                                               : "\"" + juce::String(track.name) + "\""));
}

void MainComponent::pasteTrack()
{
    if (! trackClipboard_.has_value())
    {
        showError("No track copied");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const auto copied = *trackClipboard_;

    // appendTrackCopy reissues every id, so pasting the same buffer twice
    // gives two genuinely separate tracks rather than two the app can't tell
    // apart.
    history_.edit("Paste track", [&copied](model::Song& s) { model::appendTrackCopy(s, copied); });

    selectTrackAndRefreshAll(trackCount() - 1);
    showStatus("Pasted " + (copied.name.empty() ? juce::String("track")
                                                : "\"" + juce::String(copied.name) + "\""));
}

/** The per-track settings menu, opened from the gear in the tracks pane. */
void MainComponent::showTrackSettingsMenu(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];

    juce::PopupMenu colours;
    for (int i = 0; i < kNumTrackColours; ++i)
    {
        const auto& option = kTrackColours[i];
        const bool  chosen = (track.colour == option.argb);

        // Ticked rather than swatched: PopupMenu has no colour-chip item, and
        // a tick at least says which one is in force.
        colours.addItem(kFirstColourMenuId + i, option.name, true, chosen);
    }

    // Audio isn't offered here: it's a different authoring mode (a file-backed
    // clip, not a MIDI pattern) rather than another instrument the same clip
    // content could play through, so it doesn't belong in a "change which
    // instrument plays these notes" picker the way Instrument/Drum/Guitar do.
    struct TypeOption { model::TrackType type; const char* name; };
    static constexpr TypeOption kTypeOptions[] = {
        { model::TrackType::Instrument, "Instrument (Synth)" },
        { model::TrackType::Drum,       "Drum Kit" },
        { model::TrackType::Guitar,     "Guitar" },
    };
    juce::PopupMenu instrumentTypes;
    for (const auto& option : kTypeOptions)
        instrumentTypes.addItem(kFirstTrackTypeMenuId + (int) option.type, option.name,
                                true, track.type == option.type);

    juce::PopupMenu menu;
    menu.addSectionHeader(track.name.empty() ? ("Track " + juce::String(trackIndex + 1))
                                             : juce::String(track.name));
    menu.addSubMenu("Colour", colours);
    menu.addSubMenu("Instrument Type", instrumentTypes);
    menu.addItem(1, "Rename...");
    menu.addSeparator();

    // Greyed rather than absent when it's the last track: an item that isn't
    // there reads as a missing feature, where a disabled one says the rule.
    menu.addItem(2, "Delete Track", song.tracks.size() > 1);

    juce::Component::SafePointer<MainComponent> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [self, trackIndex](int result)
    {
        if (self == nullptr || result == 0)
            return;

        if (result == 1)
        {
            self->renameTrackAt(trackIndex);
            return;
        }

        if (result == 2)
        {
            self->deleteTrackAt(trackIndex);
            return;
        }

        if (const int index = result - kFirstColourMenuId; index >= 0 && index < kNumTrackColours)
        {
            self->setTrackColour(trackIndex, kTrackColours[index].argb);
            return;
        }

        if (result >= kFirstTrackTypeMenuId)
            self->setTrackType(trackIndex, (model::TrackType) (result - kFirstTrackTypeMenuId));
    });
}

/** Colour is document state, so it's an undoable edit rather than a live
    tweak — unlike mute, which is a performance control you flip while
    listening and would not want filling the undo stack. */
void MainComponent::setTrackColour(int trackIndex, unsigned int argb)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;

    history_.edit("Recolour track", [trackId, argb](model::Song& s)
    {
        if (auto* track = model::findTrack(s, trackId))
            track->colour = argb;
    });

    arrangementView_.setSong(history_.current());
    updateMixerStrips();
}

/** Changes which instrument a track's MIDI clips play through — Instrument
    (synth), Drum Kit, or Guitar. A track's clips already hold the same thing
    regardless of type (a Pattern of notes; see model::Clip) and every
    per-type settings struct (drumKit, guitarSettings) is kept regardless of
    which type is active, the same "don't lose the others" trade EffectSlot
    already makes for its built-in-vs-plugin choice — so switching type never
    loses anything and is fully reversible.

    The one first-use gap: a track that has never been a Drum track has an
    empty drumKit.pads (see Track.h), which would route its notes to a
    kit with nothing assigned to any pad — silence, not an error, but not
    useful either. Populated with the same starting kit a brand-new Drum
    track gets, and only if it's still empty, so flipping back and forth
    doesn't clobber pads someone already assigned. */
void MainComponent::setTrackType(int trackIndex, model::TrackType newType)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;

    // Computed outside the edit lambda: it's a const member function (needs
    // access to factoryDrumKitDirectory()), which a history_.edit lambda
    // capturing only plain values can't reach. Matches Add Drum and the
    // starter song, which both already use this over the bare
    // model::makeDefaultDrumKit() for exactly this reason — its pads come
    // with real synthesized samples already assigned, so a track switched
    // to Drum Kit is audible immediately instead of silent until someone
    // manually loads four samples.
    const model::DrumKit factoryKit = defaultDrumKitWithFactorySamples();

    history_.edit("Change instrument type", [trackId, newType, factoryKit](model::Song& s)
    {
        auto* track = model::findTrack(s, trackId);
        if (track == nullptr)
            return;

        track->type = newType;
        if (newType == model::TrackType::Drum && track->drumKit.pads.empty())
            track->drumKit = factoryKit;
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    updateMixerStrips();
    updateEditingLabel();
}

/** One-click tone templates: overwrites the selected Guitar track's
    GuitarSettings and effect chain with the hand-tuned values for @p tone
    (see model::presetForGuitarTone, which is what actually knows them). One
    undo step, same shape as applyPreset/setTrackType. */
void MainComponent::applyGuitarTone(engine::GuitarTone tone)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    if (history_.current().tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Guitar)
        return;

    const int trackIndex = selectedTrackIndex_;
    const auto preset     = model::presetForGuitarTone(tone);
    history_.edit(std::string(engine::guitarToneName(tone)) + " Tone", [trackIndex, preset](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        track.guitarSettings = preset.guitar;
        track.effectChain    = preset.effectChain;
    });

    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    updateMixerStrips();
    updateEditingLabel();
}

/** One-click synth tone templates: overwrites the selected Instrument
    track's SynthSettings and effect chain with the hand-tuned values for
    @p tone (see model::presetForSynthTone). Same body as applyPreset's, but
    read from an in-memory table instead of a file — so unlike a saved
    preset there's nothing to fail to load, and nothing to report. */
void MainComponent::applySynthTone(engine::SynthTone tone)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    if (history_.current().tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Instrument)
        return;

    const int  trackIndex = selectedTrackIndex_;
    const auto preset     = model::presetForSynthTone(tone);
    history_.edit(preset.name + " Tone", [trackIndex, preset](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        track.synthSettings = preset.synth;
        track.effectChain   = preset.effectChain;
    });

    syncEngineTracks();
    refreshSynthEditorForSelected();
    refreshEffectChainForSelected();
    updateMixerStrips();
    updateEditingLabel();
    showStatus("Applied synth tone: " + juce::String(preset.name));
}

/** One-click drum kit styles: replaces the selected Drum track's whole kit
    with the samples and per-pad mix for @p style (see
    engine::padsForDrumKitStyle). Every style keeps the same four note
    numbers and labels on purpose — clips store raw note numbers, and
    generateDrumLoop looks pads up by label, so a kit that renumbered its
    pads would silently orphan every note already written against it. */
void MainComponent::applyDrumKitStyle(engine::DrumKitStyle style)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    if (history_.current().tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Drum)
        return;

    const int  trackIndex = selectedTrackIndex_;
    const auto kit        = kitForDrumKitStyle(style);

    history_.edit(std::string(engine::drumKitStyleName(style)) + " Kit",
        [trackIndex, kit](model::Song& s)
        {
            s.tracks[(size_t) trackIndex].drumKit = kit;
        });

    syncEngineTracks();
    // The piano roll builds its drum rows from drumKit.pads, so it would
    // otherwise keep drawing the old kit's labels.
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    updateMixerStrips();
    showStatus("Applied drum kit: " + juce::String(engine::drumKitStyleName(style)));
}

/** Moves an arrangement clip from one track to another, in place of a
    same-track reposition (see ArrangementView::onClipMovedToTrack — this
    only ever fires when the drop landed on a track the view already
    confirmed is compatible, but the check is repeated here rather than
    trusted, since the model layer shouldn't rely on a UI-side gate alone).

    Composed from the same two model primitives every other clip edit uses —
    erase from the source track's clips, model::addClip into the
    destination (which reissues the id) — as one history_.edit, so a
    cross-track drag is one undo step just like a same-track one. */
void MainComponent::moveClipToTrack(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats)
{
    const auto& song = history_.current();
    if (srcTrackIndex < 0 || srcTrackIndex >= (int) song.tracks.size())
        return;
    if (destTrackIndex < 0 || destTrackIndex >= (int) song.tracks.size())
        return;

    const auto& srcTrack = song.tracks[(size_t) srcTrackIndex];
    if (clipIndex < 0 || clipIndex >= (int) srcTrack.clips.size())
        return;

    const auto& destTrack = song.tracks[(size_t) destTrackIndex];
    if (srcTrack.type != destTrack.type || srcTrack.type == model::TrackType::Audio)
        return;

    const int srcTrackId  = srcTrack.id;
    const int destTrackId = destTrack.id;
    int       newClipIndex = -1;

    history_.edit("Move clip to track", [srcTrackId, destTrackId, clipIndex, newStartBeats, &newClipIndex](model::Song& s)
    {
        auto* source = model::findTrack(s, srcTrackId);
        if (source == nullptr || clipIndex < 0 || clipIndex >= (int) source->clips.size())
            return;

        model::Clip clip = source->clips[(size_t) clipIndex]; // copied before erase invalidates the reference
        source->clips.erase(source->clips.begin() + clipIndex);
        clip.startBeats = juce::jmax(0.0, newStartBeats);

        if (model::addClip(s, destTrackId, clip) != nullptr)
        {
            if (auto* dest = model::findTrack(s, destTrackId))
                newClipIndex = (int) dest->clips.size() - 1;
        }
    });

    if (newClipIndex < 0)
        return; // the edit above bailed out (a stale index) - nothing to select or refresh

    selectedTrackIndex_ = destTrackIndex;
    selectedClipIndex_  = newClipIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

void MainComponent::renameTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track   = song.tracks[(size_t) trackIndex];
    const int   trackId = track.id;

    auto* window = new juce::AlertWindow("Rename Track", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", juce::String(track.name), "Name:");
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<MainComponent>(this), window, trackId](int result)
            {
                if (self == nullptr || result != 1)
                    return;

                const auto name = window->getTextEditorContents("name").trim();
                if (name.isEmpty())
                    return; // an unnamed track is worse than the generated name

                self->history_.edit("Rename track", [trackId, name](model::Song& s)
                {
                    model::renameTrack(s, trackId, name.toStdString());
                });

                self->syncEngineTracks();
                self->updateMixerStrips();
                self->refreshSessionView();
                self->arrangementView_.setSong(self->history_.current());
                self->updateEditingLabel();
            }),
        true);
}

void MainComponent::duplicateClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const auto source   = clips[(size_t) selectedClipIndex_];
    const int  trackIdx = selectedTrackIndex_;
    int        newIndex = -1;

    history_.edit("Duplicate clip", [trackIdx, &source, &newIndex](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIdx];

        auto clip       = source;
        clip.id         = model::allocateId(s);
        clip.startBeats = source.startBeats + source.lengthBeats;
        track.clips.push_back(std::move(clip));
        newIndex = (int) track.clips.size() - 1;
    });

    if (newIndex >= 0)
        selectedClipIndex_ = newIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Snaps the open clip's notes onto the grid, optionally swung. Acts on the
    piano roll's selection, or the whole pattern when nothing is selected. */
void MainComponent::quantizeNotes(double swingAmount)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& song = history_.current();
    if (selectedTrackIndex_ >= (int) song.tracks.size())
        return;
    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const int  trackIdx  = selectedTrackIndex_;
    const int  clipIdx   = selectedClipIndex_;
    const auto selection = pianoRoll_.selectedNoteIndices();
    const int  affected  = selection.empty()
                              ? (int) clips[(size_t) clipIdx].pattern.notes.size()
                              : (int) selection.size();

    if (affected == 0)
    {
        showStatus("Nothing to " + juce::String(swingAmount > 0.0 ? "swing" : "quantize")
                   + " - this clip has no notes");
        return;
    }

    history_.edit(swingAmount > 0.0 ? "Swing" : "Quantize",
                  [trackIdx, clipIdx, swingAmount, &selection](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& trackClips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) trackClips.size())
            return;

        // The grid the editor draws is 16ths, so that's what notes snap to.
        engine::NoteOps::quantizeNotes(trackClips[(size_t) clipIdx].pattern.notes, 0.25, swingAmount, selection);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();

    // Reloading the pattern clears the selection, which would silently widen
    // a follow-up Swing to the whole part. Quantizing never adds, removes or
    // reorders notes, so the same indices still mean the same notes.
    pianoRoll_.setSelectedNoteIndices(selection);

    showStatus((swingAmount > 0.0 ? "Swung " : "Quantized ") + juce::String(affected)
               + (affected == 1 ? " note" : " notes"));
}

/** Sets a clip's window on the timeline (from the arrangement's resize
    handle). Note this is the window, not the pattern's loop length — see
    setPatternBars. A track holding a *single* clip is still given an
    unbounded window by syncEngineTracks (the long-standing "one clip plays
    until Stop" rule), so resizing a lone clip changes what you see and what
    gets exported, but not when it stops sounding; that only bites once the
    track has more than one clip. */
void MainComponent::setClipLength(int trackIndex, int clipIndex, double newLengthBeats)
{
    history_.edit("Resize clip", [trackIndex, clipIndex, newLengthBeats](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < (int) clips.size())
            clips[(size_t) clipIndex].lengthBeats = juce::jmax(1.0, newLengthBeats);
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Sets how many bars the open clip's pattern loops over. Growing the pattern
    also grows the clip's window if the window would otherwise be too short to
    contain it — keeping a clip able to hold its own content isn't the same as
    silently re-looping it, which is why the window is only ever grown here,
    never shrunk. */
void MainComponent::setPatternBars(int bars)
{
    if (bars <= 0 || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const double beatsPerBar = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    const double lengthBeats = beatsPerBar * bars;
    const int    trackIdx    = selectedTrackIndex_;
    const int    clipIdx     = selectedClipIndex_;

    history_.edit("Set pattern length", [trackIdx, clipIdx, lengthBeats](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIdx];
        clip.pattern.lengthBeats = lengthBeats;
        clip.lengthBeats         = juce::jmax(clip.lengthBeats, lengthBeats);

        // Notes now past the end would be unreachable in the editor and
        // silent in the sequencer, so drop them rather than leave them
        // invisibly attached to the clip.
        auto& notes = clip.pattern.notes;
        notes.erase(std::remove_if(notes.begin(), notes.end(),
                                   [lengthBeats](const engine::Note& n) { return n.startBeats >= lengthBeats; }),
                    notes.end());
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Mirrors the open clip's pattern length into the Bars box. */
void MainComponent::updateBarsControl()
{
    const double beatsPerBar = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    const auto&  pattern     = currentPattern();
    const int    bars        = juce::jmax(1, (int) std::llround(pattern.lengthBeats / beatsPerBar));

    // Only reflects lengths the box actually offers; an odd length set
    // elsewhere leaves it blank rather than silently rounding the clip.
    barsBox_.setSelectedId(bars == 1 || bars == 2 || bars == 4 ? bars : 0, juce::dontSendNotification);
}

/** model::PluginFormat -> the name JUCE's format manager uses. The document
    stores an enum so the file format doesn't depend on JUCE's spelling; this
    is the one place the two meet. */
static std::string pluginFormatName(model::PluginFormat format)
{
    switch (format)
    {
        case model::PluginFormat::VST3:      return "VST3";
        case model::PluginFormat::AudioUnit: return "AudioUnit";
        case model::PluginFormat::Unknown:   break;
    }
    return {};
}

/** Converts a track's model automation lanes into the engine's curve form.
    The engine can't use model::AutomationLane directly — `model` already
    depends on `engine`, so the dependency can't run both ways — and this is
    the single place the two representations meet, used by both live playback
    and the offline exporter. */
static engine::TrackAutomation toTrackAutomation(const model::Track& track)
{
    engine::TrackAutomation curves;

    auto copyLane = [&track](model::TrackParam param, engine::AutomationCurve& into)
    {
        if (const auto* lane = track.lane(param))
        {
            for (const auto& point : lane->points())
                into.addPoint(point.beat, point.value);
            into.sortPoints();
        }
    };

    copyLane(model::TrackParam::Gain, curves.gain);
    copyLane(model::TrackParam::Pan, curves.pan);
    copyLane(model::TrackParam::SendLevel, curves.sendLevel);
    return curves;
}

void MainComponent::syncEngineTracks()
{
    const auto& song = history_.current();
    const int   n    = juce::jmin((int) song.tracks.size(), engine_.maxTracks());

    for (int i = 0; i < n; ++i)
    {
        const auto& track = song.tracks[(size_t) i];

        std::vector<engine::ClipSlot> slots;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Instrument)
                continue; // audio clips aren't sequenced

            engine::ClipSlot slot;
            slot.pattern     = clip.pattern;
            slot.startBeats  = clip.startBeats;
            slot.lengthBeats = clip.lengthBeats;
            slots.push_back(slot);
        }
        engine_.setTrackClips(i, slots);

        // Audio clips -> the track's own audio-clip player. Each Audio-type
        // clip becomes one AudioClipSlot, gated to its own
        // [startBeats, startBeats+lengthBeats) window exactly like the
        // instrument clips above. Unconditionally resubmitted every sync,
        // same as instrument clips — cheap, since AudioEngine caches decoded
        // audio by file path (see AudioEngine::setTrackAudioClips), so this
        // never re-decodes a file it's already loaded, even across tracks
        // that share one.
        std::vector<engine::AudioClipSpec> audioSpecs;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;

            engine::AudioClipSpec spec;
            spec.file        = juce::File(clip.audioFile);
            spec.startBeats  = clip.startBeats;
            spec.lengthBeats = clip.lengthBeats;
            spec.gainDb      = clip.gainDb;
            spec.stretchFactor = warpFactorFor(clip);
            audioSpecs.push_back(spec);
        }
        // Submitted even when empty, which the guard here used to skip: the
        // engine holds the last list it was given, so deleting a track's only
        // audio clip left that clip still loaded and still playing, with
        // nothing on screen to explain it. "Unconditionally resubmitted" in
        // the comment above is only true if it's also submitted when there's
        // nothing to submit.
        engine_.setTrackAudioClips(i, audioSpecs);

        // Drum kit -> routes this track's notes to the drum sampler instead
        // of the synth (see InstrumentTrack::instrument — unlike audio
        // clips, the synth doesn't naturally stay silent without content, so
        // this has to be explicit). Unconditionally resubmitted every sync
        // for the same reason as the clip lists above: cheap, since
        // AudioEngine caches decoded samples by path.
        engine_.setTrackInstrument(i, track.type == model::TrackType::Drum   ? engine::TrackInstrument::Drum
                                    : track.type == model::TrackType::Guitar ? engine::TrackInstrument::Guitar
                                                                             : engine::TrackInstrument::Synth);

        if (track.type == model::TrackType::Guitar)
        {
            const auto& guitar = track.guitarSettings;
            engine_.setTrackGuitarSettings(i, guitar);
            engine_.setTrackGuitarTuning(i, guitar.tuning);
        }
        if (track.type == model::TrackType::Drum)
        {
            // Pad solo is resolved here rather than on the audio thread: the
            // whole pad map is rebuilt and swapped on any kit change anyway,
            // so the engine only ever needs the effective mute. Same
            // "solo overrides, mute always wins" rule as track solo.
            const bool anyPadSoloed = std::any_of(track.drumKit.pads.begin(), track.drumKit.pads.end(),
                                                  [](const model::DrumPad& p) { return p.solo; });

            std::vector<engine::DrumPadSpec> padSpecs;
            for (const auto& pad : track.drumKit.pads)
            {
                engine::DrumPadSpec spec;
                spec.noteNumber     = pad.noteNumber;
                spec.file           = pad.samplePath.empty() ? juce::File() : juce::File(pad.samplePath);
                spec.gainDb         = pad.gainDb;
                spec.pan            = pad.pan;
                spec.pitchSemitones = pad.pitchSemitones;
                spec.muted          = pad.muted || (anyPadSoloed && ! pad.solo);
                padSpecs.push_back(spec);
            }
            engine_.setTrackDrumKit(i, padSpecs);
        }

        engine_.setTrackMuted(i, track.muted);
        engine_.setTrackSolo(i, track.solo);
        engine_.setTrackGainDb(i, track.gainDb);
        engine_.setTrackPan(i, track.pan);
        engine_.setTrackAutomation(i, toTrackAutomation(track));

        // The session grid's column for this track. Empty slots are submitted
        // too — the index is the scene, so the list has to stay aligned with
        // Song::scenes even where there's nothing to play.
        std::vector<engine::SessionSlotData> sessionSlots;
        sessionSlots.reserve(track.sessionSlots.size());
        for (const auto& slot : track.sessionSlots)
        {
            engine::SessionSlotData data;
            data.hasClip = slot.hasClip && slot.clip.type == model::ClipType::Instrument;
            if (data.hasClip)
                data.pattern = slot.clip.pattern;
            sessionSlots.push_back(std::move(data));
        }
        engine_.setTrackSessionSlots(i, sessionSlots);
        engine_.setTrackSendLevel(i, track.sendLevel);

        const auto& synth = track.synthSettings;
        engine_.setTrackSynthWaveform(i, synth.waveform);
        engine_.setTrackSynthAttackMs(i, synth.attackMs);
        engine_.setTrackSynthDecayMs(i, synth.decayMs);
        engine_.setTrackSynthSustain(i, synth.sustain);
        engine_.setTrackSynthReleaseMs(i, synth.releaseMs);
        engine_.setTrackSynthFilterEnabled(i, synth.filterEnabled);
        engine_.setTrackSynthFilterMode(i, synth.filterMode);
        engine_.setTrackSynthFilterCutoff(i, synth.filterCutoff);
        engine_.setTrackSynthFilterResonance(i, synth.filterResonance);
        engine_.setTrackSynthGainDb(i, synth.gainDb);
        engine_.setTrackSynthFilterEnvAmount(i, synth.filterEnvAmount);
        engine_.setTrackSynthFilterEnvAttackMs(i, synth.filterEnvAttackMs);
        engine_.setTrackSynthFilterEnvDecayMs(i, synth.filterEnvDecayMs);
        engine_.setTrackSynthFilterEnvSustain(i, synth.filterEnvSustain);
        engine_.setTrackSynthFilterEnvReleaseMs(i, synth.filterEnvReleaseMs);
        engine_.setTrackSynthSubOscEnabled(i, synth.subOscEnabled);
        engine_.setTrackSynthSubOscLevel(i, synth.subOscLevel);
        engine_.setTrackSynthUnisonVoices(i, synth.unisonVoices);
        engine_.setTrackSynthUnisonDetuneCents(i, synth.unisonDetuneCents);

        // Sidechain routing: the document names the source by track *id*, the
        // engine addresses its pool by index, and this is the only place that
        // knows both. Resolving here (rather than storing an index) is what
        // stops deleting or reordering a track from silently re-pointing a
        // sidechain at whatever instrument inherited that slot.
        //
        // The last compressor with a source set wins if a chain somehow holds
        // two: the engine routes one detector per track, and picking the last
        // is at least a rule rather than an accident of iteration order.
        int sidechainSourceIndex = -1;
        for (const auto& slot : track.effectChain)
        {
            if (slot.kind != model::EffectKind::Compressor || slot.compressor.sidechainTrackId < 0)
                continue;

            const int sourceIndex = trackIndexForId(slot.compressor.sidechainTrackId);
            if (sourceIndex >= 0 && sourceIndex != i)
                sidechainSourceIndex = sourceIndex;
        }
        engine_.setTrackSidechainSource(i, sidechainSourceIndex);

        // Group-bus routing. Both sides go through the id->index bridge for
        // the same reason the sidechain does: the document names tracks by id
        // so that deleting or reordering one cannot silently re-route audio
        // into whatever inherited its slot.
        engine_.setTrackIsBus(i, track.type == model::TrackType::Bus);
        engine_.setTrackOutputBus(i, trackIndexForId(track.outputBusId));

        // The chain's shape, in order. Only pushed when it actually changed —
        // rebuilding resets every tail in the chain, so an unrelated edit must
        // not glitch a delay (see AudioEngine::setTrackEffectChain).
        std::vector<engine::EffectSlotSpec> chainSpecs;
        chainSpecs.reserve(track.effectChain.size());
        for (const auto& slot : track.effectChain)
        {
            engine::EffectSlotSpec spec;
            switch (slot.kind)
            {
                case model::EffectKind::Filter: spec.kind = engine::EffectNodeKind::Filter; break;
                case model::EffectKind::Delay:  spec.kind = engine::EffectNodeKind::Delay;  break;
                case model::EffectKind::Reverb: spec.kind = engine::EffectNodeKind::Reverb; break;
                case model::EffectKind::Drive:  spec.kind = engine::EffectNodeKind::Drive;  break;
                case model::EffectKind::Compressor: spec.kind = engine::EffectNodeKind::Compressor; break;
                case model::EffectKind::Tremolo:    spec.kind = engine::EffectNodeKind::Tremolo;    break;
                case model::EffectKind::Chorus:     spec.kind = engine::EffectNodeKind::Chorus;     break;
                case model::EffectKind::Wobble:     spec.kind = engine::EffectNodeKind::Wobble;     break;
                case model::EffectKind::Gate:       spec.kind = engine::EffectNodeKind::Gate;       break;
                case model::EffectKind::Eq:         spec.kind = engine::EffectNodeKind::Eq;         break;
                case model::EffectKind::Plugin:
                    spec.kind             = engine::EffectNodeKind::Plugin;
                    spec.pluginFormat     = pluginFormatName(slot.plugin.format);
                    spec.pluginIdentifier = slot.plugin.identifier;
                    spec.pluginState      = slot.plugin.state;
                    break;
            }
            chainSpecs.push_back(std::move(spec));
        }
        // A rebuild destroys this track's nodes, hosted plugins included, so
        // any editor drawing one has to go first. Only on an actual rebuild —
        // closing plugin windows on every unrelated edit would be maddening.
        if (engine_.setTrackEffectChain(i, chainSpecs))
            closePluginEditors();

        // Parameters, one call per slot, addressed by position — a chain may
        // hold two filters, and "the filter" stops meaning anything then.
        for (size_t s = 0; s < track.effectChain.size(); ++s)
            engine_.setTrackEffectSlotParams(i, (int) s, engine::toSlotParams(track.effectChain[s]));
    }
    engine_.setActiveTrackCount(n);

    // The arrangement just changed, so the loop it runs over has too. This is
    // the one place every clip edit passes through, which is why it lives
    // here rather than in each of them.
    updateLoopRegion();
}

void MainComponent::refreshPianoRollForSelected()
{
    pianoRoll_.setPattern(currentPattern());
    updateBarsControl();

    // The same condition currentPattern() falls back to its shared empty
    // Pattern for — the roll can't otherwise tell "nothing is open" apart
    // from "a real clip that's genuinely empty."
    const auto& song = history_.current();
    const bool  noClipOpen = selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size()
                          || selectedClipIndex_ < 0
                          || selectedClipIndex_ >= (int) song.tracks[(size_t) selectedTrackIndex_].clips.size();
    pianoRoll_.setNoClipSelected(noClipOpen);

    if (! noClipOpen)
    {
        const auto& track = song.tracks[(size_t) selectedTrackIndex_];
        pianoRoll_.setTrackInfo(track.name, track.colour);
    }

    const bool isDrum = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                     && history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Drum;

    if (isDrum)
        pianoRoll_.setDrumPads(history_.current().tracks[(size_t) selectedTrackIndex_].drumKit.pads);
    else
        pianoRoll_.setMelodicMode();
}

/** Shows the Drums pane's kit and step grid for the selected track, or a
    placeholder if it isn't a Drum track — the same gating
    refreshSynthEditorForSelected does for Instrument tracks. */
void MainComponent::refreshDrumsPaneForSelected()
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
    {
        drumsPane_.setNoDrumTrackSelected();
        return;
    }

    const auto& track = history_.current().tracks[(size_t) trackIndex];
    drumsPane_.setKit(track.drumKit.pads, currentPattern());
    drumsPane_.setTrackInfo(track.name, track.colour);
}

/** The selected clip if it's an Audio clip that actually references a file,
    or nullptr. Everything the audio editor does needs all three of those to
    hold, so they're checked once here rather than at each call site. */
/** The time-stretch this clip needs to sit at the project's tempo.

    The tempo is taken *at the clip's start* rather than as one project-wide
    number, because with a tempo map there is no such single number. That is
    also this feature's honest v1 limit: a clip spanning a tempo change warps
    to the tempo it begins at and then drifts, which is a deliberate deferral
    (see docs/PLAN.md §29) rather than an oversight — warping across a ramp
    means a time-varying ratio and a different rendering strategy entirely. */
int MainComponent::trackIndexForId(int trackId) const
{
    if (trackId < 0)
        return -1;

    const auto& tracks = history_.current().tracks;
    for (int i = 0; i < (int) tracks.size(); ++i)
        if (tracks[(size_t) i].id == trackId)
            return i;

    // A source track that has since been deleted. Reported as "no sidechain"
    // rather than clamped to some other track — the routing is gone, and the
    // compressor falling back to its own input is the least surprising thing
    // that can happen.
    return -1;
}

double MainComponent::warpFactorFor(const model::Clip& clip) const
{
    return engine::warpStretchFactor(clip.sourceBpm,
                                     model::tempoAtBeat(history_.current(), clip.startBeats),
                                     clip.warpEnabled);
}

/** Runs tempo detection over a clip's decoded audio. Message thread, and not
    instant on a long file — the caller shows a busy message. */
engine::TempoEstimate MainComponent::detectTempoForClip(const model::Clip& clip)
{
    if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
        return {};

    return engine_.detectFileTempo(juce::File(clip.audioFile));
}

/** Switches warping on or off for the selected audio clip. */
void MainComponent::toggleClipWarp()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    if (clip->sourceBpm <= 0.0)
    {
        // Nothing to warp *to*. Reachable only if the menu item's enablement
        // and the model disagree, but saying so beats silently doing nothing.
        showError("This clip's tempo isn't known - use Detect Clip Tempo first");
        return;
    }

    const int  trackIndex = selectedTrackIndex_;
    const int  clipIndex  = selectedClipIndex_;
    const bool turningOn  = ! clip->warpEnabled;

    history_.edit(turningOn ? "Warp clip" : "Unwarp clip", [trackIndex, clipIndex, turningOn](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;
        clips[(size_t) clipIndex].warpEnabled = turningOn;
    });

    // Rendering the stretch happens inside this call, on the message thread,
    // so a long clip pauses briefly here rather than glitching the audio
    // thread — the whole reason warping is pre-rendered.
    if (turningOn)
        showBusy("Warping clip...");

    syncEngineTracks();

    const double projectBpm = model::tempoAtBeat(history_.current(), clip->startBeats);
    showStatus(turningOn
        ? "Warped " + juce::String(clip->sourceBpm, 1) + " BPM clip to "
              + juce::String(projectBpm, 1) + " BPM"
        : juce::String("Warping off - clip plays at its own rate"));
}

/** Detects (or re-detects) the selected clip's tempo and stores it. */
void MainComponent::detectSelectedClipTempo()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    showBusy("Detecting tempo...");

    const auto estimate = detectTempoForClip(*clip);
    if (! estimate.isUsable())
    {
        showError("Could not detect a tempo in this clip");
        return;
    }

    const int trackIndex = selectedTrackIndex_;
    const int clipIndex  = selectedClipIndex_;
    const double detected = estimate.bpm;

    history_.edit("Detect clip tempo", [trackIndex, clipIndex, detected](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;
        clips[(size_t) clipIndex].sourceBpm = detected;
    });

    syncEngineTracks();

    // The confidence is reported rather than hidden: a detector that always
    // answers, with no way to tell a sure 128 from a coin-flip 91, is one that
    // will eventually warp something to a tempo it invented.
    showStatus("Detected " + juce::String(estimate.bpm, 1) + " BPM"
               + (estimate.confidence < 0.5 ? "  (low confidence - check it)" : ""));
}

/** Makes the project follow the clip rather than the other way round. */
void MainComponent::setProjectTempoFromClip()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr || clip->sourceBpm <= 0.0)
        return;

    const double bpm = clip->sourceBpm;
    setTempoAtPlayhead(bpm);
    showStatus("Project tempo set to " + juce::String(bpm, 1) + " BPM from the clip");
}

const model::Clip* MainComponent::selectedAudioClip() const
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return nullptr;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return nullptr;

    const auto& clip = clips[(size_t) selectedClipIndex_];
    if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
        return nullptr;

    return &clip;
}

/** Shows the selected audio clip in the editor, or a placeholder if the
    selection isn't one — the same is-it-this-kind gating the Synth, Drums
    and Guitar panes use. */
/** Hands the automation pane the selected track's lane for whichever
    parameter it is showing. */
void MainComponent::refreshAutomationPaneForSelected()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
    {
        automationPane_.setNoTrackSelected();
        return;
    }

    const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
    const auto  param = automationPane_.param();

    // A track with no lane for this parameter gets an empty one rather than
    // nothing: an empty lane is a real state (no automation, sitting at the
    // static value) and is the one you start drawing into.
    const auto* lane = track.lane(param);

    automationPane_.setLane(track.name.empty()
                                ? ("Track " + juce::String(selectedTrackIndex_ + 1))
                                : juce::String(track.name),
                            track.type,
                            lane != nullptr ? *lane : model::AutomationLane {},
                            juce::jmax(16.0, songEndBeats()));
}

/** Commits a lane edited in the automation pane.

    One undo step per gesture, not per breakpoint: the pane reports the whole
    lane when a drag ends, which is the same "a drag is one edit" rule the
    mixer faders follow. */
void MainComponent::applyEditedAutomationLane(model::TrackParam param,
                                              const model::AutomationLane& lane)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;

    history_.edit("Edit automation", [index, param, &lane](model::Song& s)
    {
        if (index < 0 || index >= (int) s.tracks.size())
            return;

        auto& track = s.tracks[(size_t) index];

        // An emptied lane is erased rather than stored empty, so a track with
        // no automation carries no lanes at all — the state every serialization
        // and playback path already treats as "use the static value".
        if (lane.empty())
            track.automation.erase((int) param);
        else
            track.laneFor(param) = lane;
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
}

void MainComponent::refreshAudioEditorForSelected()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
    {
        audioEditor_.setNoAudioClipSelected();
        return;
    }

    const juce::File file(clip->audioFile);
    const auto&      track = history_.current().tracks[(size_t) selectedTrackIndex_];

    // The file's own duration, not the clip's window: the editor edits the
    // recording, and a clip shortened on the timeline still has all of its
    // audio behind it.
    audioEditor_.setClip(file, engine_.probeDurationSeconds(file), clip->gainDb,
                         track.name, track.colour);
    audioEditor_.setNoisePrintCaptured(! noiseProfiles_.empty() && noiseProfileFile_ == file);

    // Read once per file, not once per refresh. Denoise writes a new file
    // and repoints the clip, so a changed path is exactly the signal that
    // the audio itself changed and the peaks are stale.
    if (file != waveformPeaksFile_)
    {
        double     sampleRate = 0.0;
        const auto channels   = readAudioFileChannels(file, sampleRate);

        waveformPeaks_.clear();
        if (! channels.empty() && sampleRate > 0.0)
            waveformPeaks_.build(channels);

        waveformPeaksFile_       = file;
        waveformPeaksSampleRate_ = sampleRate;
    }

    audioEditor_.setWaveform(waveformPeaks_, waveformPeaksSampleRate_);
}

/** Writes the selected clip's gain. Live during a slider drag — the
    surrounding beginStructDrag/commitStructDrag pair is what makes the whole
    drag one undo step, same as every other continuous control here. */
void MainComponent::setSelectedClipGainDb(float gainDb)
{
    if (selectedAudioClip() == nullptr)
        return;

    const int trackIndex = selectedTrackIndex_;
    const int clipIndex  = selectedClipIndex_;

    auto& song = history_.mutableCurrent();
    song.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].gainDb = gainDb;

    syncEngineTracks();
}

/** Sets the clip's gain so its loudest sample just reaches kNormaliseTargetPeak.

    Reads the file rather than using the thumbnail's summary: a thumbnail is a
    downsampled peak envelope, so it can under-report the true peak by enough
    to leave a "normalised" clip clipping. Reading is exact and happens once,
    on a button press, which is the one place it's affordable. */
void MainComponent::normaliseSelectedClip()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const juce::File file(clip->audioFile);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        showError("Could not read " + file.getFileName());
        return;
    }

    // Both extremes per channel, then the largest magnitude across them: a
    // waveform is rarely symmetric, so taking only the maximum would
    // under-read a signal whose biggest excursion is negative.
    const int numChannels = juce::jmax(1, (int) reader->numChannels);
    std::vector<juce::Range<float>> levels((size_t) numChannels);
    reader->readMaxLevels(0, reader->lengthInSamples, levels.data(), numChannels);

    float peak = 0.0f;
    for (const auto& range : levels)
        peak = juce::jmax(peak, std::abs(range.getStart()), std::abs(range.getEnd()));

    if (peak <= 0.0f)
    {
        // Silence has no peak to normalise to, and the alternative is
        // dividing by zero and handing the clip an infinite gain.
        showError("That clip is silent");
        return;
    }

    const float gainDb = juce::Decibels::gainToDecibels(kNormaliseTargetPeak / peak);

    const int trackIndex = selectedTrackIndex_;
    const int clipIndex  = selectedClipIndex_;
    history_.edit("Normalize clip", [trackIndex, clipIndex, gainDb](model::Song& s)
    {
        s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].gainDb = gainDb;
    });

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    showStatus("Normalized: " + juce::String(gainDb, 1) + " dB");
}

/** Pushes the document's mastering rack into the pane and the engine. The
    one place both are refreshed from the model, so undo, load and a preset
    click all land the same way. */
void MainComponent::updateMasteringControls()
{
    const auto& mastering = history_.current().mastering;
    masteringPane_.setSettings(mastering);
    engine_.setMastering(mastering);
}

/** Live tweak from the mastering pane — document in place, then the engine,
    same path as the master EQ sliders. The undo step is bracketed by the
    drag pair below rather than taken per move. */
void MainComponent::setMasteringSettings(const model::MasteringSettings& settings)
{
    history_.mutableCurrent().mastering = settings;
    engine_.setMastering(settings);
}

void MainComponent::beginMasteringDrag()
{
    masteringDragging_ = true;
    masteringDragFrom_ = history_.current().mastering;
}

void MainComponent::endMasteringDrag()
{
    if (! masteringDragging_)
        return;

    masteringDragging_ = false;

    commitStructDrag(history_, "Set mastering", masteringDragFrom_,
                     history_.current().mastering,
                     [](model::Song& s, const model::MasteringSettings& value)
    {
        s.mastering = value;
    });
}

/** One-click mastering starting points — see model::presetForMastering,
    which is what knows the values. One undo step, same shape as every other
    preset application here. */
void MainComponent::applyMasteringPreset(engine::MasteringPreset preset)
{
    const auto settings = model::presetForMastering(preset);
    history_.edit(std::string(engine::masteringPresetName(preset)) + " mastering",
                  [settings](model::Song& s) { s.mastering = settings; });

    updateMasteringControls();
    showStatus("Mastering: " + juce::String(engine::masteringPresetName(preset)));
}

/** Reads @p file fully into per-channel float vectors.

    Deliberately its own read rather than reaching into AudioEngine's decode
    cache: that cache is keyed by *path* and shared by every clip pointing at
    the same file, so processing a buffer borrowed from it would silently
    alter every other clip using that recording. Offline editing here always
    reads fresh and writes somewhere new. */
std::vector<std::vector<float>> MainComponent::readAudioFileChannels(const juce::File& file,
                                                                     double& sampleRateOut) const
{
    std::vector<std::vector<float>> channels;
    sampleRateOut = 0.0;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return channels;

    const int numChannels = juce::jmax(1, (int) reader->numChannels);
    const int length      = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                          (juce::int64) std::numeric_limits<int>::max());

    juce::AudioBuffer<float> buffer(numChannels, length);
    reader->read(&buffer, 0, length, 0, true, true);
    sampleRateOut = reader->sampleRate;

    channels.resize((size_t) numChannels);
    for (int ch = 0; ch < numChannels; ++ch)
        channels[(size_t) ch].assign(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + length);

    return channels;
}

/** Copies the selection into the audio clipboard. Non-destructive, so it
    doesn't go through applyDestructiveEdit. */
void MainComponent::copyAudioSelection()
{
    int    from = 0, to = 0, length = 0;
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;

    if (! selectedSampleRange(from, to, length, sampleRate, channels, false))
    {
        showError("Select part of the clip first");
        return;
    }

    audioClipboard_.clear();
    for (const auto& channel : channels)
        audioClipboard_.push_back(engine::audioedits::extractRange(channel, from, to));

    audioClipboardSampleRate_ = sampleRate;
    showStatus("Copied " + juce::String((double) (to - from) / sampleRate, 2) + "s");
}

/** Runs @p transform over the selected range of the clip's audio.

    The range is resolved *inside* the transform, where the samples and the
    file's sample rate are already in hand. That's what makes this read the
    file once: resolving it beforehand meant decoding to find the boundaries
    and then decoding again to edit, which on a long take is two full passes
    over the whole recording for every button press. */
bool MainComponent::editSelection(
    const juce::String& label, bool snapToZeroCrossings,
    const std::function<void(std::vector<std::vector<float>>&, int from, int to, double sampleRate)>& transform)
{
    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        // For a destructive edit "no selection" must refuse rather than mean
        // "the whole clip" — a stray click before Cut would otherwise destroy
        // the take.
        showError("Select part of the clip first");
        return false;
    }

    return applyDestructiveEditToAllChannels(label,
        [range, snapToZeroCrossings, &transform](std::vector<std::vector<float>>& channels, double sampleRate)
    {
        const int length = (int) channels[0].size();
        int       from   = juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
        int       to     = juce::jlimit(0, length, (int) std::llround(range.endSeconds * sampleRate));

        if (snapToZeroCrossings)
        {
            // Decided once from the first channel and applied to all: snapping
            // each channel to its own crossing would shear a stereo file apart
            // at the edit point.
            from = engine::audioedits::nearestZeroCrossing(channels[0], from);
            to   = engine::audioedits::nearestZeroCrossing(channels[0], to);
            if (to < from)
                std::swap(from, to);
        }

        transform(channels, from, to, sampleRate);
    });
}

void MainComponent::cutAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Cut audio", true, [this](std::vector<std::vector<float>>& channels,
                                                int from, int to, double sampleRate)
        {
            // Lifted before the removal, from the same samples that are about
            // to be cut.
            audioClipboard_.clear();
            for (const auto& channel : channels)
                audioClipboard_.push_back(engine::audioedits::extractRange(channel, from, to));
            audioClipboardSampleRate_ = sampleRate;

            for (auto& channel : channels)
                channel = engine::audioedits::removeRange(channel, from, to);
        }))
        showStatus("Cut " + juce::String(range.lengthSeconds(), 2) + "s");
}

void MainComponent::deleteAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Delete audio", true, [](std::vector<std::vector<float>>& channels,
                                               int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::removeRange(channel, from, to);
        }))
        showStatus("Deleted " + juce::String(range.lengthSeconds(), 2) + "s");
}

/** Pastes the clipboard at the selection's start, replacing the selection if
    there is one. Resamples when the clipboard came from a file at another
    rate — otherwise pasting 44.1k into 48k would play back too fast. */
void MainComponent::pasteAudioAtSelection()
{
    if (audioClipboard_.empty())
    {
        showError("Nothing to paste");
        return;
    }

    const auto range = audioEditor_.selection();

    // With no selection, paste lands at the cursor — not at the start. It
    // used to read startSeconds off an *empty* range, which is always zero,
    // so every paste without a selection went to the beginning of the clip
    // however far along the cursor had been placed.
    const double atSeconds = range.isEmpty() ? audioEditor_.cursorSeconds() : range.startSeconds;

    const bool applied = applyDestructiveEditToAllChannels("Paste audio",
        [this, range, atSeconds](std::vector<std::vector<float>>& channels, double sampleRate)
    {
        const int wanted = juce::jmax(0, (int) std::llround(atSeconds * sampleRate));

        // A cursor past the end of the file is a request to paste *after* the
        // recording, so the gap is filled with silence rather than the paste
        // being dragged back to the last sample.
        for (auto& channel : channels)
            if (wanted > (int) channel.size())
                channel.resize((size_t) wanted, 0.0f);

        const int length = (int) channels[0].size();
        const int at     = juce::jlimit(0, length, wanted);
        const int until  = range.isEmpty()
                             ? at
                             : juce::jlimit(at, length, (int) std::llround(range.endSeconds * sampleRate));

        const double ratio = audioClipboardSampleRate_ > 0.0 ? audioClipboardSampleRate_ / sampleRate : 1.0;

        for (int ch = 0; ch < (int) channels.size(); ++ch)
        {
            // A mono clipboard into a stereo clip (or the reverse) reuses the
            // last available channel rather than refusing — the same rule the
            // players follow for channel-count mismatches.
            const auto& source = audioClipboard_[(size_t) juce::jmin(ch, (int) audioClipboard_.size() - 1)];
            const auto  fitted = std::abs(ratio - 1.0) < 1.0e-9
                                     ? source
                                     : engine::audioedits::resample(source, ratio);

            const auto cleared = until > at
                                     ? engine::audioedits::removeRange(channels[(size_t) ch], at, until)
                                     : channels[(size_t) ch];
            channels[(size_t) ch] = engine::audioedits::insertAt(cleared, fitted, at);
        }
    });

    if (applied)
        showStatus("Pasted");
}

void MainComponent::trimToAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Trim audio", true, [](std::vector<std::vector<float>>& channels,
                                             int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::keepRange(channel, from, to);
        }))
        showStatus("Trimmed to " + juce::String(range.lengthSeconds(), 2) + "s");
}

void MainComponent::silenceAudioSelection()
{
    // No zero-crossing snap: nothing moves, so there is no join to click.
    if (editSelection("Silence audio", false, [](std::vector<std::vector<float>>& channels,
                                                 int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::silenceRange(channel, from, to);
        }))
        showStatus("Silenced");
}

void MainComponent::fadeInAudioSelection()
{
    if (editSelection("Fade in", false, [](std::vector<std::vector<float>>& channels,
                                           int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::fadeIn(channel, from, to);
        }))
        showStatus("Faded in");
}

void MainComponent::fadeOutAudioSelection()
{
    if (editSelection("Fade out", false, [](std::vector<std::vector<float>>& channels,
                                            int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::fadeOut(channel, from, to);
        }))
        showStatus("Faded out");
}

void MainComponent::reverseAudioSelection()
{
    if (editSelection("Reverse audio", true, [](std::vector<std::vector<float>>& channels,
                                                int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::reverseRange(channel, from, to);
        }))
        showStatus("Reversed");
}

void MainComponent::splitClipAtSelection()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        showError("Select where to split first");
        return;
    }

    const juce::File source(clip->audioFile);
    double           sampleRate = 0.0;
    const auto       channels   = readAudioFileChannels(source, sampleRate);
    if (channels.empty() || sampleRate <= 0.0)
    {
        showError("Could not read " + source.getFileName());
        return;
    }

    const int length = (int) channels[0].size();
    int       at     = juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
    at = engine::audioedits::nearestZeroCrossing(channels[0], at);

    if (at <= 0 || at >= length)
    {
        showError("That split point is at the very edge of the clip");
        return;
    }

    // Both halves written before the document is touched, so a failure to
    // write leaves the project exactly as it was.
    auto writeHalf = [&](int from, int to) -> juce::File
    {
        juce::AudioBuffer<float> buffer((int) channels.size(), to - from);
        for (int ch = 0; ch < (int) channels.size(); ++ch)
        {
            const auto part = engine::audioedits::keepRange(channels[(size_t) ch], from, to);
            std::copy(part.begin(), part.end(), buffer.getWritePointer(ch));
        }

        const auto file = editsDirectory()
                              .getNonexistentChildFile(source.getFileNameWithoutExtension(), ".wav");
        return engine::OfflineRenderer::writeWav(file, buffer, sampleRate) ? file : juce::File{};
    };

    const auto firstFile  = writeHalf(0, at);
    const auto secondFile = writeHalf(at, length);
    if (firstFile == juce::File{} || secondFile == juce::File{})
    {
        showError("Could not write the split halves");
        return;
    }

    const int    trackIndex   = selectedTrackIndex_;
    const int    clipIndex    = selectedClipIndex_;
    const double bpm          = history_.current().bpm;
    const double firstBeats   = engine::beatsForSeconds((double) at / sampleRate, bpm);
    const double secondBeats  = engine::beatsForSeconds((double) (length - at) / sampleRate, bpm);
    const auto   firstPath    = firstFile.getFullPathName().toStdString();
    const auto   secondPath   = secondFile.getFullPathName().toStdString();

    history_.edit("Split audio", [=](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        auto& first = track.clips[(size_t) clipIndex];

        const double startBeats = first.startBeats;
        first.audioFile   = firstPath;
        first.lengthBeats = juce::jmax(0.25, firstBeats);

        model::Clip second = first;
        second.audioFile   = secondPath;
        second.lengthBeats = juce::jmax(0.25, secondBeats);
        second.startBeats  = startBeats + first.lengthBeats;

        model::addClip(s, track.id, second); // reissues the id
    });

    noiseProfiles_.clear();
    noiseProfileFile_  = juce::File{};
    waveformPeaksFile_ = juce::File{};

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    arrangementView_.setSong(history_.current());
    showStatus("Split at " + juce::String((double) at / sampleRate, 2) + "s");
}

/** Offers a scratch effect chain to render into the selection. */
void MainComponent::showApplyEffectsDialog()
{
    if (selectedAudioClip() == nullptr)
        return;

    if (audioEditor_.selection().isEmpty())
    {
        showError("Select part of the clip first");
        return;
    }

    auto dialog = std::make_unique<ApplyEffectsDialog>();
    dialog->setSize(520, 460);

    auto* raw = dialog.get();
    raw->onApply = [this, raw](const std::vector<model::EffectSlot>& chain)
    {
        applyEffectsToSelection(chain);
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };
    raw->onCancel = [raw]
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Apply Effects to Selection";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/** Renders @p chain into the selected range.

    The range is processed as its own buffer and written back over the
    original, with a short crossfade at each boundary. Without the crossfade
    an effect that changes level — any compressor, or a reverb's wet mix —
    produces a step at the edges of the selection, heard as a click exactly
    where the edit begins and ends. A few milliseconds of blend removes it
    and is far too short to be heard as a fade.

    Plugin slots are skipped: instantiating one needs the plugin host, which
    lives in the engine, and a half-rendered chain would be worse than an
    honest refusal. */
void MainComponent::applyEffectsToSelection(const std::vector<model::EffectSlot>& chain)
{
    const auto range = audioEditor_.selection();
    if (range.isEmpty())
        return;

    int builtIns = 0, plugins = 0;
    for (const auto& slot : chain)
    {
        if (! slot.enabled)
            continue;
        (slot.kind == model::EffectKind::Plugin ? plugins : builtIns) += 1;
    }

    if (builtIns == 0)
    {
        showError(plugins > 0 ? "Plugins can't be rendered into a selection yet"
                              : "Add an effect first");
        return;
    }

    const double bpm = history_.current().bpm;

    const bool applied = applyDestructiveEditToAllChannels("Apply effects",
        [&chain, range, bpm](std::vector<std::vector<float>>& channels, double sampleRate)
    {
        const int total = (int) channels[0].size();
        const int from  = juce::jlimit(0, total, (int) std::llround(range.startSeconds * sampleRate));
        const int to    = juce::jlimit(from, total, (int) std::llround(range.endSeconds * sampleRate));
        const int count = to - from;
        if (count <= 0)
            return;

        const int numChannels = (int) channels.size();

        juce::AudioBuffer<float> block(numChannels, count);
        for (int ch = 0; ch < numChannels; ++ch)
            std::copy(channels[(size_t) ch].begin() + from,
                      channels[(size_t) ch].begin() + to,
                      block.getWritePointer(ch));

        engine::EffectChain built;
        for (const auto& slot : chain)
        {
            if (! slot.enabled || slot.kind == model::EffectKind::Plugin)
                continue;

            if (auto node = engine::makeConfiguredNode(slot))
                built.add(std::move(node));
        }

        built.prepare(sampleRate, count);
        built.setBpm(bpm); // the wobble pedal is tempo-locked
        built.process(block);

        // Blend back over the original at both edges.
        const int fade = juce::jmin(count / 2, (int) std::llround(sampleRate * kEffectEdgeFadeSeconds));
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto*       destination = channels[(size_t) ch].data() + from;
            const auto* processed   = block.getReadPointer(ch);

            for (int i = 0; i < count; ++i)
            {
                float wet = 1.0f;
                if (fade > 0)
                {
                    if (i < fade)                 wet = (float) i / (float) fade;
                    else if (i >= count - fade)   wet = (float) (count - 1 - i) / (float) fade;
                }
                destination[i] = destination[i] * (1.0f - wet) + processed[i] * wet;
            }
        }
    });

    if (applied)
        showStatus(plugins > 0 ? "Applied effects (plugins skipped)" : "Applied effects");
}

/** Measures the frequency content of the audio editor's selection.

    Falls back to the whole clip when nothing is selected: unlike the
    destructive actions, analysing everything is harmless — the same rule the
    audition transport follows. */
void MainComponent::analyseSelection()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    double     sampleRate = 0.0;
    const auto channels   = readAudioFileChannels(juce::File(clip->audioFile), sampleRate);
    if (channels.empty() || sampleRate <= 0.0)
    {
        showError("Could not read that clip");
        return;
    }

    const auto range  = audioEditor_.selection();
    const int  length = (int) channels[0].size();
    const int  from   = range.isEmpty() ? 0
                          : juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
    const int  to     = range.isEmpty() ? length
                          : juce::jlimit(from, length, (int) std::llround(range.endSeconds * sampleRate));

    // Channel 0 rather than a sum: summing a stereo pair cancels whatever is
    // out of phase between them, which would hide exactly the kind of problem
    // someone opens an analyser to find.
    const std::vector<float> passage(channels[0].begin() + from, channels[0].begin() + to);

    const auto measured = engine::spectrum::analyse(passage, sampleRate);
    if (measured.isEmpty())
    {
        showError("That selection is too short to analyse - select at least ~50ms");
        return;
    }

    analyserPane_.setSpectrum(measured);
    showStatus("Analysed " + juce::String((double) (to - from) / sampleRate, 2) + "s");
}

/** Speed and pitch, on the whole clip.

    Whole clip rather than a selection on purpose: both change the audio's
    duration, and splicing a re-timed section back into the middle of a clip
    would either leave a gap or overlap what follows. Audacity's own
    Change Speed works this way for the same reason. */
void MainComponent::showSpeedPitchDialog()
{
    if (selectedAudioClip() == nullptr)
        return;

    auto* window = new juce::AlertWindow("Speed and Pitch", {}, juce::MessageBoxIconType::NoIcon, this);

    window->addComboBox("speed", { "0.5x (half)", "0.75x", "1x (unchanged)", "1.5x", "2x (double)" },
                        "Speed (moves pitch with it):");
    window->getComboBoxComponent("speed")->setSelectedItemIndex(2);

    juce::StringArray semitones;
    for (int i = -12; i <= 12; ++i)
        semitones.add(i == 0 ? juce::String("0 (unchanged)") : juce::String(i > 0 ? "+" : "") + juce::String(i));
    window->addComboBox("pitch", semitones, "Pitch, keeping the length:");
    window->getComboBoxComponent("pitch")->setSelectedItemIndex(12); // 0

    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            static const double kSpeeds[] = { 0.5, 0.75, 1.0, 1.5, 2.0 };
            const int speedIndex = window->getComboBoxComponent("speed")->getSelectedItemIndex();
            const int pitchIndex = window->getComboBoxComponent("pitch")->getSelectedItemIndex();

            self->applySpeedAndPitch(kSpeeds[(size_t) juce::jlimit(0, 4, speedIndex)],
                                     (double) (juce::jlimit(0, 24, pitchIndex) - 12));
        }));
}

/** Applies a speed change and a pitch shift to the whole clip.

    Speed first, then pitch: the pitch shift preserves length, so doing it
    second means it operates on the already-retimed audio and the two
    settings compose the way the dialog implies. */
void MainComponent::applySpeedAndPitch(double speedFactor, double semitones)
{
    const bool changesSpeed = std::abs(speedFactor - 1.0) > 1.0e-9;
    const bool changesPitch = std::abs(semitones) > 1.0e-9;

    if (! changesSpeed && ! changesPitch)
        return;

    showBusy("Processing...");

    const bool applied = applyDestructiveEditToAllChannels("Speed and pitch",
        [speedFactor, semitones, changesSpeed, changesPitch](std::vector<std::vector<float>>& channels, double)
    {
        for (auto& channel : channels)
        {
            if (changesSpeed)
                channel = engine::timestretch::changeSpeed(channel, speedFactor);
            if (changesPitch)
                channel = engine::timestretch::pitchShift(channel, semitones);
        }
    });

    if (applied)
    {
        juce::String what;
        if (changesSpeed) what += juce::String(speedFactor, 2) + "x speed";
        if (changesSpeed && changesPitch) what += ", ";
        if (changesPitch) what += juce::String(semitones > 0 ? "+" : "") + juce::String((int) semitones) + " semitones";
        showStatus("Applied " + what);
    }
}

/** The song beat a point in the selected clip's file corresponds to.

    The audio editor works in seconds into a file; the transport works in
    song beats. This is the one place that conversion lives, so the click
    gesture and the playhead drawing can't disagree about it. */
double MainComponent::songBeatForClipSeconds(double secondsIntoFile) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return 0.0;

    return clip->startBeats + engine::beatsForSeconds(secondsIntoFile, history_.current().bpm);
}

/** The inverse: where the song's playhead falls inside the selected clip's
    file. Negative before the clip starts, past its end after — the editor
    simply draws the playhead off the edge of the view in both cases. */
double MainComponent::clipSecondsForSongBeat(double beat) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return 0.0;

    const double bpm = history_.current().bpm;
    if (bpm <= 0.0)
        return 0.0;

    return (beat - clip->startBeats) * 60.0 / bpm;
}

/** Measures the noise in the selected range, per channel.

    Per channel rather than from a mono sum: a stereo recording's two sides
    routinely have different noise floors (different preamps, or one side
    nearer a fan), and subtracting an average from both would under-clean one
    and over-clean the other. */
void MainComponent::captureNoisePrint()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        showError("Select a passage of noise first");
        return;
    }

    const juce::File file(clip->audioFile);
    double           sampleRate = 0.0;
    const auto       channels   = readAudioFileChannels(file, sampleRate);

    if (channels.empty() || sampleRate <= 0.0)
    {
        showError("Could not read " + file.getFileName());
        return;
    }

    const int total = (int) channels[0].size();
    const int from  = juce::jlimit(0, total, (int) std::llround(range.startSeconds * sampleRate));
    const int to    = juce::jlimit(from, total, (int) std::llround(range.endSeconds * sampleRate));

    std::vector<engine::NoiseProfile> profiles;
    for (const auto& channel : channels)
    {
        const std::vector<float> passage(channel.begin() + from, channel.begin() + to);
        profiles.push_back(engine::noisereduction::captureNoiseProfile(passage));
    }

    // captureNoiseProfile refuses a passage shorter than one analysis frame,
    // which is the honest answer rather than a profile built from padding —
    // so that refusal has to be reported, not silently stored.
    if (profiles.empty() || profiles[0].isEmpty())
    {
        showError("That selection is too short to measure - select at least ~50ms");
        return;
    }

    noiseProfiles_    = std::move(profiles);
    noiseProfileFile_ = file;
    audioEditor_.setNoisePrintCaptured(true);
    showStatus("Noise print captured from "
               + juce::String(range.lengthSeconds(), 2) + "s");
}

/** Subtracts the captured print from the whole clip, writing the result to a
    new file and repointing the clip at it in one undo step.

    Writing a new file rather than editing in place is what keeps this
    undoable and keeps it from leaking: undo just points the clip back at the
    original, which is still on disk and untouched. It also sidesteps the
    decode cache's path-keyed sharing entirely — a new path is a new entry. */
void MainComponent::reduceNoiseOnSelectedClip(float amountDb, float floorDb)
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    if (noiseProfiles_.empty() || noiseProfileFile_ != juce::File(clip->audioFile))
    {
        showError("Capture a noise print from this clip first");
        return;
    }

    showBusy("Reducing noise...");

    // Through the shared destructive path rather than its own copy of it.
    // This predated that helper and had drifted: it wrote into the
    // Recordings folder alongside real takes instead of the Edits folder
    // every other edit uses, and it repeated the repoint-and-invalidate
    // sequence that only has to be right once.
    const bool applied = applyDestructiveEditToAllChannels("Reduce noise",
        [this, amountDb, floorDb](std::vector<std::vector<float>>& channels, double)
    {
        for (int ch = 0; ch < (int) channels.size(); ++ch)
        {
            // A mono print on a stereo file (or the reverse) is possible if
            // the file changed underneath; reusing the last profile is better
            // than refusing, and clamping is how.
            const auto& profile = noiseProfiles_[(size_t) juce::jmin(ch, (int) noiseProfiles_.size() - 1)];
            channels[(size_t) ch] = engine::noisereduction::reduceNoise(channels[(size_t) ch], profile,
                                                                        amountDb, floorDb);
        }
    });

    if (applied)
        showStatus("Noise reduced");
}

juce::File MainComponent::editsDirectory() const
{
    // Separate from Recordings: these are derived files, and mixing them in
    // with takes makes it impossible to tell which is which when clearing
    // out space later.
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Edits");
    dir.createDirectory();
    return dir;
}

/** Reads the selected clip and resolves the editor's selection to sample
    indices. Returns false — having reported why — when there's no selection,
    which for a destructive edit must refuse rather than quietly mean "the
    whole clip": a stray click before Cut would otherwise destroy the take. */
bool MainComponent::selectedSampleRange(int& fromOut, int& toOut, int& lengthOut,
                                        double& sampleRateOut,
                                        std::vector<std::vector<float>>& channelsOut,
                                        bool snapToZeroCrossings) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return false;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
        return false;

    channelsOut = readAudioFileChannels(juce::File(clip->audioFile), sampleRateOut);
    if (channelsOut.empty() || sampleRateOut <= 0.0)
        return false;

    lengthOut = (int) channelsOut[0].size();
    fromOut   = juce::jlimit(0, lengthOut, (int) std::llround(range.startSeconds * sampleRateOut));
    toOut     = juce::jlimit(0, lengthOut, (int) std::llround(range.endSeconds * sampleRateOut));

    if (snapToZeroCrossings)
    {
        // Decided once, from the first channel, and applied to all of them:
        // snapping each channel to its own crossing would shear a stereo
        // file apart at the edit point.
        fromOut = engine::audioedits::nearestZeroCrossing(channelsOut[0], fromOut);
        toOut   = engine::audioedits::nearestZeroCrossing(channelsOut[0], toOut);
        if (toOut < fromOut)
            std::swap(fromOut, toOut);
    }

    return true;
}

/** The one path every destructive edit takes.

    Centralised because each step is easy to forget individually and each
    failure is quiet: a clip whose lengthBeats no longer matches its file is
    silently "repaired" by ClipLengthRepair (undoing the edit), and a stale
    waveform-peaks cache draws the old audio over the new. */
bool MainComponent::applyDestructiveEdit(
    const juce::String& label,
    const std::function<std::vector<float>(const std::vector<float>&, int channel)>& transform)
{
    return applyDestructiveEditToAllChannels(label,
        [&transform](std::vector<std::vector<float>>& channels, double)
        {
            for (int ch = 0; ch < (int) channels.size(); ++ch)
                channels[(size_t) ch] = transform(channels[(size_t) ch], ch);
        });
}

bool MainComponent::applyDestructiveEditToAllChannels(
    const juce::String& label,
    const std::function<void(std::vector<std::vector<float>>&, double sampleRate)>& transform)
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return false;

    const juce::File source(clip->audioFile);
    double           sampleRate = 0.0;
    auto             channels   = readAudioFileChannels(source, sampleRate);
    if (channels.empty() || sampleRate <= 0.0)
    {
        showError("Could not read " + source.getFileName());
        return false;
    }

    transform(channels, sampleRate);

    const int newLength = channels.empty() ? 0 : (int) channels[0].size();
    if (newLength <= 0)
    {
        // Editing a clip down to nothing would leave a clip referencing an
        // unreadable file, which plays as silence with no explanation.
        showError("That would leave the clip empty");
        return false;
    }

    juce::AudioBuffer<float> buffer((int) channels.size(), newLength);
    for (int ch = 0; ch < (int) channels.size(); ++ch)
    {
        // Channels can differ in length only if a transform is inconsistent,
        // which is a bug — but writing past the buffer would be a crash, so
        // it is clamped rather than trusted.
        const int count = juce::jmin(newLength, (int) channels[(size_t) ch].size());
        buffer.clear(ch, 0, newLength);
        std::copy(channels[(size_t) ch].begin(), channels[(size_t) ch].begin() + count,
                  buffer.getWritePointer(ch));
    }

    const auto destination = editsDirectory()
                                 .getNonexistentChildFile(source.getFileNameWithoutExtension(), ".wav");
    if (! engine::OfflineRenderer::writeWav(destination, buffer, sampleRate))
    {
        showError("Could not write " + destination.getFileName());
        return false;
    }

    // lengthBeats is derived from the file's duration, not chosen — see
    // ClipLengthRepair, which will "correct" any clip that disagrees. An
    // edit that changed the duration without this would be silently undone.
    const double newSeconds     = (double) newLength / sampleRate;
    const int    trackIndex     = selectedTrackIndex_;
    const int    clipIndex      = selectedClipIndex_;
    const auto   newPath        = destination.getFullPathName().toStdString();
    const double newLengthBeats = engine::beatsForSeconds(newSeconds, history_.current().bpm);

    history_.edit(label.toStdString(), [trackIndex, clipIndex, newPath, newLengthBeats](model::Song& s)
    {
        auto& target       = s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
        target.audioFile   = newPath;
        target.lengthBeats = juce::jmax(0.25, newLengthBeats);
    });

    // A noise print described the old file, and the peaks cache is keyed by
    // path — without clearing it the editor keeps drawing the old audio.
    noiseProfiles_.clear();
    noiseProfileFile_  = juce::File{};
    waveformPeaksFile_ = juce::File{};

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    arrangementView_.setSong(history_.current());
    return true;
}

/** The selected track's index if it's a Drum track, or -1 — the one check
    every drum-kit edit below needs before touching the document. */
int MainComponent::selectedDrumTrackIndex() const
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return -1;
    return history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Drum
               ? selectedTrackIndex_ : -1;
}

/** Live tweak of one pad's mute/solo/gain/pan/pitch — updates the document in
    place (not a separate undo step), same as a mixer fader. Deliberately does
    not rebuild the kit editor's rows: they hold the very slider being dragged
    (see DrumKitEditor::setPads); only the step grid, which dims muted pads,
    needs refreshing. */
void MainComponent::setDrumPadMix(int padIndex, const model::DrumPad& pad)
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
        return;

    auto& pads = history_.mutableCurrent().tracks[(size_t) trackIndex].drumKit.pads;
    if (padIndex < 0 || padIndex >= (int) pads.size())
        return;

    pads[(size_t) padIndex] = pad;
    syncEngineTracks(); // rebuilds this track's pad map with the new mix settings
    drumsPane_.refreshPadsForMixChange(pads);
}

/** Adds a pad to the selected kit, on the next free MIDI note above the
    highest one it already uses — a structural edit, so it goes through
    history_ like adding a track or clip. */
void MainComponent::addDrumPad()
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
        return;

    history_.edit("Add drum pad", [trackIndex](model::Song& s)
    {
        auto& pads = s.tracks[(size_t) trackIndex].drumKit.pads;

        int highestNote = 35; // one below the usual GM kick, so an empty kit starts at 36
        for (const auto& pad : pads)
            highestNote = juce::jmax(highestNote, pad.noteNumber);

        model::DrumPad pad;
        pad.noteNumber = juce::jmin(127, highestNote + 1);
        pad.label      = "Pad " + std::to_string(pads.size() + 1);
        pads.push_back(pad);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
}

/** Removes a pad, along with any notes that triggered it — leaving orphaned
    hits behind would show up as a silent row nothing can play. Never removes
    the last pad (the editor's Remove button is disabled at one pad). */
void MainComponent::removeDrumPad(int padIndex)
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
        return;

    history_.edit("Remove drum pad", [trackIndex, padIndex](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        auto& pads  = track.drumKit.pads;
        if (padIndex < 0 || padIndex >= (int) pads.size() || pads.size() <= 1)
            return;

        const int removedNote = pads[(size_t) padIndex].noteNumber;
        pads.erase(pads.begin() + padIndex);

        for (auto& clip : track.clips)
        {
            auto& notes = clip.pattern.notes;
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [removedNote](const engine::Note& n) { return n.noteNumber == removedNote; }),
                        notes.end());
        }
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();

    // More destructive than the row disappearing suggests: every hit that
    // played this pad, on every clip on the track, went with it.
    showStatus("Removed pad - undo to bring it back");
}

/** Shows the Synth pane's controls for the selected track's timbre, or a
    placeholder if it's not an Instrument track (Drum/Audio tracks have no
    synth to edit) — the same is-it-this-track-type gating
    refreshPianoRollForSelected already does for the drum-kit editor. */
void MainComponent::refreshSynthEditorForSelected()
{
    const bool isInstrument = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                            && history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Instrument;

    if (isInstrument)
    {
        const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
        synthEditor_.setSettings(track.synthSettings);
        synthEditor_.setTrackInfo(track.name, track.colour);
    }
    else
        synthEditor_.setNoTrackSelected();
}

/** Live tweak from the Synth pane (a knob turn) — updates the current
    document in place, same non-undoable-per-notch pattern as setTrackGain,
    and mirrors it into the engine. */
void MainComponent::setTrackSynthSettings(const model::SynthSettings& settings)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].synthSettings = settings;

    const int index = selectedTrackIndex_;
    engine_.setTrackSynthWaveform(index, settings.waveform);
    engine_.setTrackSynthAttackMs(index, settings.attackMs);
    engine_.setTrackSynthDecayMs(index, settings.decayMs);
    engine_.setTrackSynthSustain(index, settings.sustain);
    engine_.setTrackSynthReleaseMs(index, settings.releaseMs);
    engine_.setTrackSynthFilterEnabled(index, settings.filterEnabled);
    engine_.setTrackSynthFilterMode(index, settings.filterMode);
    engine_.setTrackSynthFilterCutoff(index, settings.filterCutoff);
    engine_.setTrackSynthFilterResonance(index, settings.filterResonance);
    engine_.setTrackSynthGainDb(index, settings.gainDb);
    engine_.setTrackSynthFilterEnvAmount(index, settings.filterEnvAmount);
    engine_.setTrackSynthFilterEnvAttackMs(index, settings.filterEnvAttackMs);
    engine_.setTrackSynthFilterEnvDecayMs(index, settings.filterEnvDecayMs);
    engine_.setTrackSynthFilterEnvSustain(index, settings.filterEnvSustain);
    engine_.setTrackSynthFilterEnvReleaseMs(index, settings.filterEnvReleaseMs);
    engine_.setTrackSynthSubOscEnabled(index, settings.subOscEnabled);
    engine_.setTrackSynthSubOscLevel(index, settings.subOscLevel);
    engine_.setTrackSynthUnisonVoices(index, settings.unisonVoices);
    engine_.setTrackSynthUnisonDetuneCents(index, settings.unisonDetuneCents);
}

/** Briefly sounds @p noteNumber through whichever track is currently armed —
    the same live-MIDI path the on-screen keyboard already uses (see
    InstrumentTrack::render's receivesLiveMidi routing), so it plays through
    that track's actual instrument: the synth pitch for an Instrument track,
    or the matching pad's sample for a Drum track. Fired when clicking to add
    a note in the piano roll, so pitches (or pads) can be found by ear. */
void MainComponent::previewNote(int noteNumber)
{
    engine_.keyboardState().noteOn(1, noteNumber, 0.8f);

    // Guarded by a SafePointer rather than capturing `this` directly: the
    // note-off fires 150ms later, and quitting the app within that window
    // would otherwise run this lambda against a destroyed MainComponent (and
    // a destroyed engine). A dangling preview is easy to trigger — click a
    // note, close the window — and would crash on the way out.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Timer::callAfterDelay(150, [safeThis, noteNumber]
    {
        if (auto* self = safeThis.getComponent())
            self->engine_.keyboardState().noteOff(1, noteNumber, 0.8f);
    });
}

/** Changes the bar length. Structural document state, so it goes through the
    history like a track's colour rather than being a live tweak like tempo —
    a bar length is part of the piece, not a knob you ride while listening.

    Everything that measures bars has to follow: the engine's metronome and
    count-in, the loop region, and the grids in the tracks and keys panes. */
void MainComponent::setTimeSignature(int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0)
        return;

    const auto& song = history_.current();
    if (song.timeSigNumerator == numerator && song.timeSigDenominator == denominator)
        return;

    history_.edit("Change time signature", [numerator, denominator](model::Song& s)
    {
        s.timeSigNumerator   = numerator;
        s.timeSigDenominator = denominator;
    });

    uiTempoMap_.setTimeSignature(numerator, denominator);
    post(Cmd::SetTimeSignature, (double) numerator, (double) denominator);

    updateTimeSignatureControls();
    updateLoopRegion();               // bars just changed length, so the loop did too
    pianoRoll_.setBeatsPerBar(beatsPerBar());
    arrangementView_.setSong(history_.current());

    showStatus("Time signature: " + juce::String(numerator) + "/" + juce::String(denominator));
}

/** Points the control at whatever the document says, without reporting it
    straight back as a user edit. */
void MainComponent::updateTimeSignatureControls()
{
    const auto& song = history_.current();

    for (int i = 0; i < kNumTimeSignatures; ++i)
    {
        if (kTimeSignatures[i].numerator == song.timeSigNumerator
            && kTimeSignatures[i].denominator == song.timeSigDenominator)
        {
            timeSigBox_.setSelectedId(i + 1, juce::dontSendNotification);
            return;
        }
    }

    // A signature loaded from a project that isn't in the list — show nothing
    // rather than a wrong one.
    timeSigBox_.setSelectedId(0, juce::dontSendNotification);
}

void MainComponent::updateEditingLabel()
{
    const auto& song = history_.current();

    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        editingLabel_.setText("No track selected", juce::dontSendNotification);
        return;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    const auto  name  = track.name.empty() ? ("Track " + juce::String(selectedTrackIndex_ + 1))
                                           : juce::String(track.name);

    juce::String text = "Editing: " + name;
    if (! track.clips.empty())
    {
        text << "   |   Clip " << (selectedClipIndex_ + 1) << " of " << (int) track.clips.size();

        if (selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) track.clips.size()
            && track.clips[(size_t) selectedClipIndex_].type == model::ClipType::Audio)
            text << "  (audio clip — not MIDI-editable)";
    }
    editingLabel_.setText(text, juce::dontSendNotification);
}

void MainComponent::updateMixerStrips()
{
    const auto& song = history_.current();

    // The buses anything may feed. Built once rather than per strip, and a bus
    // is excluded from its own list below — a bus feeding itself is a loop,
    // and a bus feeding another bus is not supported in this pass.
    std::vector<std::pair<int, juce::String>> buses;
    for (const auto& track : song.tracks)
        if (track.type == model::TrackType::Bus)
            buses.emplace_back(track.id, juce::String(track.name));

    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip  = trackStrips_[i];
        const bool active = i < (int) song.tracks.size();
        strip->setVisible(active);

        if (active)
        {
            const auto& track = song.tracks[(size_t) i];
            strip->setTrackName(track.name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(track.name));
            strip->setGainDb(track.gainDb);
            strip->setMuted(track.muted);
            strip->setSoloed(track.solo);
            strip->setSendLevel(track.sendLevel);
            strip->setPan(track.pan);

            if (track.type == model::TrackType::Bus)
            {
                // A bus always goes to the master here, so it gets no picker
                // rather than one offering a routing it cannot take.
                strip->setOutputOptions({}, -1);
            }
            else
            {
                strip->setOutputOptions(buses, track.outputBusId);
            }
        }
        strip->setSelected(i == selectedTrackIndex_);
    }

    layoutMixerView();
}

void MainComponent::updateDelayControls()
{
    const auto& d = history_.current().delay;
    delayButton.setToggleState(d.enabled, juce::dontSendNotification);
    delayTimeSlider.setValue(d.timeMs, juce::dontSendNotification);
    delayFbSlider.setValue(d.feedback * 100.0, juce::dontSendNotification);
    delayMixSlider.setValue(d.mix * 100.0, juce::dontSendNotification);

    engine_.setMasterDelayEnabled(d.enabled);
    engine_.setMasterDelayTimeMs(d.timeMs);
    engine_.setMasterDelayFeedback(d.feedback);
    engine_.setMasterDelayMix(d.mix);
}

void MainComponent::updateFilterControls()
{
    const auto& f = history_.current().filter;
    filterButton.setToggleState(f.enabled, juce::dontSendNotification);
    filterModeBox_.setSelectedId(f.mode + 1, juce::dontSendNotification);
    filterCutoffSlider.setValue(f.cutoff, juce::dontSendNotification);
    filterResoSlider.setValue(f.resonance, juce::dontSendNotification);

    engine_.setMasterFilterEnabled(f.enabled);
    engine_.setMasterFilterMode(f.mode);
    engine_.setMasterFilterCutoff(f.cutoff);
    engine_.setMasterFilterResonance(f.resonance);
}

void MainComponent::updateReverbControls()
{
    const auto& rv = history_.current().reverb;
    reverbButton.setToggleState(rv.enabled, juce::dontSendNotification);
    reverbRoomSlider.setValue(rv.roomSize * 100.0, juce::dontSendNotification);
    reverbDampSlider.setValue(rv.damping * 100.0, juce::dontSendNotification);
    reverbMixSlider.setValue(rv.mix * 100.0, juce::dontSendNotification);

    engine_.setMasterReverbEnabled(rv.enabled);
    engine_.setMasterReverbRoomSize(rv.roomSize);
    engine_.setMasterReverbDamping(rv.damping);
    engine_.setMasterReverbMix(rv.mix);
}

void MainComponent::updateEqControls()
{
    const auto& eq = history_.current().eq;
    eqButton.setToggleState(eq.enabled, juce::dontSendNotification);
    eqBassSlider.setValue(eq.bassDb, juce::dontSendNotification);
    eqMidSlider.setValue(eq.midDb, juce::dontSendNotification);
    eqTrebleSlider.setValue(eq.trebleDb, juce::dontSendNotification);

    engine_.setMasterEqEnabled(eq.enabled);
    engine_.setMasterEqBassDb(eq.bassDb);
    engine_.setMasterEqMidDb(eq.midDb);
    engine_.setMasterEqTrebleDb(eq.trebleDb);

    eqCurveView_.setSettings(eq);
}

void MainComponent::updateSendBusControls()
{
    const auto& sb = history_.current().sendBus;
    sendBusButton.setToggleState(sb.enabled, juce::dontSendNotification);
    sendEffectTypeBox_.setSelectedId(sb.effectType == model::SendBusEffectType::Delay ? 2 : 1,
                                     juce::dontSendNotification);
    sendRoomSlider.setValue(sb.roomSize * 100.0, juce::dontSendNotification);
    sendDampSlider.setValue(sb.damping * 100.0, juce::dontSendNotification);
    sendDelayTimeSlider.setValue(sb.delayTimeMs, juce::dontSendNotification);
    sendDelayFbSlider.setValue(sb.delayFeedback * 100.0, juce::dontSendNotification);
    sendReturnSlider.setValue(sb.returnLevel * 100.0, juce::dontSendNotification);
    updateSendBusEffectVisibility();

    engine_.setSendBusEnabled(sb.enabled);
    engine_.setSendBusEffectType((int) sb.effectType);
    engine_.setSendBusRoomSize(sb.roomSize);
    engine_.setSendBusDamping(sb.damping);
    engine_.setSendBusDelayTimeMs(sb.delayTimeMs);
    engine_.setSendBusDelayFeedback(sb.delayFeedback);
    engine_.setSendBusReturnLevel(sb.returnLevel);
}

void MainComponent::updateSendBusEffectVisibility()
{
    const bool isDelay = history_.current().sendBus.effectType == model::SendBusEffectType::Delay;
    sendRoomSlider.setVisible(! isDelay);
    sendDampSlider.setVisible(! isDelay);
    sendDelayTimeSlider.setVisible(isDelay);
    sendDelayFbSlider.setVisible(isDelay);
}

void MainComponent::setTrackGain(int index, float gainDb)
{
    // Live tweak: update the current document in place (not a separate undo step).
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.gainDb = gainDb;

        // The same global "Rec Auto" toggle arms every automatable per-track
        // parameter — touch whichever control you want to automate while it's
        // on (see also setTrackPan and setTrackSendLevel).
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::Gain)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), gainDb);
    }
    engine_.setTrackGainDb(index, gainDb);
}

/** Reads the value a fader controls, straight from the document. */
static float readFader(const model::Song& song, int index, MixerStrip::Fader fader)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return 0.0f;

    const auto& track = song.tracks[(size_t) index];
    switch (fader)
    {
        case MixerStrip::Fader::Gain: return track.gainDb;
        case MixerStrip::Fader::Pan:  return track.pan;
        case MixerStrip::Fader::Send: return track.sendLevel;
    }
    return 0.0f;
}

static void writeFader(model::Song& song, int index, MixerStrip::Fader fader, float value)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    auto& track = song.tracks[(size_t) index];
    switch (fader)
    {
        case MixerStrip::Fader::Gain: track.gainDb    = value; break;
        case MixerStrip::Fader::Pan:  track.pan       = value; break;
        case MixerStrip::Fader::Send: track.sendLevel = value; break;
    }
}

static const char* faderName(MixerStrip::Fader fader)
{
    switch (fader)
    {
        case MixerStrip::Fader::Gain: return "Set track gain";
        case MixerStrip::Fader::Pan:  return "Set track pan";
        case MixerStrip::Fader::Send: return "Set track send";
    }
    return "Set track level";
}

/** Remembers where a fader was when it was grabbed. */
void MainComponent::beginFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    faderDragTrack_ = trackIndex;
    faderDragWhich_ = fader;
    faderDragFrom_  = readFader(history_.current(), trackIndex, fader);
    faderDragging_  = true;
}

/** Turns a whole fader drag into one undo step.

    The live changes during the drag go through mutableCurrent, so the audio
    follows the fader without hundreds of snapshots. On release the document is
    rewound to where the drag started and the final value committed as a single
    edit — which is what leaves exactly one step on the stack for the whole
    gesture.

    Mute and solo don't need this: a click is already one edit. A fader is
    hundreds of values, and one step each would bury the last real edit under a
    drag. */
void MainComponent::endFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    if (! faderDragging_ || faderDragTrack_ != trackIndex || faderDragWhich_ != fader)
        return;

    faderDragging_ = false;

    const float landedOn = readFader(history_.current(), trackIndex, fader);
    commitDrag(history_, faderName(fader), faderDragFrom_, landedOn,
               [trackIndex, fader](model::Song& s, float v) { writeFader(s, trackIndex, fader, v); });
}

/** Remembers an effect slot's parameters before a drag on one of its controls
    started — see EffectChainPanel::onSlotParamsDragStart. */
void MainComponent::beginEffectSlotParamsDrag(int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    effectSlotDragging_  = true;
    effectSlotDragTrack_ = selectedTrackIndex_;
    effectSlotDragIndex_ = slotIndex;
    effectSlotDragFrom_  = chain[(size_t) slotIndex];
}

/** Commits a whole effect-slot-parameters drag as one undo step, the
    commitStructDrag equivalent of endFaderDrag above — a slot's parameters
    are a struct of several fields changed together, not one number, so
    there's no meaningful tolerance to check against: any real change
    commits, equality is the whole test. */
void MainComponent::endEffectSlotParamsDrag(int slotIndex)
{
    if (! effectSlotDragging_ || effectSlotDragTrack_ != selectedTrackIndex_ || effectSlotDragIndex_ != slotIndex)
        return;

    effectSlotDragging_ = false;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    const auto landedOn   = chain[(size_t) slotIndex];
    const int  trackIndex = selectedTrackIndex_;

    commitStructDrag(history_, "Set effect parameters", effectSlotDragFrom_, landedOn,
                     [trackIndex, slotIndex](model::Song& s, const model::EffectSlot& value)
    {
        auto& c = s.tracks[(size_t) trackIndex].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) c.size())
            c[(size_t) slotIndex] = value;
    });
}

/** Remembers a track's synth settings before a drag on one of the Synth
    pane's controls started — see SynthEditor::onSettingsDragStart. */
void MainComponent::beginSynthSettingsDrag()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    synthSettingsDragging_  = true;
    synthSettingsDragTrack_ = selectedTrackIndex_;
    synthSettingsDragFrom_  = history_.current().tracks[(size_t) selectedTrackIndex_].synthSettings;
}

/** Commits a whole synth-settings drag as one undo step — the
    commitStructDrag equivalent of endEffectSlotParamsDrag above. */
void MainComponent::endSynthSettingsDrag()
{
    if (! synthSettingsDragging_ || synthSettingsDragTrack_ != selectedTrackIndex_)
        return;

    synthSettingsDragging_ = false;

    const int  trackIndex = selectedTrackIndex_;
    const auto landedOn   = history_.current().tracks[(size_t) trackIndex].synthSettings;

    commitStructDrag(history_, "Set synth settings", synthSettingsDragFrom_, landedOn,
                     [trackIndex](model::Song& s, const model::SynthSettings& value)
    {
        s.tracks[(size_t) trackIndex].synthSettings = value;
    });
}

/** Remembers the selected clip's gain before a drag on the audio editor's
    gain slider started, so the whole drag lands as one undo step rather than
    one per mouse-move — the same pair, for the same reason, as the synth and
    guitar settings drags. */
void MainComponent::beginClipGainDrag()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    clipGainDragging_   = true;
    clipGainDragTrack_  = selectedTrackIndex_;
    clipGainDragClip_   = selectedClipIndex_;
    clipGainDragFrom_   = clip->gainDb;
}

void MainComponent::endClipGainDrag()
{
    if (! clipGainDragging_
        || clipGainDragTrack_ != selectedTrackIndex_
        || clipGainDragClip_ != selectedClipIndex_)
        return;

    clipGainDragging_ = false;

    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const int   trackIndex = clipGainDragTrack_;
    const int   clipIndex  = clipGainDragClip_;
    const float landedOn   = clip->gainDb;

    commitStructDrag(history_, "Set clip gain", clipGainDragFrom_, landedOn,
                     [trackIndex, clipIndex](model::Song& s, const float& value)
    {
        s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].gainDb = value;
    });
}

/** Remembers a track's guitar settings before a drag on one of the
    fretboard's controls started — see FretboardPane::onSettingsDragStart. */
void MainComponent::beginGuitarSettingsDrag()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    guitarSettingsDragging_  = true;
    guitarSettingsDragTrack_ = selectedTrackIndex_;
    guitarSettingsDragFrom_  = history_.current().tracks[(size_t) selectedTrackIndex_].guitarSettings;
}

/** Commits a whole guitar-settings drag as one undo step — the
    commitStructDrag equivalent of endSynthSettingsDrag above. */
void MainComponent::endGuitarSettingsDrag()
{
    if (! guitarSettingsDragging_ || guitarSettingsDragTrack_ != selectedTrackIndex_)
        return;

    guitarSettingsDragging_ = false;

    const int  trackIndex = selectedTrackIndex_;
    const auto landedOn   = history_.current().tracks[(size_t) trackIndex].guitarSettings;

    commitStructDrag(history_, "Set guitar settings", guitarSettingsDragFrom_, landedOn,
                     [trackIndex](model::Song& s, const model::GuitarSettings& value)
    {
        s.tracks[(size_t) trackIndex].guitarSettings = value;
    });
}

/** Mutes or unmutes a track, as an undoable edit.

    Mute and solo go through the history where gain, pan and send level do
    not, and the difference is that these two are discrete. A click is one
    edit, so it makes one undo step. A fader is a drag of hundreds of values,
    and putting each on the stack would bury the last real edit under a
    hundred nudges — those stay live tweaks until there is somewhere to
    coalesce a whole drag into a single step.

    Undo reaches the audio as well as the document: refreshFromModel runs
    syncEngineTracks, which pushes every track's mute and solo back to the
    engine. Without that an undone mute would restore the checkbox and leave
    the track silent. */
void MainComponent::setTrackMuted(int index, bool muted)
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    if (song.tracks[(size_t) index].muted == muted)
        return; // nothing changed, so nothing worth an undo step

    history_.edit(muted ? "Mute track" : "Unmute track", [index, muted](model::Song& s)
    {
        s.tracks[(size_t) index].muted = muted;
    });

    engine_.setTrackMuted(index, muted);

    // Both views show mute, and either can set it, so both are refreshed from
    // the document here rather than by whichever one happened to be clicked.
    // Without this the tracks pane muted the audio and left its own icon
    // unchanged — indistinguishable from a button that does nothing.
    // MixerStrip::setMuted uses dontSendNotification, so this can't echo back.
    arrangementView_.setSong(history_.current());
    updateMixerStrips();
}

/** Solos or unsolos a track. Undoable for the same reason as mute — see
    setTrackMuted, which explains why the continuous controls are not. */
void MainComponent::setTrackSolo(int index, bool solo)
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    if (song.tracks[(size_t) index].solo == solo)
        return;

    history_.edit(solo ? "Solo track" : "Unsolo track", [index, solo](model::Song& s)
    {
        s.tracks[(size_t) index].solo = solo;
    });

    engine_.setTrackSolo(index, solo);
    updateMixerStrips();
}

/** Routes a track into a group bus, or back to the master (@p busTrackId -1).

    A real undo step rather than an in-place edit like the faders: this is a
    structural change to the mix, not a continuous control being dragged, and
    it is the kind of thing you want to be able to take back. */
void MainComponent::setTrackOutputBus(int index, int busTrackId)
{
    if (index < 0 || index >= trackCount())
        return;

    if (history_.current().tracks[(size_t) index].outputBusId == busTrackId)
        return; // repopulating the picker must not manufacture an undo step

    history_.edit("Route track", [index, busTrackId](model::Song& s)
    {
        if (index >= 0 && index < (int) s.tracks.size())
            s.tracks[(size_t) index].outputBusId = busTrackId;
    });

    syncEngineTracks();
    updateMixerStrips();
}

void MainComponent::setTrackPan(int index, float pan)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.pan = pan;
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::Pan)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), pan);
    }
    engine_.setTrackPan(index, pan);
}

void MainComponent::setTrackSendLevel(int index, float level)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.sendLevel = level;
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::SendLevel)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), level);
    }
    engine_.setTrackSendLevel(index, level);
}

void MainComponent::selectTrack(int index)
{
    // A mixer-strip click doesn't know about specific clips, so it defaults to
    // the track's first one.
    selectTrackAndClip(index, 0);
}

void MainComponent::selectTrackAndClip(int trackIndex, int clipIndex)
{
    if (trackIndex < 0 || trackIndex >= trackCount())
        return;

    selectedTrackIndex_ = trackIndex;
    selectedClipIndex_  = clipIndex;
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    updateMixerStrips(); // refreshes the selection highlight
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

void MainComponent::refreshFromModel()
{
    // The song's metre drives both tempo maps: the UI's (bar/beat readout,
    // bars-to-beats for pattern lengths) and the engine's (which decides
    // where the metronome's downbeat accent falls). Neither was ever told,
    // so both sat at 4/4 no matter what the document said.
    const auto& song = history_.current();
    uiTempoMap_.setTimeSignature(song.timeSigNumerator, song.timeSigDenominator);
    post(Cmd::SetTimeSignature, (double) song.timeSigNumerator, (double) song.timeSigDenominator);
    updateTimeSignatureControls();
    pianoRoll_.setBeatsPerBar(beatsPerBar());

    if (selectedTrackIndex_ >= trackCount())
        selectedTrackIndex_ = juce::jmax(0, trackCount() - 1);

    const int clipCount = (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount())
                             ? (int) history_.current().tracks[(size_t) selectedTrackIndex_].clips.size()
                             : 0;
    if (selectedClipIndex_ >= clipCount)
        selectedClipIndex_ = juce::jmax(0, clipCount - 1);

    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();
    updateEqControls();
    updateMasteringControls();
    updateSendBusControls();
    fileBrowser_.setProjectRootFolder(history_.current().projectRootFolder.empty()
                                          ? juce::File{}
                                          : juce::File(history_.current().projectRootFolder));
    updateEditingLabel();
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

void MainComponent::chooseFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load an audio file", juce::File{},
                                                   audiofiles::wildcards());

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File{})
            previewAudioFile(file);
    });
}

/** Loads a file into the global preview player (not tied to any track) — used
    by the "Preview Audio File..." menu item and by double-clicking a file in
    the file-browser pane.

    Named "Preview" rather than "Import" because that is what it does: the
    file plays once and never becomes a clip. While both menu items said
    "Import Audio", picking this one looked like importing and produced a
    project with nothing in it. */
void MainComponent::previewAudioFile(const juce::File& file)
{
    if (engine_.loadAudioFile(file))
        clipLabel.setText(engine_.loadedClipName()
                              + juce::String::formatted("   (%.2f s)", engine_.loadedClipSeconds()),
                          juce::dontSendNotification);
    else
        showError("Could not load: " + file.getFileName());
}

void MainComponent::importMidiFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import MIDI file", juce::File{}, "*.mid;*.midi");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        engine::MidiImportResult result;
        history_.edit("Import MIDI", [&file, &result](model::Song& s)
        {
            result = engine::importMidiFile(file, s);
        });

        if (! result.ok)
        {
            showError("Could not import: " + file.getFileName());
            return;
        }

        syncEngineTracks();
        arrangementView_.setSong(history_.current());
        updateMixerStrips();
        updateEditingLabel();

        auto msg = "Imported " + juce::String(result.tracksImported) + " track(s) at "
                 + juce::String(history_.current().bpm, 1) + " BPM";
        if (result.extraTempoEventsIgnored > 0)
            msg += " (" + juce::String(result.extraTempoEventsIgnored) + " further tempo change(s) not imported)";
        showStatus(msg);
    });
}

void MainComponent::exportMidiFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export MIDI file", juce::File{}, "*.mid");
    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;
        file = file.withFileExtension("mid");

        const bool ok = engine::exportMidiFile(file, history_.current());
        if (ok)
            showStatus("Exported: " + file.getFileName());
        else
            showError("MIDI export failed (no instrument track has any notes)");
    });
}

void MainComponent::setProjectRootFolderDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Set project root folder", juce::File{});
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir == juce::File{} || ! dir.isDirectory())
            return;

        const auto path = dir.getFullPathName().toStdString();
        history_.edit("Set project root folder", [path](model::Song& s) { s.projectRootFolder = path; });

        fileBrowser_.setProjectRootFolder(dir);
        showStatus("Project root folder set to: " + dir.getFullPathName());
    });
}

/** One-time fix-up for projects saved while recorded takes were always given
    a flat four beats regardless of how long the take actually ran (see
    finishRecordingIfReady). Re-measures every audio clip against its file and
    corrects any that disagree — see ClipLengthRepair.h for why "disagrees
    with its file" rather than "is exactly four beats" is the right test. */
void MainComponent::repairRecordedClipLengths()
{
    auto probe = [this](const std::string& path) -> double
    {
        const juce::File file(path);
        return file.existsAsFile() ? engine_.probeDurationSeconds(file) : 0.0;
    };

    // A dry run on a copy first, so nothing is added to the undo stack when
    // there is nothing to fix.
    auto        dryRun = history_.current();
    const auto  fixes  = repairAudioClipLengths(dryRun, probe);

    if (fixes.empty())
    {
        showStatus("No recorded clips needed a length fix");
        return;
    }

    history_.edit("Repair recorded clip lengths", [probe](model::Song& s)
    {
        repairAudioClipLengths(s, probe);
    });

    refreshFromModel();
    showStatus(juce::String((int) fixes.size())
               + (fixes.size() == 1 ? " clip length was fixed" : " clip lengths were fixed"));
}

/** Imports an audio file onto a brand-new Audio track (as its one clip, at
    beat 0), so it actually plays back as part of the mix — unlike "Import
    Audio..." above, which only feeds the disconnected global preview player. */
void MainComponent::importAudioToNewTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser>("Import audio to a new track", juce::File{},
                                                   audiofiles::wildcards());
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File{})
            importAudioFileAtBeat(file, 0.0);
    });
}

/** Imports a file as an audio clip starting at @p startBeats — the shared
    machinery behind "Import Audio to Track..." (always beat 0, always a new
    track), and dragging a file from the file-browser pane onto the
    arrangement (beat = wherever it was dropped; @p targetTrackIndex = the
    track lane it landed on, or -1 for empty space below the tracks).

    Dropping onto an existing Audio-type track adds a clip there instead of
    creating a new track — the track keeps its single-clip unbounded window
    if it still only has one clip, or gets real per-clip length gating (see
    AudioFilePlayerNode) the moment it has more than one, exactly like
    instrument clips. Any other drop target (empty space, or a non-Audio
    track) creates a brand-new Audio track instead, as it always has. */
void MainComponent::importAudioFileAtBeat(const juce::File& file, double startBeats,
                                          int targetTrackIndex, bool isRecordedTake)
{
    const auto& song = history_.current();
    const bool  addToExistingTrack = targetTrackIndex >= 0 && targetTrackIndex < (int) song.tracks.size()
                                   && song.tracks[(size_t) targetTrackIndex].type == model::TrackType::Audio;

    // Size the clip to the file's real duration rather than a fixed guess —
    // display-only for a track's sole clip (unbounded window regardless), but
    // functionally gates playback the moment a track has more than one clip,
    // so guessing wrong there would audibly truncate the clip.
    const double durationSeconds = engine_.probeDurationSeconds(file);
    if (durationSeconds <= 0.0)
    {
        // The file couldn't be decoded at all — corrupt, truncated, or an
        // unsupported format. Falling through to a fabricated 4-beat clip
        // pointing at a file the engine can't play would create a track (or
        // clip) that just sits there silent with nothing to say why.
        showError("Could not import: " + file.getFileName());
        return;
    }
    const double measured        = engine::beatsForSeconds(durationSeconds, song.bpm);
    const double lengthBeats     = measured > 0.0 ? measured : 4.0;
    const auto   path            = file.getFullPathName().toStdString();

    // Tempo detection, which is what makes a dropped loop actually usable:
    // without it every import plays at whatever tempo it was recorded at,
    // against everything else in the project.
    //
    // Warping is switched on only when the detector is confident *and* the
    // tempo genuinely differs. A low-confidence result is still stored - it
    // costs nothing, and it means "Warp Clip to Project Tempo" is available to
    // accept by hand - but it is not acted on, because a sustained pad that
    // happens to correlate at 91 BPM must not be silently stretched. Either
    // way the status line says what happened, so warping is never a mystery.
    const double projectBpm = model::tempoAtBeat(song, juce::jmax(0.0, startBeats));

    // A recorded take is at the project tempo by definition — it was just
    // played against this project's click. Its tempo is recorded as such
    // (which makes it usable later, e.g. if the project tempo changes) but it
    // is never analysed and never warped on arrival.
    if (isRecordedTake)
    {
        const double takeBpm = projectBpm;

        if (addToExistingTrack)
        {
            int newClipIndex = -1;
            history_.edit("Add audio clip", [targetTrackIndex, &path, startBeats, lengthBeats,
                                             takeBpm, &newClipIndex](model::Song& s)
            {
                auto& track = s.tracks[(size_t) targetTrackIndex];

                model::Clip clip;
                clip.id          = model::allocateId(s);
                clip.type        = model::ClipType::Audio;
                clip.startBeats  = juce::jmax(0.0, startBeats);
                clip.lengthBeats = lengthBeats;
                clip.audioFile   = path;
                clip.sourceBpm   = takeBpm;
                track.clips.push_back(clip);

                newClipIndex = (int) track.clips.size() - 1;
            });

            syncEngineTracks();
            selectTrackAndClip(targetTrackIndex, newClipIndex);
            arrangementView_.setSong(history_.current());
            return;
        }

        if (trackCount() >= engine_.maxTracks())
        {
            showError("Track limit reached");
            return;
        }

        int newTrackIndex = -1;
        history_.edit("Import audio track", [&path, &newTrackIndex, startBeats, lengthBeats,
                                             takeBpm](model::Song& s)
        {
            const auto name = "Audio " + juce::String((int) s.tracks.size() + 1);
            model::addTrack(s, model::TrackType::Audio, name.toStdString());

            model::Clip clip;
            clip.id          = model::allocateId(s);
            clip.type        = model::ClipType::Audio;
            clip.startBeats  = juce::jmax(0.0, startBeats);
            clip.lengthBeats = lengthBeats;
            clip.audioFile   = path;
            clip.sourceBpm   = takeBpm;
            s.tracks.back().clips.push_back(clip);

            newTrackIndex = (int) s.tracks.size() - 1;
        });

        selectTrackAndRefreshAll(newTrackIndex);
        return;
    }

    // Decoding the whole file to analyse it is not instant, and this is
    // reached by a drag-and-drop, where an unexplained pause reads as a hang.
    showBusy("Analysing tempo...");
    const auto estimate = engine_.detectFileTempo(file);

    const bool tempoDiffers = estimate.isUsable() && projectBpm > 0.0
                           && std::abs(estimate.bpm - projectBpm) > 0.5;
    const bool autoWarp     = tempoDiffers && estimate.confidence >= 0.5;

    const double detectedBpm = estimate.isUsable() ? estimate.bpm : 0.0;

    juce::String tempoNote;
    if (autoWarp)
        tempoNote = "  (" + juce::String(detectedBpm, 1) + " BPM, warped to "
                  + juce::String(projectBpm, 1) + ")";
    else if (estimate.isUsable() && tempoDiffers)
        tempoNote = "  (" + juce::String(detectedBpm, 1) + " BPM? - low confidence, not warped)";
    else if (estimate.isUsable())
        tempoNote = "  (" + juce::String(detectedBpm, 1) + " BPM)";

    if (addToExistingTrack)
    {
        int newClipIndex = -1;
        history_.edit("Add audio clip", [targetTrackIndex, &path, startBeats, lengthBeats,
                                         detectedBpm, autoWarp, &newClipIndex](model::Song& s)
        {
            auto& track = s.tracks[(size_t) targetTrackIndex];

            model::Clip clip;
            clip.id          = model::allocateId(s);
            clip.type        = model::ClipType::Audio;
            clip.startBeats  = juce::jmax(0.0, startBeats);
            clip.lengthBeats = lengthBeats;
            clip.audioFile   = path;
            clip.sourceBpm   = detectedBpm;
            clip.warpEnabled = autoWarp;
            track.clips.push_back(clip);

            newClipIndex = (int) track.clips.size() - 1;
        });

        syncEngineTracks();
        selectTrackAndClip(targetTrackIndex, newClipIndex);
        arrangementView_.setSong(history_.current());
        showStatus("Imported: " + file.getFileName() + "  (added clip)" + tempoNote);
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    int newTrackIndex = -1;
    history_.edit("Import audio track", [&path, &newTrackIndex, startBeats, lengthBeats,
                                        detectedBpm, autoWarp](model::Song& s)
    {
        const auto name = "Audio " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Audio, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(s);
        clip.type        = model::ClipType::Audio;
        clip.startBeats  = juce::jmax(0.0, startBeats);
        clip.lengthBeats = lengthBeats;
        clip.audioFile   = path;
        clip.sourceBpm   = detectedBpm;
        clip.warpEnabled = autoWarp;
        s.tracks.back().clips.push_back(clip);

        newTrackIndex = (int) s.tracks.size() - 1;
    });

    selectTrackAndRefreshAll(newTrackIndex);
    showStatus("Imported: " + file.getFileName() + "  (new track)" + tempoNote);
}

/** Points the whole UI at a track: every pane that shows per-track state is
    refreshed from it. Used after adding a track and after deleting one, which
    is why it isn't named for either. */
void MainComponent::selectTrackAndRefreshAll(int newTrackIndex)
{
    if (newTrackIndex < 0)
        return;

    selectedTrackIndex_ = newTrackIndex;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Toggles between arming/starting a take and stopping it.

    The take streams straight to its file as it is played, so the destination
    and the track it will land on are both decided *here*, at arm time. They
    used to be decided when the take ended — which meant every recording became
    a new track at bar 1, however the project was set up when you hit record. */
app::RecordSource MainComponent::chooseRecordSource(int trackIndex, juce::String& explanation) const
{
    const auto& song = history_.current();

    // Instrument, Drum and Guitar are all driven by MIDI clips, so all three
    // can hold a recorded pattern; only an Audio track cannot.
    const bool trackHoldsMidi = trackIndex >= 0 && trackIndex < (int) song.tracks.size()
                             && song.tracks[(size_t) trackIndex].type != model::TrackType::Audio;

    // The decision itself lives in app/RecordSourceChoice.h, where the whole
    // table is enumerated and tested — this function only gathers the inputs
    // and turns the reason into something worth reading.
    const auto decision = app::chooseRecordSource(trackHoldsMidi,
                                                  engine_.hasMidiInput(),
                                                  engine_.hasAudioInput(),
                                                  engine_.inputOpenError().isNotEmpty());

    switch (decision.reason)
    {
        case app::RecordSourceReason::Ok:
            break;

        case app::RecordSourceReason::FallbackToAudioNoMidi:
            explanation = "No MIDI input connected - recording audio to a new track instead";
            break;

        case app::RecordSourceReason::NoAudioInput:
            explanation = "No audio input device to record from";
            break;

        case app::RecordSourceReason::NoAudioInputPermission:
            explanation = "No audio input - check microphone permission "
                          "(System Settings > Privacy & Security > Microphone), then restart";
            break;

        case app::RecordSourceReason::NothingConnected:
            explanation = "Nothing to record from - connect a MIDI controller, or an audio "
                          "input (and check microphone permission)";
            break;
    }

    return decision.source;
}

bool MainComponent::ensureMicrophoneAccess()
{
    const auto status = app::microphonePermission();

    if (status == app::MicPermission::NotRequired)
        return true; // no permission model here; a missing device is a different report

    if (status == app::MicPermission::Granted)
    {
        // Granted, but possibly *after* the input was opened at startup — in
        // which case the engine is still running output-only and would report
        // "no audio input" with the microphone switched on. Re-ask once here
        // rather than telling anyone to restart the app.
        if (engine_.hasAudioInput())
            return true;

        if (engine_.reopenAudioInput())
            return true;

        showError("Microphone access is on, but no audio input device could be opened - "
                  "check the input device in Audio Settings");
        return false;
    }

    if (status == app::MicPermission::NotDetermined)
    {
        // The OS has never asked — usually because its one prompt appeared at
        // launch, before anyone had a reason to care, and was dismissed. This
        // is the moment it actually means something, so ask now.
        showStatus("Waiting for microphone permission...");

        app::requestMicrophonePermission(
            [self = juce::Component::SafePointer<MainComponent>(this)](bool granted)
        {
            if (self == nullptr)
                return; // the window went away while the prompt was up

            if (! granted)
            {
                self->showError("Microphone access denied - recording audio is not possible "
                                "until it is enabled in System Settings");
                return;
            }

            // Granted: the device was opened without input at startup, so it
            // has to be re-opened before anything can be captured.
            self->engine_.reopenAudioInput();

            // Pick up exactly where the user left off — they pressed Record,
            // and answering a permission prompt should not mean pressing it
            // again. Guarded so a grant that still yields no input reports
            // that rather than looping back here.
            if (self->retryingAfterMicPermission_)
                return;

            self->retryingAfterMicPermission_ = true;
            self->toggleRecording();
            self->retryingAfterMicPermission_ = false;
        });

        return false; // the prompt owns this press now
    }

    // Denied, or restricted by policy. The OS will not prompt again no matter
    // what this app does, so the only useful thing left is to take the user
    // straight to the setting instead of describing where it lives.
    juce::NativeMessageBox::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Microphone access is off",
        "Looper-Audio needs microphone access to record audio.\n\n"
        "macOS will not ask again, so it has to be switched on in System Settings > "
        "Privacy & Security > Microphone. Recording will work as soon as it is on - "
        "no need to restart.",
        this,
        juce::ModalCallbackFunction::create([](int result)
        {
            if (result == 1) // Open Settings
                app::openMicrophonePrivacySettings();
        }));

    return false;
}

void MainComponent::toggleRecording()
{
    // Stopping always goes back to whichever take is actually running — the
    // sources are re-examined only when starting one.
    if (awaitingMidiTake_)
    {
        toggleMidiRecording();
        return;
    }

    if (! awaitingRecordedTake_)
    {
        // Devices are re-scanned here rather than trusted from startup: a
        // controller plugged in after launch is extremely common, and before
        // this it was invisible to the app for the whole session.
        engine_.refreshMidiInputs();

        juce::String explanation;
        auto         source = chooseRecordSource(selectedTrackIndex_, explanation);

        // A MIDI take needs no microphone, so it is decided before any
        // permission question — prompting a controller user for microphone
        // access would be a non-sequitur.
        if (source == app::RecordSource::Midi)
        {
            toggleMidiRecording();
            return;
        }

        // Everything else wants audio — *including* the "nothing connected"
        // answer, which is exactly what a blocked microphone looks like from
        // here, since a denied permission shows up as a device with no input
        // channels. So permission is settled before that answer is treated as
        // final; otherwise a one-click fix gets reported as missing hardware.
        if (! ensureMicrophoneAccess())
            return; // prompting, or already explained

        // Asked again, because granting access can have just opened an input
        // and turned None into Audio (and because a controller may have been
        // plugged in while a prompt was up).
        explanation.clear();
        source = chooseRecordSource(selectedTrackIndex_, explanation);

        if (source == app::RecordSource::None)
        {
            showError(explanation);
            return;
        }

        if (source == app::RecordSource::Midi)
        {
            toggleMidiRecording();
            return;
        }

        if (explanation.isNotEmpty())
            showStatus(explanation); // audio, but not from the armed track
    }

    if (! awaitingRecordedTake_)
    {
        const auto file = recordingsDirectory().getNonexistentChildFile("Recording", ".wav");

        if (! engine_.beginRecording(file))
        {
            // Names microphone permission first when that is what actually
            // happened, rather than making the user guess between three
            // possible causes. No longer says "then restart": the input is
            // re-opened on the next Record press (see ensureMicrophoneAccess),
            // so a restart has stopped being part of the fix.
            if (engine_.inputOpenError().isNotEmpty())
                showError("No audio input - check microphone permission "
                          "(System Settings > Privacy & Security > Microphone)");
            else
                showError("Could not start recording (no audio input device, "
                          "or the file could not be created)");
            return;
        }

        recordingFile_ = file;

        // Onto the selected track if it can hold audio, otherwise a new one.
        // A Guitar or Synth track can't take an audio clip, so recording while
        // one is selected has to mean "somewhere else" rather than fail.
        const auto& song = history_.current();
        const bool  canHoldAudio = selectedTrackIndex_ >= 0
                                && selectedTrackIndex_ < (int) song.tracks.size()
                                && song.tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Audio;
        recordingTargetTrack_ = canHoldAudio ? selectedTrackIndex_ : -1;

        awaitingRecordedTake_ = true;
        recordButton.setToggleState(true, juce::dontSendNotification); // swaps to the stop square
        recordButton.setTooltip(withShortcut("Stop recording", keys::record));

        // The transport runs free for the length of a take. Looping would wrap
        // it at the end of what is already arranged, which is precisely where
        // a recording needs to keep going — you are recording the part that
        // isn't there yet. The button's own state is left alone and restored
        // when the take ends, so the user's setting survives.
        post(Cmd::SetLooping, 0.0);
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopRecording();
        post(Cmd::SetPlaying, 0.0);
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0); // whatever it was before
        recordButton.setToggleState(false, juce::dontSendNotification); // back to the record disc
        recordButton.setTooltip(withShortcut("Record", keys::record));
    }
}

void MainComponent::finishRecordingIfReady()
{
    if (! awaitingRecordedTake_ || ! engine_.isRecordingFinished())
        return;
    awaitingRecordedTake_ = false;

    // Every ending passes through here — the stop button, play/pause during a
    // take, or the engine finishing on its own — so this is where looping is
    // put back. Restoring it only in the stop button's handler would leave
    // loop silently off after any other route out, including an empty take
    // that returns just below.
    post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);

    const int64_t dropped   = engine_.recordedDroppedSamples();
    const int64_t startedAt = engine_.recordedTakeStartSample();

    // Closes the file and hands it over; empty means nothing was captured.
    const auto file = engine_.finishRecordedTake();
    if (file == juce::File{})
    {
        showError("Recording was empty (no input captured)");
        return;
    }

    // Where the take goes on the timeline: where the transport actually was
    // when capture began, which is after any count-in. Falls back to the start
    // only if the engine never reported a position.
    const double startBeats = startedAt >= 0
                                ? juce::jmax(0.0, uiTempoMap_.ppqFromSamples(startedAt))
                                : 0.0;

    // The clip's length, undo, and selection all come from the existing import
    // path — which measures the file's real duration rather than guessing, and
    // appends to the target track rather than always making a new one. This
    // used to be a second, hand-written copy of that logic here.
    importAudioFileAtBeat(file, startBeats, recordingTargetTrack_, /*isRecordedTake=*/true);

    recordingFile_        = juce::File{};
    recordingTargetTrack_ = -1;

    // Reported after the import, so the take is on the timeline either way —
    // a recording with a gap is still worth keeping, it just must not be
    // presented as a clean one.
    if (dropped > 0)
    {
        showError("Recorded with gaps — the disk could not keep up ("
                  + juce::String((int) dropped) + " samples lost)");
        return;
    }

    // A take of pure digital silence means the input device handed us zeros
    // for its whole length, which is a different failure from "no input
    // device" and used to be reported as a success: the track appeared, the
    // status bar said "Recorded:", and only playing it back revealed nothing
    // was there. On macOS the usual cause is microphone permission — the OS
    // grants none and CoreAudio delivers zeros rather than an error — so the
    // message names that first.
    if (isSilentAudioFile(file))
    {
        showError("Recorded silence — check microphone permission "
                  "(System Settings > Privacy & Security > Microphone) and the input device");
        return;
    }

    showStatus("Recorded: " + file.getFileName());
}

/** Starts or stops a MIDI take. The mirror of toggleRecording's audio path,
    and deliberately the same shape — the transport handling, the button state
    and the looping-off rule are identical, because they are the same
    behaviours for the same reasons. What differs is only which recorder is
    armed and that there is no file to open, so this cannot fail: a controller
    that is absent simply sends nothing, which is an empty take rather than an
    error. */
void MainComponent::toggleMidiRecording()
{
    if (! awaitingMidiTake_)
    {
        midiTakeEvents_.clear();
        midiRecordingTargetTrack_ = selectedTrackIndex_;

        engine_.beginMidiRecording();
        awaitingMidiTake_ = true;

        recordButton.setToggleState(true, juce::dontSendNotification);
        recordButton.setTooltip(withShortcut("Stop recording", keys::record));

        // Looping off for the length of a take, restored when it ends — the
        // same reasoning as the audio path: looping would wrap the transport
        // at the end of what is already arranged, which is precisely where a
        // recording needs to keep going.
        post(Cmd::SetLooping, 0.0);
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopMidiRecording();
        post(Cmd::SetPlaying, 0.0);
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        recordButton.setToggleState(false, juce::dontSendNotification);
        recordButton.setTooltip(withShortcut("Record", keys::record));
    }
}

void MainComponent::finishMidiRecordingIfReady()
{
    if (! awaitingMidiTake_)
        return;

    // Drained every tick, take finished or not: this is what keeps the
    // engine's ring from having to hold a whole take (see engine::MidiRecorder
    // for why that matters — a fixed ring sized for a take is a silent cap).
    engine_.drainMidiTake(midiTakeEvents_);

    if (! engine_.isMidiRecordingFinished())
        return;
    awaitingMidiTake_ = false;

    // Every ending passes through here, so this is where looping is put back
    // — restoring it only in the stop handler would leave it silently off
    // after any other route out, including the empty take that returns below.
    post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);

    const int64_t dropped   = engine_.midiRecordedDroppedEvents();
    const int64_t startedAt = engine_.midiTakeStartSample();
    const int64_t endedAt   = engine_.midiTakeEndSample();
    const int     target    = midiRecordingTargetTrack_;

    midiRecordingTargetTrack_ = -1;

    if (midiTakeEvents_.empty() || startedAt < 0)
    {
        midiTakeEvents_.clear();
        showError("Recording was empty (no MIDI input captured)");
        return;
    }

    commitMidiTake(target, startedAt, endedAt);
    midiTakeEvents_.clear();

    // Reported after the commit, so the take is on the timeline either way —
    // a take missing a note is still worth keeping, it just must not be
    // presented as a clean one.
    if (dropped > 0)
        showError("Recorded with gaps — " + juce::String((int) dropped)
                  + " MIDI event(s) were lost");
}

void MainComponent::commitMidiTake(int targetTrack, int64_t startSample, int64_t endSample)
{
    if (targetTrack < 0 || targetTrack >= trackCount())
        return;

    // Samples to beats happens here, on the message thread, through the same
    // tempo map the UI already reads — which is why engine::MidiCapture takes
    // beats and knows nothing about tempo: with a tempo map, a take spanning a
    // tempo change cannot be converted by one scalar, and this is the only
    // place that has the map.
    const double takeStartBeats = juce::jmax(0.0, uiTempoMap_.ppqFromSamples(startSample));
    const double takeEndBeats   = endSample > startSample
                                    ? juce::jmax(takeStartBeats, uiTempoMap_.ppqFromSamples(endSample))
                                    : takeStartBeats;

    std::vector<engine::TimedMidiEvent> timed;
    timed.reserve(midiTakeEvents_.size());
    for (const auto& event : midiTakeEvents_)
    {
        engine::TimedMidiEvent converted;
        // Relative to the take's own start: a clip's notes are positioned from
        // the clip start, and the clip is placed at takeStartBeats below.
        converted.beats      = uiTempoMap_.ppqFromSamples(event.timeSamples) - takeStartBeats;
        converted.noteNumber = event.noteNumber;
        converted.velocity   = event.velocity;
        converted.noteOn     = event.noteOn;
        timed.push_back(converted);
    }

    auto notes = engine::MidiCapture::notesFromEvents(std::move(timed),
                                                      takeEndBeats - takeStartBeats);
    if (notes.empty())
    {
        // Every captured event was an unmatched note-off — keys that were
        // already down when capture began. Nothing was actually played.
        showError("Recording was empty (no MIDI input captured)");
        return;
    }

    // The clip is as long as the take, rounded up to a whole bar: a take is a
    // musical phrase, and ending the clip on the last note's release would
    // make a loop of it jarringly short.
    double contentEnd = takeEndBeats - takeStartBeats;
    for (const auto& note : notes)
        contentEnd = juce::jmax(contentEnd, note.startBeats + note.lengthBeats);

    const double lengthBeats = engine::MidiCapture::clipLengthForTake(
        contentEnd, juce::jmax(1.0, uiTempoMap_.quartersPerBar()));

    const int noteCount = (int) notes.size();
    int       newClipIndex = -1;

    history_.edit("Record MIDI", [&](model::Song& s)
    {
        if (targetTrack < 0 || targetTrack >= (int) s.tracks.size())
            return;
        auto& track = s.tracks[(size_t) targetTrack];

        model::Clip clip;
        clip.id                  = model::allocateId(s);
        clip.type                = model::ClipType::Instrument;
        clip.startBeats          = takeStartBeats;
        clip.lengthBeats         = lengthBeats;
        clip.pattern.lengthBeats = lengthBeats;
        clip.pattern.notes       = std::move(notes);

        track.clips.push_back(clip);
        newClipIndex = (int) track.clips.size() - 1;
    });

    if (newClipIndex < 0)
        return;

    // Open the take in the piano roll, the way Add Clip opens the clip it
    // made: the first thing anyone does with a recorded part is look at it.
    selectedTrackIndex_ = targetTrack;
    selectedClipIndex_  = newClipIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();

    showStatus("Recorded " + juce::String(noteCount) + " note(s)");
}

/** True if every sample in @p file is exactly zero.

    Reads the written file rather than a buffer held in memory, because with
    recording streamed to disk there is no such buffer any more — and reading
    back what was actually written is the stronger check anyway. */
bool MainComponent::isSilentAudioFile(const juce::File& file)
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(manager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false; // unreadable is a different problem, and not this one to report

    const int numChannels = juce::jmax(1, (int) reader->numChannels);
    std::vector<juce::Range<float>> levels((size_t) numChannels);
    reader->readMaxLevels(0, reader->lengthInSamples, levels.data(), numChannels);

    for (const auto& range : levels)
        if (range.getStart() != 0.0f || range.getEnd() != 0.0f)
            return false;

    return true;
}

juce::File MainComponent::recordingsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Recordings");
    dir.createDirectory();
    return dir;
}

/** Where synth presets live — one file per preset, listed by directory scan
    rather than through any index, the same "no bookkeeping beyond the
    filesystem itself" choice recordingsDirectory() already makes. */
juce::File MainComponent::presetsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Presets");
    dir.createDirectory();
    return dir;
}

/** Rescans presetsDirectory() and pushes the names into the Synth pane's
    list. Called whenever the set of saved presets can have changed (save,
    delete, startup) — the list is never mutated in place, only rebuilt,
    since a directory scan is cheap and a project with a handful of presets
    is the expected case, not hundreds. */
void MainComponent::refreshPresetList()
{
    presetFiles_.clear();
    for (const auto& entry : juce::RangedDirectoryIterator(presetsDirectory(), false, "*.looperpreset",
                                                            juce::File::findFiles))
        presetFiles_.push_back(entry.getFile());

    std::sort(presetFiles_.begin(), presetFiles_.end(),
             [](const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });

    juce::StringArray names;
    for (const auto& file : presetFiles_)
    {
        model::SynthPreset preset;
        // An unreadable preset (hand-edited, half-written) is still listed
        // by filename rather than silently vanishing — invisible is worse
        // than ugly for something the user put there on purpose.
        names.add(model::deserializePreset(file.loadFileAsString().toStdString(), preset)
                     ? (preset.name.empty() ? file.getFileNameWithoutExtension() : juce::String(preset.name))
                     : file.getFileNameWithoutExtension());
    }
    synthEditor_.setPresetNames(names);
}

/** Prompts for a name and saves the selected track's synth settings and
    whole effect chain (built-ins and any hosted distortion plugin alike) as
    a new preset file. */
void MainComponent::savePresetDialog()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
    if (track.type != model::TrackType::Instrument)
        return;

    auto* window = new juce::AlertWindow("Save Preset", "Name this preset:", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", track.name.empty() ? "My Preset" : (juce::String(track.name) + " Preset"));
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<MainComponent>(this), window,
             trackIndex = selectedTrackIndex_](int result)
            {
                if (self == nullptr || result != 1)
                    return;

                const auto name = window->getTextEditorContents("name").trim();
                if (name.isEmpty())
                    return;

                const auto& song = self->history_.current();
                if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
                    return;
                const auto& savedTrack = song.tracks[(size_t) trackIndex];

                model::SynthPreset preset;
                preset.name        = name.toStdString();
                preset.synth       = savedTrack.synthSettings;
                preset.effectChain = savedTrack.effectChain;

                const auto file = self->presetsDirectory()
                                      .getNonexistentChildFile(juce::File::createLegalFileName(name), ".looperpreset");
                if (file.replaceWithText(juce::String(model::serializePreset(preset))))
                {
                    self->refreshPresetList();
                    self->showStatus("Saved preset: " + name);
                }
                else
                {
                    self->showError("Could not save preset: " + name);
                }
            }),
        true);
}

/** Loads a preset onto the selected Instrument track, replacing its synth
    settings and whole effect chain as one undo step — a preset is one
    thing, not two separate edits a user would have to undo twice. */
void MainComponent::applyPreset(int index)
{
    if (index < 0 || index >= (int) presetFiles_.size())
        return;
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    if (history_.current().tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Instrument)
        return;

    model::SynthPreset preset;
    std::string        error;
    if (! model::deserializePreset(presetFiles_[(size_t) index].loadFileAsString().toStdString(), preset, &error))
    {
        showError("Could not load preset: " + juce::String(error));
        return;
    }

    const int trackIndex = selectedTrackIndex_;
    history_.edit("Load preset \"" + preset.name + "\"", [trackIndex, preset](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        track.synthSettings = preset.synth;
        track.effectChain   = preset.effectChain;
    });

    syncEngineTracks();
    refreshSynthEditorForSelected();
    refreshEffectChainForSelected();
    showStatus("Loaded preset: " + juce::String(preset.name));
}

/** Deletes a preset file and refreshes the list — no confirmation dialog,
    matching how this app treats every other delete (undo is the safety net
    for document edits, but a preset file is outside the document, so this
    one really is final; the list refresh and status message are the
    acknowledgment). */
void MainComponent::deletePresetAt(int index)
{
    if (index < 0 || index >= (int) presetFiles_.size())
        return;

    const auto file = presetFiles_[(size_t) index];
    const auto name = file.getFileNameWithoutExtension();

    if (file.deleteFile())
    {
        refreshPresetList();
        showStatus("Deleted preset: " + name);
    }
    else
    {
        showError("Could not delete preset: " + name);
    }
}

/** Populates an empty presets directory with a handful of starting points on
    first run, so the feature isn't an empty list the first time anyone
    opens it. Never touches a directory that already has anything in it —
    including a user who deleted every factory preset on purpose. */
void MainComponent::seedFactoryPresets()
{
    const auto dir = presetsDirectory();
    if (dir.getNumberOfChildFiles(juce::File::findFiles, "*.looperpreset") > 0)
        return;

    auto drive = [](float amount, float tone, float level, bool hardClip)
    {
        model::EffectSlot slot;
        slot.kind             = model::EffectKind::Drive;
        slot.enabled          = true;
        slot.drive.enabled    = true;
        slot.drive.drive      = amount;
        slot.drive.tone       = tone;
        slot.drive.level      = level;
        slot.drive.hardClip   = hardClip;
        slot.drive.cabinet    = true;
        return slot;
    };

    std::vector<model::SynthPreset> factory;

    {
        model::SynthPreset p;
        p.name             = "Warm Pad";
        p.synth.waveform   = 3; // triangle
        p.synth.attackMs   = 400.0f;
        p.synth.decayMs    = 600.0f;
        p.synth.sustain    = 0.8f;
        p.synth.releaseMs  = 1200.0f;
        p.synth.filterEnabled   = true;
        p.synth.filterMode      = 0;
        p.synth.filterCutoff    = 1800.0f;
        p.synth.filterResonance = 0.6f;

        model::EffectSlot chorus;
        chorus.kind          = model::EffectKind::Chorus;
        chorus.enabled       = true;
        chorus.chorus.enabled = true;
        chorus.chorus.rateHz = 0.4f;
        chorus.chorus.depth  = 0.6f;
        chorus.chorus.mix    = 0.5f;
        p.effectChain = { chorus };
        factory.push_back(std::move(p));
    }
    {
        model::SynthPreset p;
        p.name             = "Aggressive Bass";
        p.synth.waveform   = 1; // saw
        p.synth.attackMs   = 2.0f;
        p.synth.decayMs    = 80.0f;
        p.synth.sustain    = 0.9f;
        p.synth.releaseMs  = 60.0f;
        p.synth.filterEnabled   = true;
        p.synth.filterMode      = 0;
        p.synth.filterCutoff    = 500.0f;
        p.synth.filterResonance = 1.4f;

        model::EffectSlot compressor;
        compressor.kind                    = model::EffectKind::Compressor;
        compressor.enabled                 = true;
        compressor.compressor.enabled      = true;
        compressor.compressor.thresholdDb  = -20.0f;
        compressor.compressor.ratio        = 6.0f;
        p.effectChain = { drive(16.0f, 0.4f, 0.8f, false), compressor };
        factory.push_back(std::move(p));
    }
    {
        model::SynthPreset p;
        p.name             = "Dubstep Wobble Bass";
        p.synth.waveform   = 1; // saw
        p.synth.attackMs   = 1.0f;
        p.synth.decayMs    = 50.0f;
        p.synth.sustain    = 1.0f;
        p.synth.releaseMs  = 40.0f;

        model::EffectSlot wobble;
        wobble.kind                = model::EffectKind::Wobble;
        wobble.enabled             = true;
        wobble.wobble.enabled      = true;
        wobble.wobble.rateBeats    = 0.25f;
        wobble.wobble.depth        = 0.85f;
        wobble.wobble.baseCutoffHz = 150.0f;
        wobble.wobble.resonance    = 1.6f;
        p.effectChain = { drive(20.0f, 0.5f, 0.7f, true), wobble };
        factory.push_back(std::move(p));
    }
    {
        model::SynthPreset p;
        p.name             = "Bright Pluck";
        p.synth.waveform   = 2; // square
        p.synth.attackMs   = 1.0f;
        p.synth.decayMs    = 220.0f;
        p.synth.sustain    = 0.0f;
        p.synth.releaseMs  = 80.0f;

        model::EffectSlot tremolo;
        tremolo.kind            = model::EffectKind::Tremolo;
        tremolo.enabled         = true;
        tremolo.tremolo.enabled = true;
        tremolo.tremolo.rateHz  = 6.0f;
        tremolo.tremolo.depth   = 0.3f;
        p.effectChain = { tremolo };
        factory.push_back(std::move(p));
    }
    {
        // A punchy, filtered-down bass pluck — a low base cutoff swept open
        // by a fast, short filter envelope (the "pluck" is the sweep, not
        // the amp envelope) plus a sub-oscillator for low-end weight, in the
        // spirit of the analog-synth-bass tone Paul Meany plays in Mutemath.
        model::SynthPreset p;
        p.name             = "Analog Pluck Bass";
        p.synth.waveform   = 1; // saw
        p.synth.attackMs   = 1.0f;
        p.synth.decayMs    = 200.0f;
        p.synth.sustain    = 0.3f;
        p.synth.releaseMs  = 100.0f;
        p.synth.filterEnabled   = true;
        p.synth.filterMode      = 0;
        p.synth.filterCutoff    = 300.0f;
        p.synth.filterResonance = 1.2f;
        p.synth.filterEnvAmount    = 4000.0f;
        p.synth.filterEnvAttackMs  = 1.0f;
        p.synth.filterEnvDecayMs   = 180.0f;
        p.synth.filterEnvSustain   = 0.1f;
        p.synth.filterEnvReleaseMs = 60.0f;
        p.synth.subOscEnabled = true;
        p.synth.subOscLevel   = 0.4f;

        p.effectChain = { drive(10.0f, 0.4f, 0.75f, false) };
        factory.push_back(std::move(p));
    }

    for (const auto& preset : factory)
    {
        const auto file = dir.getNonexistentChildFile(juce::File::createLegalFileName(preset.name), ".looperpreset");
        file.replaceWithText(juce::String(model::serializePreset(preset)));
    }
}

/** Where the procedurally-generated starter drum sounds live — same
    "own directory, listed by scanning it" pattern as recordingsDirectory()
    and presetsDirectory(). */
juce::File MainComponent::factoryDrumKitDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Factory Kit");
    dir.createDirectory();
    return dir;
}

/** Writes twelve one-shot .wav files (three kicks, three snares, three
    closed hats, three claps — see engine::DrumSynth) into
    factoryDrumKitDirectory(), if it's empty. Never touches a directory that
    already has anything in it, same reasoning as seedFactoryPresets(): a
    user who removed or replaced these on purpose keeps that choice. */
void MainComponent::seedFactoryDrumKit()
{
    const auto dir = factoryDrumKitDirectory();

    // Fixed rather than the live device rate: these are rendered once, to
    // disk, and reused across sessions — re-rendering at whatever rate the
    // audio device happens to be running would make two machines' factory
    // kits differ for no reason, and AudioFilePlayerNode already resamples
    // whatever a file's own rate is.
    const double sampleRate = 48000.0;

    // Seeded per file rather than bailing on the whole directory. The intent
    // was always "a file the user replaced or deleted stays that way", and
    // per-file honours that exactly — while still letting a later version
    // add a sound an existing install would otherwise never receive, which
    // is what the whole-directory check quietly prevented.
    auto writeOneShot = [&](const juce::String& name, std::vector<float> samples)
    {
        const auto file = dir.getChildFile(name + ".wav");
        if (file.existsAsFile())
            return;

        engine::normalizePeak(samples);
        juce::AudioBuffer<float> buffer(1, (int) samples.size());
        std::copy(samples.begin(), samples.end(), buffer.getWritePointer(0));
        engine::OfflineRenderer::writeWav(file, buffer, sampleRate);
    };

    // Three variants each, so "generic" doesn't mean "one option" — a user
    // who doesn't like the default can swap to another via the existing
    // per-pad Load... button without leaving the app.
    writeOneShot("Kick Tight",  engine::synthesizeKick(sampleRate, 180.0f, 55.0f, 15.0f, 150.0f, 2.0f));
    writeOneShot("Kick Punchy", engine::synthesizeKick(sampleRate, 160.0f, 45.0f, 25.0f, 250.0f, 3.0f));
    writeOneShot("Kick Deep",   engine::synthesizeKick(sampleRate, 120.0f, 35.0f, 40.0f, 400.0f, 1.5f));

    writeOneShot("Snare Crisp", engine::synthesizeSnare(sampleRate, 200.0f, 0.25f, 140.0f, 3000.0f, 201u));
    writeOneShot("Snare Fat",   engine::synthesizeSnare(sampleRate, 180.0f, 0.4f,  220.0f, 1800.0f, 202u));
    writeOneShot("Snare Tight", engine::synthesizeSnare(sampleRate, 220.0f, 0.2f,  100.0f, 2500.0f, 203u));

    writeOneShot("Hat Closed", engine::synthesizeHat(sampleRate, 7000.0f, 60.0f, 301u));
    writeOneShot("Hat Tight",  engine::synthesizeHat(sampleRate, 9000.0f, 35.0f, 302u));
    writeOneShot("Hat Bright", engine::synthesizeHat(sampleRate, 11000.0f, 90.0f, 303u));

    writeOneShot("Clap Classic", engine::synthesizeClap(sampleRate, 1500.0f, 120.0f, 401u));
    writeOneShot("Clap Tight",   engine::synthesizeClap(sampleRate, 1800.0f, 80.0f, 402u));
    writeOneShot("Clap Roomy",   engine::synthesizeClap(sampleRate, 1200.0f, 200.0f, 403u));

    // The sounds engine::DrumKitStyle's non-Classic kits are built from.
    // Same four generators, pushed harder: the industrial kick clips into
    // its own drive (which is what makes it read as a machine rather than a
    // drum), the industrial snare trades body for noise, and the sub kick
    // starts and ends low with a long tail so a halftime groove has
    // something to sit on.
    writeOneShot("Kick Industrial",  engine::synthesizeKick(sampleRate, 220.0f, 50.0f, 8.0f, 180.0f, 8.0f));
    writeOneShot("Kick Sub",         engine::synthesizeKick(sampleRate, 90.0f,  30.0f, 60.0f, 600.0f, 2.5f));
    writeOneShot("Snare Industrial", engine::synthesizeSnare(sampleRate, 320.0f, 0.12f, 180.0f, 4500.0f, 204u));
}

/** engine::padsForDrumKitStyle's four pads for @p style, with each sample
    stem resolved against factoryDrumKitDirectory(). This resolution step is
    the whole reason the table itself lives in the engine layer and deals in
    stems: a file path is exactly the kind of thing neither that layer nor
    the model layer knows about. */
model::DrumKit MainComponent::kitForDrumKitStyle(engine::DrumKitStyle style) const
{
    const auto dir = factoryDrumKitDirectory();

    model::DrumKit kit;
    for (const auto& spec : engine::padsForDrumKitStyle(style))
    {
        model::DrumPad pad;
        pad.noteNumber     = spec.noteNumber;
        pad.label          = spec.label;
        pad.samplePath     = dir.getChildFile(juce::String(spec.sampleStem) + ".wav")
                                .getFullPathName().toStdString();
        pad.gainDb         = spec.gainDb;
        pad.pitchSemitones = spec.pitchSemitones;
        kit.pads.push_back(pad);
    }
    return kit;
}

/** The kit a new Drum track starts with: the Classic style, which is the
    same Kick Tight/Snare Crisp/Hat Closed/Clap Classic set this returned
    before kit styles existed. One definition rather than two, so the
    starting kit and the Classic button can't drift apart. */
model::DrumKit MainComponent::defaultDrumKitWithFactorySamples() const
{
    return kitForDrumKitStyle(engine::DrumKitStyle::Classic);
}

/** The project that exists the moment the app opens — deliberately not also
    what "New Project" resets to (createEmptyProject() stays a blank synth
    track with an empty clip, on purpose: a user who explicitly asks for a
    new project most likely wants a clean canvas, not a demo). A drum track
    with a programmed loop and real sounds is what makes a *fresh launch*
    audible immediately rather than opening on silence twice over — an
    empty synth clip and a kit with nothing assigned to it. */
model::Song MainComponent::makeStarterSong() const
{
    model::Song song;

    // Everything is keyed off the guitar's lowest open string, because that is
    // the one pitch here that isn't free: the Modern Metal preset is drop
    // tuned, and a riff has to land on a string the guitar actually has (see
    // makeStarterGuitarRiffPattern). Deriving the bass and lead from the same
    // note is what puts all four parts in one key — the previous starter song
    // had a drop-C guitar riff against a synth clip in no particular key, and
    // they simply clashed.
    const auto guitarTone = model::presetForGuitarTone(engine::GuitarTone::ModernMetal);
    const int  keyRoot    = guitarTone.guitar.tuning[0];

    constexpr double kBar   = engine::kStarterBeatsPerBar;
    constexpr double kCycle = engine::kStarterCycleBeats;

    /** One clip, positioned. Every part below is placed the same way, and the
        arrangement is easier to read as four lists of bars than as forty
        lines of struct-filling. */
    auto place = [&song](int trackId, engine::Pattern pattern, double startBeats)
    {
        model::Clip clip;
        clip.type        = model::ClipType::Instrument;
        clip.startBeats  = startBeats;
        clip.lengthBeats = pattern.lengthBeats;
        clip.pattern     = std::move(pattern);
        model::addClip(song, trackId, clip);
    };

    // --- Bass: enters at bar 5, and is the first track so that index 0 stays
    // an Instrument track, as it has always been.
    const auto bassPreset = model::presetForSynthTone(engine::SynthTone::CyberBass);

    const int bassId = model::addTrack(song, model::TrackType::Instrument, "Bass").id;
    song.tracks.back().synthSettings = bassPreset.synth;
    song.tracks.back().effectChain   = bassPreset.effectChain;
    // An octave above the guitar's low string: a bass under a drop tuning is
    // already at the bottom of what most speakers reproduce.
    const int bassRoot = keyRoot + 12;
    for (int cycle = 1; cycle < 4; ++cycle)
        place(bassId, engine::makeStarterBassPattern(bassRoot), (double) cycle * kCycle);

    // --- Drums: the only part playing from bar 1, so the song starts with
    // something rather than with a count of silence. Four contiguous cycles —
    // a gap between drum clips would be a hole in the song, since a track
    // with more than one clip really is silent between them.
    const int drumId = model::addTrack(song, model::TrackType::Drum, "Drums").id;
    song.tracks.back().drumKit = defaultDrumKitWithFactorySamples();

    place(drumId, engine::makeStarterDrumIntroPattern(), 0.0);
    place(drumId, engine::makeStarterDrumGroovePattern(false), kCycle);
    place(drumId, engine::makeStarterDrumGroovePattern(false), 2.0 * kCycle);
    place(drumId, engine::makeStarterDrumGroovePattern(true),  3.0 * kCycle); // fill to finish

    // --- Guitar: enters at bar 9. Given the Modern Metal tone rather than a
    // bare default so the pedal chain, the gate and the drop tuning are all on
    // screen and audible on first launch instead of being features you have to
    // know to go looking for.
    const int guitarId = model::addTrack(song, model::TrackType::Guitar, "Guitar").id;
    song.tracks.back().guitarSettings = guitarTone.guitar;
    song.tracks.back().effectChain    = guitarTone.effectChain;

    for (int cycle = 2; cycle < 4; ++cycle)
        place(guitarId, engine::makeStarterGuitarRiffPattern(keyRoot), (double) cycle * kCycle);

    // --- Lead: enters last, at bar 13, as two answering phrases. Two clips
    // rather than one four-bar clip on purpose — see makeStarterLeadPattern.
    const auto leadPreset = model::presetForSynthTone(engine::SynthTone::CyberLead);

    const int leadId = model::addTrack(song, model::TrackType::Instrument, "Lead").id;
    song.tracks.back().synthSettings = leadPreset.synth;
    song.tracks.back().effectChain   = leadPreset.effectChain;

    const int leadRoot = keyRoot + 24; // two octaves up: melody register
    place(leadId, engine::makeStarterLeadPattern(leadRoot, 0), 3.0 * kCycle);
    place(leadId, engine::makeStarterLeadPattern(leadRoot, 1), 3.0 * kCycle + 2.0 * kBar);

    return song;
}

/** Puts a passing message on screen. Deliberately not routed through any
    pane: a pane can be collapsed or closed, and a report that lands somewhere
    invisible is worse than none — the user reads silence as success. */
void MainComponent::showStatus(const juce::String& message)
{
    status_.show(message, false);
}

/** As showStatus, for the messages that report something didn't work. Held
    longer and marked, since these are the ones worth being sure was seen. */
void MainComponent::showError(const juce::String& message)
{
    status_.show(message, true);
    juce::Logger::writeToLog("Status: " + message);
}

/** As showStatus, but forces the message on screen before returning — for
    the handful of actions (bounce, plugin scan) that then block the message
    thread for real work. A plain showStatus() only marks the banner dirty;
    without a peer repaint forced here, that paint request would just sit
    queued behind the very call that's about to freeze the UI, and the
    message would never be seen until after the freeze was already over. */
void MainComponent::showBusy(const juce::String& message)
{
    status_.show(message, false);
    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();
}

void MainComponent::wireUndoableSlider(juce::Slider& slider, juce::String label,
                                       std::function<float(const model::Song&)> read,
                                       std::function<void(model::Song&, float)> write)
{
    // Shared, not a member: this is called once per slider (there are over a
    // dozen in the master panel alone), and a dedicated member per slider is
    // exactly the per-control bookkeeping this helper exists to avoid.
    auto dragFrom = std::make_shared<float>(0.0f);

    slider.onDragStart = [this, dragFrom, read] { *dragFrom = read(history_.current()); };
    slider.onDragEnd = [this, dragFrom, label, read, write]
    {
        const float landedOn = read(history_.current());
        commitDrag(history_, label.toStdString(), *dragFrom, landedOn, write);
    };
}

/** True while the document differs from what's on disk. Asks the history for
    the identity of the state it's holding rather than tracking a modified
    flag, so undoing back to the saved state reads as saved again — see
    History::stateId. */
bool MainComponent::hasUnsavedChanges() const
{
    return history_.stateId() != savedStateId_;
}

/** Puts the project's name and an unsaved marker in the title bar, which is
    the only place either is visible. Called every timer tick, so it compares
    before setting: DocumentWindow::setName repaints the frame. */
void MainComponent::updateWindowTitle()
{
    const juce::String name = (projectFile_ == juce::File{})
                                  ? juce::String("Untitled")
                                  : projectFile_.getFileNameWithoutExtension();

    const juce::String title = name + (hasUnsavedChanges() ? " *" : "") + " - Looper-Audio";
    if (title == windowTitle_)
        return;

    windowTitle_ = title;
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName(title);
}

/** The actual write, shared by Save and Save As. Marks the document clean
    against the state that was written — not whatever it becomes later — so an
    edit made while the file chooser was up still counts as unsaved. */
bool MainComponent::writeProjectTo(const juce::File& file)
{
    const auto stateWritten = history_.stateId();
    const std::string text  = model::serialize(history_.current());

    if (! file.replaceWithText(juce::String::fromUTF8(text.c_str())))
    {
        showError("Could not save " + file.getFileName());
        return false;
    }

    projectFile_   = file;
    savedStateId_  = stateWritten;
    updateWindowTitle();
    return true;
}

/** Saves over the project's own file, falling back to Save As the first time.
    @p onDone reports whether the document actually reached disk — the
    discard prompt needs to know, since a cancelled save must cancel whatever
    it was clearing the way for. */
void MainComponent::saveProject(std::function<void(bool)> onDone)
{
    if (projectFile_ == juce::File{})
    {
        saveProjectAs(std::move(onDone));
        return;
    }

    const bool saved = writeProjectTo(projectFile_);
    if (onDone)
        onDone(saved);
}

void MainComponent::saveProjectAs(std::function<void(bool)> onDone)
{
    chooser_ = std::make_unique<juce::FileChooser>("Save project", projectFile_, "*.looper");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, onDone](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
        {
            if (onDone)
                onDone(false); // dismissed the chooser: nothing was saved
            return;
        }

        const bool saved = writeProjectTo(file.withFileExtension("looper"));
        if (onDone)
            onDone(saved);
    });
}

/** Runs @p onProceed once it's safe to throw the current document away,
    asking first if there's anything to lose. Everything that discards the
    document goes through here — New, Open, and quitting — so there is one
    place the question is asked and one place it can be got wrong.

    Cancel, and a Save the user backs out of, both simply drop @p onProceed:
    the destructive action doesn't happen. */
void MainComponent::confirmDiscardChanges(std::function<void()> onProceed)
{
    if (! hasUnsavedChanges())
    {
        if (onProceed)
            onProceed();
        return;
    }

    const juce::String name = (projectFile_ == juce::File{})
                                  ? juce::String("this project")
                                  : projectFile_.getFileName();

    juce::NativeMessageBox::showYesNoCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Unsaved changes",
        "Save changes to " + name + " before closing it?",
        this,
        juce::ModalCallbackFunction::create([self = juce::Component::SafePointer<MainComponent>(this),
                                             onProceed](int result)
        {
            if (self == nullptr)
                return; // the window went away while the box was up

            if (result == 1) // Yes: save first, and only then go ahead
            {
                self->saveProject([onProceed](bool saved) { if (saved && onProceed) onProceed(); });
            }
            else if (result == 2) // No: discard
            {
                if (onProceed)
                    onProceed();
            }
            // Cancel (0): stay exactly where we are.
        }));
}

void MainComponent::openProject()
{
    confirmDiscardChanges([this] { chooseProjectToOpen(); });
}

void MainComponent::chooseProjectToOpen()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open project", projectFile_, "*.looper");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        model::Song song;
        std::string error;
        if (! model::deserialize(file.loadFileAsString().toStdString(), song, &error))
        {
            showError("Could not open " + file.getFileName() + ": " + error);
            return;
        }

        history_.reset(song);
        selectedTrackIndex_ = 0;

        tempoSlider.setValue(song.bpm, juce::dontSendNotification);
        uiTempoMap_.setTempo(song.bpm);
        post(Cmd::SetTempo, song.bpm);
        refreshFromModel();

        projectFile_  = file;
        savedStateId_ = history_.stateId(); // what's on screen is what's on disk
        updateWindowTitle();
    });
}

/** Asks what kind of file to write, then where to put it, then writes it.

    Two dialogs in sequence rather than one: the format decides the file
    extension, so the save dialog can't filter correctly until the format is
    known. Asking for the location first would mean either an unfiltered
    chooser or one that lies about what it is about to write.

    This replaced a WAV-only "Bounce" that hardcoded both the extension and
    24-bit depth. */
void MainComponent::exportAudioDialog()
{
    const double deviceRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;

    app::ExportAudioDialog::show(this, deviceRate,
        [self = juce::Component::SafePointer<MainComponent>(this)](engine::ExportOptions options)
        {
            if (self != nullptr)
                self->exportProject(options);
        });
}

/** One file an export will write: what to render, and how to write it. */
struct MainComponent::ExportTask
{
    juce::File                                   file;
    engine::AudioEngine::OfflineRenderOptions    render;
    engine::ExportOptions                        write;
    juce::String                                 label; // shown in the progress window
};

void MainComponent::exportProject(const engine::ExportOptions& options)
{
    if (renderJob_ != nullptr)
    {
        showError("An export is already running");
        return;
    }

    const auto extension = engine::extensionFor(options.format);

    chooser_ = std::make_unique<juce::FileChooser>("Export " + extension.toUpperCase(),
                                                   juce::File{}, "*." + extension);
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, options, extension](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        file = file.withFileExtension(extension);

        bool       folderFailed = false;
        const auto tasks        = buildExportTasks(file, options, folderFailed);

        if (folderFailed)
        {
            showError("Export failed (could not create the stems folder)");
            return;
        }

        if (tasks.empty())
        {
            showError("Nothing to export — every track is muted or silenced by a solo");
            return;
        }

        startExport(tasks, file);
    });
}

/**
    Every file the export will write, decided up front on the message thread.

    Built as a list rather than worked out as it goes because the rendering
    happens on a background thread, and that thread must not reach into the
    document or the UI. Everything it needs — files, render options, track
    names — is captured by value here.
*/
std::vector<MainComponent::ExportTask>
MainComponent::buildExportTasks(const juce::File& masterFile,
                                const engine::ExportOptions& options,
                                bool& folderFailed)
{
    std::vector<ExportTask> tasks;

    // A tail past the last clip so reverb and delay decay into the file rather
    // than being cut off mid-ring at the final beat. Shared by the mix and
    // every stem, so they all come out the same length and line up when
    // dropped into another session.
    const double lengthBeats = songEndBeats() + kBounceTailBeats;

    if (engine::writesMasterMix(options.contents))
    {
        ExportTask task;
        task.file  = masterFile;
        task.write = options;
        task.label = "master mix";
        // Everything the mix contains, rendered by the mixer itself — see
        // AudioEngine::renderOffline, and the comment there for why an export
        // calling the mixer rather than copying it is the whole design.
        task.render.lengthBeats = lengthBeats;
        task.render.sampleRate  = options.sampleRate;
        tasks.push_back(std::move(task));
    }

    if (! engine::writesStems(options.contents))
        return tasks;

    const auto folder = app::stemFolderFor(masterFile);
    if (! folder.createDirectory())
    {
        folderFailed = true;
        return {};
    }

    const auto& song      = history_.current();
    const auto  extension = engine::extensionFor(options.format);
    const int   trackCount = juce::jmin((int) song.tracks.size(), engine_.maxTracks());

    for (int i = 0; i < trackCount; ++i)
    {
        // Which tracks get a stem is the engine's rule, not a second copy of
        // it here — see AudioEngine::trackContributesToMix.
        if (! engine_.trackContributesToMix(i))
            continue;

        ExportTask task;
        // Numbered by the track's position in the song, not by how many stems
        // have been written — so the file names line up with the tracks on
        // screen even when a muted track in the middle has been skipped.
        task.file  = folder.getChildFile(
            app::stemFileName(i + 1, song.tracks[(size_t) i].name, extension));
        task.write = options;
        task.label = juce::String(song.tracks[(size_t) i].name);

        task.render.lengthBeats    = lengthBeats;
        task.render.sampleRate     = options.sampleRate;
        task.render.soloTrack      = i;
        task.render.applyMasterBus = false; // stems are pre-master — see OfflineRenderOptions

        tasks.push_back(std::move(task));
    }

    return tasks;
}

/** Runs @p tasks on a background thread behind a progress window. */
void MainComponent::startExport(const std::vector<ExportTask>& tasks, const juce::File& masterFile)
{
    struct Result
    {
        int  written  = 0;
        int  stems    = 0;
        bool anyFailure = false;
    };

    auto result = std::make_shared<Result>();

    // The engine belongs to the render thread for the duration — see the guard
    // in timerCallback, and note that pump() frees retired audio objects.
    offlineRenderInProgress_ = true;

    auto work = [this, tasks, result](app::OfflineRenderJob& job)
    {
        const int count = (int) tasks.size();

        for (int i = 0; i < count; ++i)
        {
            if (job.shouldAbort())
                return;

            auto task = tasks[(size_t) i];
            const auto label = task.label;

            task.render.onProgress = [&job, i, count, label](double fraction)
            {
                job.report(app::overallProgress(i, count, fraction),
                           "Rendering " + label + "  (" + juce::String(i + 1)
                               + " of " + juce::String(count) + ")");
                return ! job.shouldAbort();
            };

            const auto buffer = engine_.renderOffline(task.render);

            // Empty means cancelled, or nothing to render. Either way there is
            // no file worth writing — see OfflineRenderOptions::onProgress for
            // why a cancelled render deliberately returns nothing rather than
            // a partial buffer.
            if (buffer.getNumSamples() == 0)
                continue;

            if (engine::writeAudioFile(task.file, buffer, task.write))
            {
                ++result->written;
                if (task.render.soloTrack >= 0)
                    ++result->stems;
            }
            else
            {
                result->anyFailure = true;
            }
        }
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this),
                       result, masterFile](bool cancelled)
    {
        if (self == nullptr)
            return;

        self->offlineRenderInProgress_ = false;
        self->renderJob_.reset();

        // Device changes were ignored while the render owned the engine, so
        // this catches up on any that happened — headphones plugged in
        // mid-export are still plugged in now.
        self->followSystemOutputIfEnabled();

        // Cancelling keeps whatever was already written rather than deleting
        // it: those files are complete, and throwing away finished work
        // because the *next* one was interrupted would be its own surprise.
        if (cancelled)
        {
            self->showError("Export cancelled — " + juce::String(result->written)
                            + " file(s) written");
            return;
        }

        if (result->anyFailure)
        {
            self->showError("Exported with errors — " + juce::String(result->written)
                            + " file(s) written, some failed");
            return;
        }

        if (result->written == 0)
        {
            self->showError("Nothing was exported");
            return;
        }

        // The stem count is said plainly because it is the only place the
        // mute/solo rule becomes visible: stems are the tracks that sound in
        // the mix, so exporting with solo left on legitimately writes one.
        if (result->stems > 0)
            self->showStatus("Exported: " + masterFile.getFileName() + " + "
                             + juce::String(result->stems) + " stem(s)");
        else
            self->showStatus("Exported: " + masterFile.getFileName());
    };

    renderJob_ = app::OfflineRenderJob::launch("Exporting audio", std::move(work),
                                               std::move(onFinished));
}

/** The beat the playhead is on, for the tempo controls. */
double MainComponent::playheadBeat() const
{
    return juce::jmax(0.0, uiTempoMap_.ppqFromSamples(engine_.playheadSamples()));
}

/** Applies @p bpm to whichever tempo is in force at the playhead.

    At the start of the song that is the song's tempo; inside a section with a
    tempo change it is that change. Both go through history_, so a tempo edit
    undoes like any other. */
void MainComponent::setTempoAtPlayhead(double bpm)
{
    const double beat = playheadBeat();

    history_.edit("Tempo", [beat, bpm](model::Song& s)
    {
        // Which entry the playhead is inside: the last change at or before it,
        // or the song's own tempo when it is before them all.
        int index = -1;
        for (int i = 0; i < (int) s.tempoChanges.size(); ++i)
            if (beat >= s.tempoChanges[(size_t) i].beat)
                index = i;

        if (index >= 0)
            s.tempoChanges[(size_t) index].bpm = bpm;
        else
            s.bpm = bpm;
    });

    pushTempoMap();
}

/** Adds a tempo change at @p beat, or edits the one already there. */
void MainComponent::editTempoChangeAt(double beat)
{
    const auto& song = history_.current();
    double      current = model::tempoAtBeat(song, beat);

    for (const auto& change : song.tempoChanges)
        if (std::abs(change.beat - beat) < 1.0e-9)
            current = change.bpm;

    auto* window = new juce::AlertWindow("Tempo change", {},
                                         juce::MessageBoxIconType::NoIcon, this);

    const double qpb = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    window->addTextEditor("bpm", juce::String(current, 2),
                          "Tempo at bar " + juce::String((int) std::round(beat / qpb) + 1) + ":");
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, beat](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double bpm = window->getTextEditorContents("bpm").getDoubleValue();

            // Refused rather than clamped: a tempo someone mistyped is worth
            // saying no to, and a silently corrected 0 would be a mystery.
            if (bpm < 20.0 || bpm > 400.0)
            {
                self->showError("Tempo must be between 20 and 400 bpm");
                return;
            }

            self->applyTempoChange(beat, bpm);
        }));
}

void MainComponent::applyTempoChange(double beat, double bpm)
{
    history_.edit("Tempo change", [beat, bpm](model::Song& s)
    {
        // Beat 0 is the song's own tempo rather than an entry in the list —
        // see model::tempoMapFor for why the two are kept apart.
        if (beat <= 0.0)
        {
            s.bpm = bpm;
            return;
        }

        for (auto& change : s.tempoChanges)
        {
            if (std::abs(change.beat - beat) < 1.0e-9)
            {
                change.bpm = bpm;
                return;
            }
        }

        s.tempoChanges.push_back({ beat, bpm });
        std::sort(s.tempoChanges.begin(), s.tempoChanges.end(),
                  [](const engine::TempoChange& a, const engine::TempoChange& b)
                  { return a.beat < b.beat; });
    });

    pushTempoMap();
}

/** Moves the tempo change at @p fromBeat to @p toBeat.

    Dropping one onto another merges rather than leaving two changes at the
    same position: only one of them could ever be in force, and a hidden
    duplicate is worse than a visible replacement. */
void MainComponent::moveTempoChange(double fromBeat, double toBeat)
{
    if (toBeat <= 0.0)
        return; // beat 0 is the song's own tempo, not a change — see model::tempoMapFor

    history_.edit("Move tempo change", [fromBeat, toBeat](model::Song& s)
    {
        double bpm = 0.0;
        for (const auto& change : s.tempoChanges)
            if (std::abs(change.beat - fromBeat) < 1.0e-9)
                bpm = change.bpm;

        if (bpm <= 0.0)
            return; // it went away underneath the drag

        s.tempoChanges.erase(std::remove_if(s.tempoChanges.begin(), s.tempoChanges.end(),
                                            [fromBeat, toBeat](const engine::TempoChange& c)
                                            {
                                                return std::abs(c.beat - fromBeat) < 1.0e-9
                                                    || std::abs(c.beat - toBeat) < 1.0e-9;
                                            }),
                             s.tempoChanges.end());

        s.tempoChanges.push_back({ toBeat, bpm });
        std::sort(s.tempoChanges.begin(), s.tempoChanges.end(),
                  [](const engine::TempoChange& a, const engine::TempoChange& b)
                  { return a.beat < b.beat; });
    });

    pushTempoMap();
}

/** Switches the change at @p beat between jumping to its tempo and sliding to
    it from the one before. */
void MainComponent::toggleTempoRamp(double beat)
{
    history_.edit("Tempo ramp", [beat](model::Song& s)
    {
        for (auto& change : s.tempoChanges)
            if (std::abs(change.beat - beat) < 1.0e-9)
                change.ramp = ! change.ramp;
    });

    pushTempoMap();
}

void MainComponent::removeTempoChangeAt(double beat)
{
    history_.edit("Remove tempo change", [beat](model::Song& s)
    {
        s.tempoChanges.erase(std::remove_if(s.tempoChanges.begin(), s.tempoChanges.end(),
                                            [beat](const engine::TempoChange& c)
                                            { return std::abs(c.beat - beat) < 1.0e-9; }),
                             s.tempoChanges.end());
    });

    pushTempoMap();
}

/** Hands the current map to the engine and the UI's own copy, and refreshes
    everything that depends on where beats fall. */
void MainComponent::pushTempoMap()
{
    const auto& song = history_.current();
    const auto  map  = model::tempoMapFor(song);

    uiTempoMap_.setTempoChanges(map);
    engine_.setTempoChanges(map);

    // The loop region is a musical position, so where it lands in samples
    // changed with the map.
    updateLoopRegion();

    arrangementView_.setSong(song);
    updateEditingLabel();
}

/** Moves the playhead to @p beat, clamped at zero. */
void MainComponent::seekToBeat(double beat)
{
    const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    uiTempoMap_.setSampleRate(sampleRate);
    post(Cmd::Seek, (double) uiTempoMap_.samplesFromPpq(juce::jmax(0.0, beat)));
}

/** Steps the playhead by whole bars — what "previous/next frame" means in a
    DAW, where the musical unit is a bar rather than a video frame. */
void MainComponent::stepByBars(int bars)
{
    const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    uiTempoMap_.setSampleRate(sampleRate);

    const double current = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
    const double perBar  = juce::jmax(1.0, uiTempoMap_.quartersPerBar());

    // Snap to the bar line first, so stepping from mid-bar lands on a bar
    // rather than carrying the offset along.
    const double currentBar = std::floor(current / perBar + 1.0e-9);
    seekToBeat((currentBar + bars) * perBar);
}

/** The end of the song's content — the furthest point any clip reaches. Zero
    for an empty project, so "go to end" is simply "go to start" there. */
/** Stops the transport once it has played everything that was arranged.

    Without this the playhead runs on for ever past the last clip, playing
    silence — the arrangement has an end, so the transport should have one
    too. Looping is left alone: that is the case where running past the last
    clip is the whole point, and the engine wraps it in the audio thread.

    Checked on the UI timer rather than in the engine: 30Hz is a thirtieth of
    a second of overshoot on a transport that is playing silence by then, and
    it costs the audio thread nothing. */
void MainComponent::stopAtEndOfArrangement()
{
    if (! engine_.isPlaying() || loopButton.getToggleState() || awaitingRecordedTake_)
        return;

    const double end = songEndBeats();
    if (end <= 0.0)
        return; // nothing arranged: there is no end to stop at

    const double playhead = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
    if (playhead < end)
        return;

    post(Cmd::SetPlaying, 0.0);
    showStatus("Reached the end of the arrangement");
}

double MainComponent::songEndBeats() const
{
    double end = 0.0;
    for (const auto& track : history_.current().tracks)
        for (const auto& clip : track.clips)
            end = juce::jmax(end, clip.startBeats + clip.lengthBeats);
    return end;
}

/** The loop runs over what has actually been arranged, rounded up to a bar.

    It used to be a hardcoded four bars whatever the song contained, so
    arranging anything longer than that silently looped only its opening —
    and arranging less looped several bars of nothing. */
void MainComponent::updateLoopRegion()
{
    const double sampleRate = engine_.sampleRate();
    if (sampleRate <= 0.0)
        return;

    uiTempoMap_.setSampleRate(sampleRate);

    // Through the map, not a multiplication: the loop end is a musical
    // position, and where it falls in samples depends on every tempo before it.
    const auto endSamples = uiTempoMap_.samplesFromPpq(loopEndBeats());
    post(Cmd::SetLoopRegion, 0.0, (double) endSamples);
}

/** Where the arrangement ends, rounded up to a whole bar — an empty song
    still gets one bar, so the loop is never zero-length. */
double MainComponent::loopEndBeats() const
{
    return engine::loopEndForContent(songEndBeats(), juce::jmax(1.0, uiTempoMap_.quartersPerBar()));
}

void MainComponent::timerCallback()
{
    // A background render owns the engine right now. pump() frees the objects
    // the audio thread has handed back, and doing that against a render in
    // flight is a use-after-free — so nothing here touches the engine until it
    // is handed back. Meters and the playhead freezing during an export is
    // correct: they would be reporting state that is being used for something
    // else entirely.
    if (offlineRenderInProgress_)
        return;

    engine_.pump();
    finishRecordingIfReady();
    finishMidiRecordingIfReady();

    // Hot-plugged MIDI, on a slow cadence: enumerating devices is a system
    // call and this timer runs at 30Hz, so once every two seconds rather than
    // every tick. Arming a take re-scans immediately anyway (see
    // toggleRecording) — this is what makes a controller plugged in mid-session
    // *play* without having to press Record first.
    if (++midiRescanTicks_ >= 60)
    {
        midiRescanTicks_ = 0;
        engine_.refreshMidiInputs();
    }
    updateWindowTitle();
    stopAtEndOfArrangement();

    addTrackButton.setEnabled(trackCount() < engine_.maxTracks());
    addDrumTrackButton_.setEnabled(trackCount() < engine_.maxTracks());
    addGuitarTrackButton_.setEnabled(trackCount() < engine_.maxTracks());
    addBusTrackButton_.setEnabled(trackCount() < engine_.maxTracks());
    addClipButton_.setEnabled(selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount());
    // Audio tracks have no MIDI pattern to generate into.
    generateLoopButton_.setEnabled(selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
        && history_.current().tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Audio);

    const double sampleRate = engine_.sampleRate();
    uiTempoMap_.setSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0);

    const int64_t playhead = engine_.playheadSamples();
    const auto    bb       = uiTempoMap_.barsBeatsFromSamples(playhead);
    const double  seconds  = sampleRate > 0.0 ? (double) playhead / sampleRate : 0.0;

    // "COUNT-IN" rather than "PLAYING" while the click is counting you in —
    // the transport is rolling but nothing is being captured yet, and that
    // distinction is the whole point of the feature.
    const char* transportState = engine_.isCountingIn() ? "COUNT-IN"
                               : engine_.isPlaying()    ? "PLAYING"
                                                        : "STOPPED";
    // The transport also starts and stops from elsewhere (clip launches, the
    // menu), so the glyph follows the engine rather than the last click.
    playPauseButton.setToggleState(engine_.isPlaying(), juce::dontSendNotification);

    positionLabel.setText(juce::String::formatted("Bar %d  Beat %d   |   %.2f s   |   %s",
                                                  bb.bar, bb.beat, seconds, transportState),
                          juce::dontSendNotification);

    // The slider follows the playhead through the tempo map, so playing into a
    // section with a different tempo shows that tempo rather than the song's
    // first one. Skipped while it is being dragged, or it would fight the
    // hand that is moving it.
    if (! tempoSlider.isMouseButtonDown())
    {
        const double atPlayhead = model::tempoAtBeat(history_.current(), playheadBeat());
        if (std::abs(tempoSlider.getValue() - atPlayhead) > 1.0e-6)
            tempoSlider.setValue(atPlayhead, juce::dontSendNotification);
    }

    meter_.setLevel(0, engine_.masterPeak(0));
    meter_.setLevel(1, engine_.masterPeak(1));
    masteringPane_.setReductionDb(engine_.masteringReductionDb());
    audioEditor_.setPlaybackState(engine_.isPlaying(),
                                  clipSecondsForSongBeat(uiTempoMap_.ppqFromSamples(playhead)));

    const int n = trackCount();
    for (int i = 0; i < n; ++i)
    {
        trackStrips_[i]->setLevel(0, engine_.trackPeak(i, 0));
        trackStrips_[i]->setLevel(1, engine_.trackPeak(i, 1));
    }

    arrangementView_.setPlayheadBeats(uiTempoMap_.ppqFromSamples(playhead));
    // The automation pane reads against the same playhead, so a curve can be
    // watched doing what it does while the song runs.
    automationPane_.setPlayheadBeat(uiTempoMap_.ppqFromSamples(playhead));

    // Which session cells are actually sounding comes from the engine, not the
    // document: a launch is pending until the next bar line, so the grid would
    // light the wrong cell if it guessed.
    {
        std::vector<int> playingSlots((size_t) n);
        for (int i = 0; i < n; ++i)
            playingSlots[(size_t) i] = engine_.sessionSlotPlaying(i);
        sessionView_.setPlayingSlots(playingSlots);
    }

    // What the guitar is actually sounding, per string. Read from the engine
    // rather than inferred: a string keeps ringing after its note-off, so the
    // document can't say which notes are live.
    {
        std::array<int, model::kNumGuitarStrings> ringing {};
        for (int s = 0; s < model::kNumGuitarStrings; ++s)
            ringing[(size_t) s] = engine_.guitarNoteOnString(selectedTrackIndex_, s);
        fretboard_.setRingingNotes(ringing);
    }

    // The step grid's playhead walks the pattern's own loop, so it needs the
    // position relative to the open clip's start rather than the song's.
    {
        const auto&  song      = history_.current();
        double       clipStart = 0.0;
        if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
        {
            const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
            if (selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) clips.size())
                clipStart = clips[(size_t) selectedClipIndex_].startBeats;
        }
        const double intoClip = uiTempoMap_.ppqFromSamples(playhead) - clipStart;
        drumsPane_.setPlayheadBeats(intoClip, engine_.isPlaying());

        // Wrapped into the pattern, because a clip loops: the engine wraps
        // playback within the pattern length, so an unwrapped position would
        // walk off the right of the grid on the first repeat and never
        // return. Only shown while the clip is actually under the playhead.
        const double patternBeats = currentPattern().lengthBeats;
        const bool   inClip       = intoClip >= 0.0 && engine_.isPlaying();
        pianoRoll_.setPlayheadBeats(patternBeats > 0.0 ? engine::wrapPositive(intoClip, patternBeats)
                                                       : 0.0,
                                    inClip);
        followKeysPlayhead();
    }

    // Gain automation playback (coarse, message-thread; sample-accurate on
    // export — see bounceProject()). Master and per-track lanes both apply.
    if (! recordAutomation_ && engine_.isPlaying())
    {
        const double beat = uiTempoMap_.ppqFromSamples(playhead);
        const auto&  song = history_.current();

        if (! song.masterGainDb.empty())
        {
            const float db = song.masterGainDb.valueAt(beat, (float) masterSlider.getValue());
            post(Cmd::SetMasterGainDb, db);
            masterSlider.setValue(db, juce::dontSendNotification);
        }

        // Per-track automation is *applied* by the engine now (each track
        // ramps its own curves across every block, see InstrumentTrack), so
        // this only moves the controls to follow along. Pushing values from
        // here as well would fight the engine and re-introduce the 30Hz
        // stepping this replaced.
        for (int i = 0; i < n; ++i)
        {
            const auto& track = song.tracks[(size_t) i];

            if (const auto* lane = track.lane(model::TrackParam::Gain))
                trackStrips_[i]->setGainDb(lane->valueAt(beat, track.gainDb));
            if (const auto* lane = track.lane(model::TrackParam::Pan))
                trackStrips_[i]->setPan(lane->valueAt(beat, track.pan));
            if (const auto* lane = track.lane(model::TrackParam::SendLevel))
                trackStrips_[i]->setSendLevel(lane->valueAt(beat, track.sendLevel));
        }
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Plugging in headphones during an export would otherwise re-open the audio
    // device underneath a render that has deliberately suspended it — the one
    // message-thread path into the engine that the progress window's modal
    // state does not block, since it is driven by the hardware rather than by
    // the user. The device is followed again as soon as the export finishes.
    if (offlineRenderInProgress_)
        return;

    followSystemOutputIfEnabled();
    logAudioDeviceStatus();
    updateLoopRegion();
}

/** The device manager broadcasts whenever the device list changes, which is
    what plugging in headphones looks like from here. JUCE won't move by
    itself — it opened a device by name at launch and keeps it — so this is
    what actually follows the system. */
void MainComponent::followSystemOutputIfEnabled()
{
    if (! followSystemOutput_ || switchingDevice_)
        return;

    // Re-opening the device broadcasts another change, and without this the
    // callback would re-enter while the device is half-open.
    const juce::ScopedValueSetter<bool> guard(switchingDevice_, true);

    const auto switchedTo = engine_.followSystemDefaultOutput();
    if (switchedTo.isNotEmpty())
        showStatus("Output: " + switchedTo);
}

void MainComponent::logAudioDeviceStatus()
{
    if (auto* device = engine_.deviceManager().getCurrentAudioDevice())
        juce::Logger::writeToLog("Audio device: " + device->getName()
            + " | " + juce::String(device->getCurrentSampleRate(), 0) + " Hz"
            + " | buffer " + juce::String(device->getCurrentBufferSizeSamples()) + " samples");
    else
        juce::Logger::writeToLog("Audio device: none open");
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto full = getLocalBounds();
    menuBar_.setBounds(full.removeFromTop(24));
    workspace_.setBounds(full); // the workspace lays its own tree out from here

    // Sits over the workspace, against the bottom of the window.
    status_.updateBounds();
}

void MainComponent::layoutLeftPane()
{
    auto area = leftPane_.getLocalBounds().reduced(12);

    auto row = area.removeFromTop(30);

    // Taken off the right first, so it stays pinned to the far edge whatever
    // width the pane has.
    collapseTransportButton_.setBounds(row.removeFromRight(28).reduced(2));
    row.removeFromRight(8);

    // The controls wrap onto another row when they don't fit — see
    // app::wrapRow for what went wrong when they didn't.
    //
    // First / previous / play-pause / next / last, in that order. The
    // frame-step glyphs are wider than tall, play/pause is taller than wide,
    // so they get different widths to keep the drawn glyphs a similar size.
    const std::vector<app::RowItem> items {
        { 32,  0, { 2, 2 } }, // first frame
        { 26,  0, { 2, 2 } }, // previous frame
        { 30,  0, { 3, 1 } }, // play/pause
        { 26,  0, { 2, 2 } }, // next frame
        { 32,  0, { 2, 2 } }, // last frame
        { 60, 12, { 0, 0 } }, // loop
        { 30, 12, { 1, 1 } }, // record — square: the icon is 25x25
        { 64, 12, { 0, 0 } }, // click
        { 78,  6, { 0, 0 } }, // monitor
        { 110, 6, { 0, 2 } }, // count-in
    };

    juce::Component* const controls[] {
        &firstFrameButton, &previousFrameButton, &playPauseButton,
        &nextFrameButton, &lastFrameButton, &loopButton, &recordButton,
        &metronomeButton, &monitorButton, &countInBox_
    };

    // The rows the buttons need, taken off the top before anything below is
    // placed — so a wrapped row pushes the tempo and position readouts down
    // rather than drawing over them.
    const int buttonsHeight = app::wrappedRowHeight(row.getWidth(), 30, 4, items);
    auto      buttonsArea   = row.withHeight(buttonsHeight);
    area.removeFromTop(buttonsHeight - row.getHeight());

    const auto bounds = app::wrapRow(buttonsArea, 30, 4, items);
    for (size_t i = 0; i < bounds.size() && i < std::size(controls); ++i)
        controls[i]->setBounds(bounds[i]);

    area.removeFromTop(8);

    if (transportCollapsed_)
        return; // nothing below the button row is showing

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(6);

    // Below tempo, sharing its label gutter: they are the two things that
    // decide what a bar is.
    timeSigBox_.setBounds(area.removeFromTop(24).withTrimmedLeft(64).removeFromLeft(90));
}

/** Shows or hides everything below the transport's button row. The arrow
    points the way the content will go, so it reads the same whichever state
    it's in. */
void MainComponent::applyTransportCollapse()
{
    juce::Component* belowFirstRow[] = { &positionLabel, &clipLabel, &tempoSlider,
                                        &timeSigBox_, &timeSigLabel_ };
    for (auto* c : belowFirstRow)
        c->setVisible(! transportCollapsed_);

    collapseTransportButton_.setButtonText(transportCollapsed_ ? "v" : "^");
    collapseTransportButton_.setTooltip(transportCollapsed_ ? "Show tempo and position"
                                                            : "Hide tempo and position");
    layoutLeftPane();
}

/** Index of @p name in the workspace's panel list — the offset that turns a
    View-menu id back into a panel. Both directions go through
    registeredPanels(), so the mapping can't drift as panes are added. */
int MainComponent::panelMenuIndex(const juce::String& name) const
{
    const auto names = workspace_.registeredPanels();
    for (int i = 0; i < (int) names.size(); ++i)
        if (names[(size_t) i] == name)
            return i;
    return 0;
}

/** Opens, reveals or closes the panel at @p index in the workspace's list. */
void MainComponent::togglePanel(int index)
{
    const auto names = workspace_.registeredPanels();
    if (index < 0 || index >= (int) names.size())
        return;

    const auto& name = names[(size_t) index];

    if (! workspace_.isPanelOpen(name))
        workspace_.openPanel(name);
    else if (workspace_.isPanelActive(name))
        workspace_.closePanel(name);   // already in front: the click means close
    else
        workspace_.revealPanel(name);  // open but buried: bring it forward first

    saveDockLayout();
}

/** The arrangement the app ships with, and what "Reset Layout" restores.
    Built with the same split/add operations a user's drags produce, so there
    is nothing special about it — Files down the left, the arrangement above
    the note editor in the middle, the mixer on the right, and the transport
    plus keyboard across the bottom. Several panels are therefore visible at
    once out of the box; the rest (Synth, Drums) start as tabs alongside the
    ones they relate to. */
/** The arrangement a workspace with no saved layout gets.

    Built from the same definition the Layout menu uses (see
    layouts::buildWorkspaceLayout) rather than by a second sequence of splits
    here — two descriptions of "the default" would drift, and the imperative
    one couldn't be checked by a headless test. */
void MainComponent::buildDefaultDockLayout()
{
    if (! workspace_.restoreLayout(layouts::workspaceLayoutText(activeWorkspace_)))
    {
        // Only reachable if a built-in layout stopped parsing, which the
        // layout tests exist to prevent. An empty single region is still a
        // usable workspace: every pane is reachable from the View menu.
        workspace_.resetToSingleRegion();
        workspace_.addPanel(workspace_.rootRegion(), "Tracks");
    }
}

juce::String MainComponent::settingsKeyForWorkspace(layouts::Workspace workspace) const
{
    return juce::String("dockLayout.") + layouts::workspaceName(workspace);
}

void MainComponent::loadDockLayout()
{
    // Which layout was in use last. Absent on an existing install, where
    // Music Creation is right: it is the arrangement the app already had.
    const auto savedName = settings_.getValue("activeLayout");
    for (int i = 0; i < layouts::kNumWorkspaces; ++i)
    {
        const auto workspace = (layouts::Workspace) i;
        if (savedName == layouts::workspaceName(workspace))
            activeWorkspace_ = workspace;
    }

    // That layout's own saved arrangement, then the flat "dockLayout" key an
    // older build wrote (so an existing install keeps the workspace it had
    // rather than being reset), then the built-in default.
    //
    // A saved layout that no longer parses — an older format, or one naming a
    // panel this build doesn't have — falls through rather than leaving a
    // half-built workspace.
    if (workspace_.restoreLayout(settings_.getValue(settingsKeyForWorkspace(activeWorkspace_))))
        return;
    if (workspace_.restoreLayout(settings_.getValue("dockLayout")))
        return;

    buildDefaultDockLayout();
}

void MainComponent::saveDockLayout()
{
    // Both the active layout's own slot and the flat key: the flat one is
    // what an older build reads, so writing it keeps a downgrade from
    // landing on an empty workspace.
    settings_.setValue("dockLayout", workspace_.saveLayout());
    saveActiveWorkspaceLayout();
}

void MainComponent::saveActiveWorkspaceLayout()
{
    settings_.setValue(settingsKeyForWorkspace(activeWorkspace_), workspace_.saveLayout());
    settings_.setValue("activeLayout", layouts::workspaceName(activeWorkspace_));
    settings_.saveIfNeeded();
}

/** Switches workspace. The outgoing arrangement is saved into its own slot
    first, so coming back finds it as it was left rather than reset to the
    built-in default. */
void MainComponent::applyWorkspaceLayout(layouts::Workspace workspace)
{
    saveActiveWorkspaceLayout();

    activeWorkspace_ = workspace;

    // The incoming layout's remembered arrangement, or its built-in default
    // the first time it's used. restoreLayout deliberately doesn't broadcast
    // a layout change, so neither of these can write back over the slot we
    // just saved.
    const auto remembered = settings_.getValue(settingsKeyForWorkspace(workspace));
    if (! workspace_.restoreLayout(remembered))
        workspace_.restoreLayout(layouts::workspaceLayoutText(workspace));

    resized();
    saveActiveWorkspaceLayout();
    showStatus("Layout: " + juce::String(layouts::workspaceName(workspace)));
}

/** Builds one of the zoom controls: icon, slider and editable multiplier.

    Shared by the tracks and keys panes. They zoom different axes — time in
    one, pitch in the other — but the control is the same thing and reads the
    same way, so it is built in one place. */
void MainComponent::setUpZoomControls(juce::Component& parent, juce::DrawableButton& icon,
                                      juce::Slider& slider, juce::Slider& box,
                                      double minZoom, double maxZoom,
                                      const juce::String& tooltip,
                                      std::function<void(float)> onZoom)
{
    // A DrawableButton in ImageFitted mode, as every other SVG in this app
    // uses. Clicks are switched off: this labels the slider, it isn't a
    // control.
    auto magnifier = icons::fromSvg(icons::kMagnifier);
    icon.setImages(magnifier.get());
    icon.setInterceptsMouseClicks(false, false);
    icon.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    parent.addAndMakeVisible(icon);

    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setRange(minZoom, maxZoom, 0.0);
    // Zoom is multiplicative, so a linear track would put x1 a fifth of the
    // way along and give most of the travel to zooming in. Skewing about the
    // midpoint puts x1 in the middle, where it belongs.
    slider.setSkewFactorFromMidPoint(1.0);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setTooltip(tooltip);
    slider.onValueChange = [&slider, onZoom] { onZoom((float) slider.getValue()); };
    parent.addAndMakeVisible(slider);

    box.setSliderStyle(juce::Slider::LinearBar); // a text field with a drag, not a track
    box.setRange(minZoom, maxZoom, 0.0);
    box.setSkewFactorFromMidPoint(1.0);
    box.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 20);
    // "x1.00" rather than "1.00 x": JUCE's suffix appends, and a multiplier
    // reads as a multiplier only with the x in front.
    box.textFromValueFunction = [](double value) { return "x" + juce::String(value, 2); };
    box.valueFromTextFunction = [](const juce::String& text)
    {
        return text.retainCharacters("0123456789.").getDoubleValue();
    };
    box.setTooltip(tooltip + " - type a multiplier, or drag");
    box.onValueChange = [&box, onZoom] { onZoom((float) box.getValue()); };
    parent.addAndMakeVisible(box);
}

/** Applies a new pitch zoom to the keys pane and keeps its controls
    describing it. The roll stores a row count, so the zoom lands on the
    nearest achievable window and the controls are set from where it landed
    rather than from what was asked for. */
void MainComponent::setKeysZoom(float zoom)
{
    pianoRoll_.setPitchZoom(zoom);
    updateKeysZoomControls();
}

void MainComponent::updateKeysZoomControls()
{
    const double zoom = pianoRoll_.pitchZoom();
    keysZoomSlider_.setValue(zoom, juce::dontSendNotification);
    keysZoomBox_.setValue(zoom, juce::dontSendNotification);
}

/** Keeps the playhead in view while the keys pane is scrolled.

    Only while playing: scrolling the view out from under someone who is
    editing a stopped pattern would be worse than the problem it solves. The
    paging rule itself is scrollToFollow, which is JUCE-free and tested — it
    pages rather than centring, so the grid stays still while the playhead
    crosses it instead of sliding continuously under a fixed line. */
void MainComponent::followKeysPlayhead()
{
    if (! keysFollowButton_.getToggleState() || ! engine_.isPlaying())
        return;

    const int viewportWidth = keysViewport_.getMaximumVisibleWidth();
    const int contentWidth  = pianoRoll_.getWidth();
    if (viewportWidth <= 0 || contentWidth <= viewportWidth)
        return; // nothing to scroll

    const int current = keysViewport_.getViewPositionX();
    const int wanted  = scrollToFollow((int) pianoRoll_.playheadX(), current,
                                       viewportWidth, contentWidth, kKeysFollowMargin);

    if (wanted != current)
        keysViewport_.setViewPosition(wanted, keysViewport_.getViewPositionY());
}

/** Widens the grid and lets the viewport scroll it. Unlike pitch zoom, which
    the roll stores as a row count and snaps, this is continuous — the roll
    simply draws to whatever width it's given. */
void MainComponent::setKeysTimeZoom(float zoom)
{
    pianoRoll_.setTimeZoom(zoom);
    updateKeysTimeZoomControls();
    layoutEditTab();
}

void MainComponent::updateKeysTimeZoomControls()
{
    const double zoom = pianoRoll_.timeZoom();
    keysTimeZoomSlider_.setValue(zoom, juce::dontSendNotification);
    keysTimeZoomBox_.setValue(zoom, juce::dontSendNotification);
}

/** Applies a new timeline zoom and keeps the controls describing it. */
void MainComponent::setTimelineZoom(float zoom)
{
    arrangementView_.setZoom(zoom);
    // Kept in step so a breakpoint sits under the bar it belongs to, rather
    // than under whichever bar this pane happened to be scaled for.
    automationPane_.setZoom(zoom);
    updateZoomControls();
}

/** Mirrors the current zoom into both controls without either of them
    reporting it straight back as a user edit — they set each other, and the
    keyboard shortcuts set both. */
void MainComponent::updateZoomControls()
{
    const double zoom = arrangementView_.zoom();
    zoomSlider_.setValue(zoom, juce::dontSendNotification);
    zoomBox_.setValue(zoom, juce::dontSendNotification);
}

void MainComponent::layoutArrangeTab()
{
    auto area = arrangeTab_.getLocalBounds();

    auto toolbar = area.removeFromTop(28).reduced(4, 2);

    zoomIcon_.setBounds(toolbar.removeFromLeft(24));
    toolbar.removeFromLeft(2);
    zoomSlider_.setBounds(toolbar.removeFromLeft(120));
    toolbar.removeFromLeft(6);
    zoomBox_.setBounds(toolbar.removeFromLeft(56));
    toolbar.removeFromLeft(12);
    addClipButton_.setBounds(toolbar.removeFromLeft(90));
    toolbar.removeFromLeft(6);
    generateLoopButton_.setBounds(toolbar.removeFromLeft(120));

    arrangementViewport_.setBounds(area);
}

void MainComponent::layoutEditTab()
{
    auto area   = editTab_.getLocalBounds();
    auto header = area.removeFromTop(24);

    barsBox_.setBounds(header.removeFromRight(56).reduced(2, 0));
    barsLabel_.setBounds(header.removeFromRight(34));

    header.removeFromRight(10);
    keysZoomBox_.setBounds(header.removeFromRight(52).reduced(0, 2));
    keysZoomSlider_.setBounds(header.removeFromRight(80).reduced(2, 1));
    keysZoomIcon_.setBounds(header.removeFromRight(22).reduced(0, 1));

    header.removeFromRight(10);
    keysTimeZoomBox_.setBounds(header.removeFromRight(52).reduced(0, 2));
    keysTimeZoomSlider_.setBounds(header.removeFromRight(80).reduced(2, 1));
    keysTimeZoomIcon_.setBounds(header.removeFromRight(22).reduced(0, 1));

    header.removeFromRight(8);
    keysFollowButton_.setBounds(header.removeFromRight(72).reduced(0, 2));

    editingLabel_.setBounds(header.reduced(6, 0));

    keysViewport_.setBounds(area);

    // The roll is as tall as the pane — rows fill it, and pitch zoom decides
    // how many — and as wide as the time zoom asks for, which is what the
    // viewport then scrolls.
    const int visibleWidth = juce::jmax(1, keysViewport_.getMaximumVisibleWidth());
    pianoRoll_.setSize(juce::jmax(visibleWidth, pianoRoll_.preferredWidth(visibleWidth)),
                       juce::jmax(1, keysViewport_.getMaximumVisibleHeight()));
}

void MainComponent::layoutMixerView()
{
    auto area = mixerView_.getLocalBounds().reduced(10);
    if (area.isEmpty())
        return;

    auto toolbar = area.removeFromTop(28);
    addTrackButton.setBounds(toolbar.removeFromLeft(100));
    toolbar.removeFromLeft(6);
    addDrumTrackButton_.setBounds(toolbar.removeFromLeft(100));
    toolbar.removeFromLeft(6);
    addGuitarTrackButton_.setBounds(toolbar.removeFromLeft(100));
    toolbar.removeFromLeft(6);
    addBusTrackButton_.setBounds(toolbar.removeFromLeft(100));
    area.removeFromTop(8);

    // ---- per-track channel strips, filling the remaining width ----
    const int stripWidth = 96;
    const int gap        = 6;
    int       x          = area.getX();
    const int n           = trackCount();

    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip = trackStrips_[i];
        if (i < n)
        {
            strip->setBounds(x, area.getY(), stripWidth, area.getHeight());
            x += stripWidth + gap;
        }
    }
}

void MainComponent::layoutMasterPanel()
{
    auto masterArea = masterPanel_.getLocalBounds();

    auto masterRow = masterArea.removeFromTop(26);
    autoRecButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(6);
    autoClearButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(10);
    masterSlider.setBounds(masterRow.withTrimmedLeft(64));
    masterArea.removeFromTop(6);

    auto filterRow = masterArea.removeFromTop(26);
    filterButton.setBounds(filterRow.removeFromLeft(64));
    filterRow.removeFromLeft(6);
    filterModeBox_.setBounds(filterRow.removeFromLeft(104));
    filterRow.removeFromLeft(8);
    const int fw = juce::jmax(60, (filterRow.getWidth() - 8) / 2);
    filterCutoffSlider.setBounds(filterRow.removeFromLeft(fw));
    filterRow.removeFromLeft(8);
    filterResoSlider.setBounds(filterRow);
    masterArea.removeFromTop(6);

    auto delayRow = masterArea.removeFromTop(26);
    delayButton.setBounds(delayRow.removeFromLeft(70));
    delayRow.removeFromLeft(8);
    const int dw = juce::jmax(50, (delayRow.getWidth() - 16) / 3);
    delayTimeSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayFbSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayMixSlider.setBounds(delayRow);
    masterArea.removeFromTop(6);

    auto reverbRow = masterArea.removeFromTop(26);
    reverbButton.setBounds(reverbRow.removeFromLeft(70));
    reverbRow.removeFromLeft(8);
    const int rw = juce::jmax(50, (reverbRow.getWidth() - 16) / 3);
    reverbRoomSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbDampSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbMixSlider.setBounds(reverbRow);
    masterArea.removeFromTop(6);

    auto eqRow = masterArea.removeFromTop(26);
    eqButton.setBounds(eqRow.removeFromLeft(70));
    eqRow.removeFromLeft(8);
    const int ew = juce::jmax(50, (eqRow.getWidth() - 16) / 3);
    eqBassSlider.setBounds(eqRow.removeFromLeft(ew));
    eqRow.removeFromLeft(8);
    eqMidSlider.setBounds(eqRow.removeFromLeft(ew));
    eqRow.removeFromLeft(8);
    eqTrebleSlider.setBounds(eqRow);
    masterArea.removeFromTop(4);
    eqCurveView_.setBounds(masterArea.removeFromTop(48));
    masterArea.removeFromTop(6);

    auto sendRow = masterArea.removeFromTop(26);
    sendBusButton.setBounds(sendRow.removeFromLeft(70));
    sendRow.removeFromLeft(8);
    sendEffectTypeBox_.setBounds(sendRow.removeFromLeft(80));
    sendRow.removeFromLeft(8);
    const int sw           = juce::jmax(50, (sendRow.getWidth() - 16) / 3);
    const auto param1Bounds = sendRow.removeFromLeft(sw);
    sendRow.removeFromLeft(8);
    const auto param2Bounds = sendRow.removeFromLeft(sw);
    sendRow.removeFromLeft(8);
    // Reverb (room/damp) and delay (time/feedback) share the same two slots —
    // only one pair is visible at a time (see updateSendBusEffectVisibility).
    sendRoomSlider.setBounds(param1Bounds);
    sendDampSlider.setBounds(param2Bounds);
    sendDelayTimeSlider.setBounds(param1Bounds);
    sendDelayFbSlider.setBounds(param2Bounds);
    sendReturnSlider.setBounds(sendRow);
    masterArea.removeFromTop(8);

    meter_.setBounds(masterArea.removeFromTop(44));
}

} // namespace looper
