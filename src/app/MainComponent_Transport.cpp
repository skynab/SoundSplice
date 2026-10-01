#include "MainComponentInternal.h"

#include "model/Timebase.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The transport: position, tempo, time signature, looping and seeking.

namespace soundsplice
{
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

/** The beat the playhead is on, for the tempo controls. */
double MainComponent::playheadBeat() const
{
    return juce::jmax(0.0, uiTempoMap_.ppqFromSamples(engine_.playheadSamples()));
}

/** The tempo the tempo control shows and sets: the starting tempo, or the
    tempo change in force at the playhead. */
double MainComponent::tempoAtPlayhead() const
{
    const auto&  song = history_.current();
    const double at   = model::tempoedit::changeInForceAt(song, playheadBeat());
    const int    i    = model::tempoedit::changeAt(song, at);
    return i >= 0 ? song.tempoChanges[(size_t) i].bpm : song.bpm;
}

/** Sets the tempo in force at the playhead - the starting tempo, or the
    change the playhead is past - as one undoable edit. Audio tracks keep
    their clips and automation at the same time in seconds, while
    instrument tracks stay on their beats: see model::tempoedit. */
void MainComponent::setProjectTempo(double bpm)
{
    if (std::abs(tempoAtPlayhead() - bpm) < 1.0e-9)
        return;

    const double at = model::tempoedit::changeInForceAt(history_.current(), playheadBeat());
    history_.edit("Tempo", [at, bpm](model::Song& s) { model::tempoedit::setTempo(s, at, bpm); });
    afterTempoEdit();
}

/** Everything that follows a change to the tempo map. */
void MainComponent::afterTempoEdit()
{
    pushTempoMap();

    // Audio clips' beat positions just changed, so the engine's windows and
    // the panes showing them have to follow.
    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Adds or edits the tempo change at @p beat, asking for the tempo. */
void MainComponent::editTempoChangeAt(double beat)
{
    const auto& song     = history_.current();
    const int   existing = model::tempoedit::changeAt(song, beat);
    const double current = existing >= 0 ? song.tempoChanges[(size_t) existing].bpm
                                         : model::tempoedit::tempoAt(song, beat);

    auto* window = new juce::AlertWindow(existing >= 0 ? "Edit Tempo Change" : "Add Tempo Change",
                                         "Bar " + juce::String((int) std::round(beat / beatsPerBar()) + 1),
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("bpm", juce::String(current, 2), "Tempo (BPM):");
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, beat](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double bpm = window->getTextEditorContents("bpm").getDoubleValue();
            if (bpm < 10.0 || bpm > 999.0)
            {
                self->showError("A tempo between 10 and 999 BPM, please");
                return;
            }
            self->history_.edit("Tempo change", [beat, bpm](model::Song& s) { model::tempoedit::setTempo(s, beat, bpm); });
            self->afterTempoEdit();
        }));
}

void MainComponent::removeTempoChangeAt(double beat)
{
    history_.edit("Remove tempo change", [beat](model::Song& s) { model::tempoedit::removeChange(s, beat); });
    afterTempoEdit();
}

void MainComponent::toggleTempoRamp(double beat)
{
    history_.edit("Tempo ramp", [beat](model::Song& s) { model::tempoedit::toggleRamp(s, beat); });
    afterTempoEdit();
}

void MainComponent::moveTempoChange(double from, double to)
{
    auto trial = history_.current();
    if (! model::tempoedit::moveChange(trial, from, to))
        return;
    history_.edit("Move tempo change", [from, to](model::Song& s) { model::tempoedit::moveChange(s, from, to); });
    afterTempoEdit();
}

/** Hands the tempo map to the engine and the UI's own map when it has
    changed, and refreshes everything that depends on where beats fall. */
void MainComponent::pushTempoMap()
{
    const auto& song = history_.current();

    const auto map = model::tempoedit::mapFor(song);
    if (map != pushedTempoMap_)
    {
        pushedTempoMap_ = map;
        uiTempoMap_.setTempoChanges(map);
        engine_.setTempoChanges(map);
    }

    // The loop region is a musical position, so where it lands in samples
    // changed with the tempo.
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

void MainComponent::setPlaySpeed(double speed)
{
    speed = engine::Varispeed::clampSpeed(speed);
    engine_.setPlaySpeed(speed);

    if (std::abs(speed - 1.0) < 1.0e-6)
        showStatus("Playing at normal speed");
    else
        showStatus("Playing at " + juce::String(speed, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".")
                   + "x speed (not while recording)");
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

/** The loop runs over the time selection when there is one, so the part
    being worked on plays round and round as it does in Audacity; otherwise
    over what has actually been arranged, rounded up to a bar.

    It used to be a hardcoded four bars whatever the song contained, so
    arranging anything longer than that silently looped only its opening —
    and arranging less looped several bars of nothing. */
void MainComponent::updateLoopRegion()
{
    const double sampleRate = engine_.sampleRate();
    if (sampleRate <= 0.0)
        return;

    uiTempoMap_.setSampleRate(sampleRate);

    const bool   selected = ! timeSelection_.isEmpty();
    const double from     = selected ? timeSelection_.startBeats : 0.0;
    const double to       = selected ? timeSelection_.endBeats : loopEndBeats();

    // Through the map, not a multiplication: a loop edge is a musical
    // position, and where it falls in samples depends on every tempo before it.
    post(Cmd::SetLoopRegion, (double) uiTempoMap_.samplesFromPpq(from), (double) uiTempoMap_.samplesFromPpq(to));
}

/** Where the arrangement ends, rounded up to a whole bar — an empty song
    still gets one bar, so the loop is never zero-length. */
double MainComponent::loopEndBeats() const
{
    return engine::loopEndForContent(songEndBeats(), juce::jmax(1.0, uiTempoMap_.quartersPerBar()));
}

} // namespace soundsplice
