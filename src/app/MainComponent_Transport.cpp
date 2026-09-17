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

/** Sets the project tempo as one undoable edit. Audio tracks keep their clips
    and automation at the same time in seconds, while instrument tracks stay
    on their beats — see model::retimeAudioForTempoChange. */
void MainComponent::setProjectTempo(double bpm)
{
    if (std::abs(history_.current().bpm - bpm) < 1.0e-9)
        return;

    history_.edit("Tempo", [bpm](model::Song& s)
    {
        model::retimeAudioForTempoChange(s, s.bpm, bpm);
        s.bpm = bpm;
    });

    pushTempoMap();

    // Audio clips' beat positions just changed, so the engine's windows and
    // the panes showing them have to follow.
    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
}

/** Hands the project tempo to the engine and the UI's own map, and refreshes
    everything that depends on where beats fall. */
void MainComponent::pushTempoMap()
{
    const auto& song = history_.current();

    uiTempoMap_.setTempo(song.bpm);
    post(Cmd::SetTempo, song.bpm);

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
