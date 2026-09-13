#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The application shell: construction and teardown, the timer, painting and
// top-level layout, device notifications and the status banner.

namespace soundsplice
{
MainComponent::MainComponent()
    : settings_(makeSettingsOptions())
{
    menuBar_.setModel(this);
    addAndMakeVisible(menuBar_);

    // Every command goes through one manager, so the menus and the keyboard
    // can't disagree about what a command is or whether it can be used right
    // now — see CommandTable.h. The key mappings listen on this component,
    // which is where keyPressed used to catch the same keys: a focused text
    // field still gets its own keys first.
    commandManager_.registerAllCommandsForTarget(this);
    commandManager_.setFirstCommandTarget(this);
    addKeyListener(commandManager_.getKeyMappings());
    setApplicationCommandManagerToWatch(&commandManager_);

    // Dockable workspace: a tree of tab groups, arranged entirely by dragging
    // tabs (see DockWorkspace). The panel registry below is the one place
    // that maps a panel's name to the Component behind it — the workspace
    // moves panels around by name from then on, including when restoring a
    // saved layout.
    addAndMakeVisible(workspace_);
    workspace_.onLayoutChanged = [this] { saveDockLayout(); };

    // ---- document: the same empty project File > New creates ----
    history_.reset(makeEmptySong());

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
    arrangementView_.setSnapToMarkers(settings_.getValue("snapToMarkers", "1") != "0");
    arrangementView_.setSnapToClipEdges(settings_.getValue("snapToClipEdges", "1") != "0");
    {
        // Anything unrecognised (a hand-edited file, a format from a later
        // build) falls back to bars and beats and 30 fps.
        const int format = settings_.getValue("timeFormat", "0").getIntValue();
        timeDisplay_.format = format >= (int) app::TimeFormat::BarsBeats && format <= (int) app::TimeFormat::Timecode
                                  ? (app::TimeFormat) format
                                  : app::TimeFormat::BarsBeats;

        const int fps = settings_.getValue("timecodeFps", "30").getIntValue();
        timeDisplay_.fps = (fps == 24 || fps == 25) ? fps : 30;

        arrangementView_.setTimeDisplay(timeDisplay_);
    }
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

    tempoSlider.onValueChange = [this] { setProjectTempo(tempoSlider.getValue()); };
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
        strip->onPanChange  = [this, i](float p)  { setTrackPan(i, p); };
        strip->onSelect     = [this, i]           { selectTrack(i); };
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
    // roll ----
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

    arrangeTab_.addAndMakeVisible(addClipButton_);
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

    arrangementView_.onTimeSelectionChanged = [this](const model::TimeSelection& selection)
    {
        setTimeSelection(selection);
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

    // The user's saved effect presets. App settings rather than the project:
    // a sound someone has dialled in is reached for across projects.
    userEffectPresets_ = model::deserializeUserPresets(settings_.getValue("effectPresets").toStdString());
    effectChain_.setUserPresets(userEffectPresets_);
    effectChain_.onPresetSaveRequested = [this](const model::EffectSlot& slot, int) { promptToSaveEffectPreset(slot); };
    effectChain_.onUserPresetDeleted   = [this](const std::string& effectId, const std::string& name)
    {
        deleteUserEffectPreset(effectId, name);
    };


    workspace_.registerPanel("Files", fileBrowser_);
    workspace_.registerPanel("Transport", leftPane_);
    workspace_.registerPanel("Tracks", arrangeTab_);
    workspace_.registerPanel("Keys", editTab_);
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

    arrangementView_.onClipStartTrimmed = [this](int trackIndex, int clipIndex, double newStartBeats)
    {
        trimClipStartTo(trackIndex, clipIndex, newStartBeats);
    };

    arrangementView_.onClipFadesChanged = [this](int trackIndex, int clipIndex, const engine::ClipFades& fades)
    {
        setClipFades(trackIndex, clipIndex, fades, "Set clip fade");
    };

    arrangementView_.onClipMenuRequested = [this](int trackIndex, int clipIndex)
    {
        showClipMenu(trackIndex, clipIndex);
    };

    arrangementView_.onMarkerMenuRequested   = [this](int markerId) { showMarkerMenu(markerId); };
    arrangementView_.onMarkerRenameRequested = [this](int markerId) { renameMarkerPrompt(markerId); };
    arrangementView_.onMarkerMoved = [this](int markerId, double startBeats) { moveMarkerTo(markerId, startBeats); };

    arrangementView_.onFileDropped = [this](const juce::File& file, double dropBeat, int trackIndex)
    {
        importAudioFileAtBeat(file, dropBeat, trackIndex);
    };

    // Mirror the initial document into the engine + UI.
    syncEngineTracks();
    engine_.setArmedTrack(0);
    refreshPianoRollForSelected();
    refreshEffectChainForSelected();
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

    // Set before the timer starts, so the first tick can't treat a recovery
    // file waiting to be offered as a stale one to clear away.
    recoveryPending_ = autosaveFile().existsAsFile();
    lastAutosaveMs_  = juce::Time::getMillisecondCounterHiRes();

    startTimerHz(30);

    // Offered once the window is up rather than from inside the constructor,
    // so the question has something to appear in front of.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)]
    {
        if (safe != nullptr)
            safe->offerAutosaveRecovery();
    });
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

    // A clean document leaves nothing to recover. A dirty one only gets here
    // past the discard prompt, which has already cleared it, so an autosave
    // that survives into the next launch is one a crash left behind.
    if (! hasUnsavedChanges() && ! recoveryPending_)
        discardAutosave();

    saveDockLayout();
    stopTimer();
    removeKeyListener(commandManager_.getKeyMappings());
    setApplicationCommandManagerToWatch(nullptr);
    menuBar_.setModel(nullptr);
    engine_.deviceManager().removeChangeListener(this);
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
    refreshEffectChainForSelected();
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
    fileBrowser_.setProjectRootFolder(history_.current().projectRootFolder.empty()
                                          ? juce::File{}
                                          : juce::File(history_.current().projectRootFolder));
    updateEditingLabel();
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
    autosaveIfDue();
    stopAtEndOfArrangement();

    addTrackButton.setEnabled(trackCount() < engine_.maxTracks());
    addClipButton_.setEnabled(selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount());

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

    // A sample grid counts at the device's rate, which can change under it.
    if (sampleRate > 0.0 && std::abs(timeDisplay_.sampleRate - sampleRate) > 1.0e-6)
    {
        timeDisplay_.sampleRate = sampleRate;
        arrangementView_.setTimeDisplay(timeDisplay_);
    }

    // The format chosen in the View menu leads and bars and beats follow (or
    // the clock does, when bars and beats are the choice): an editor wants the
    // time and a musician the bar, and both fit.
    const juce::String barBeat = juce::String::formatted("Bar %d  Beat %d", bb.bar, bb.beat);
    const juce::String time (app::formatPosition(timeDisplay_, seconds));
    const bool         byBar = timeDisplay_.format == app::TimeFormat::BarsBeats;

    positionLabel.setText((byBar ? barBeat : time) + "   |   " + (byBar ? time : barBeat)
                              + "   |   " + transportState,
                          juce::dontSendNotification);

    // The slider follows the document (undo, load), skipped while it is being
    // dragged, or it would fight the hand that is moving it.
    if (! tempoSlider.isMouseButtonDown())
    {
        const double bpm = history_.current().bpm;
        if (std::abs(tempoSlider.getValue() - bpm) > 1.0e-6)
            tempoSlider.setValue(bpm, juce::dontSendNotification);
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

    // The piano roll's playhead walks the pattern's own loop, so it needs the
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

} // namespace soundsplice
