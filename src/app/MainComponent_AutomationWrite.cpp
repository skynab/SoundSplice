#include "MainComponentInternal.h"

// Recording automation by moving controls during playback. What a pass is,
// and how its writes go into the lanes, is app::AutomationRecorder's; this is
// where it meets the engine, the transport and the views.

namespace soundsplice
{
namespace
{
    /** Where the playhead is, in beats, for writing into a lane. */
    double automationBeat(const engine::TempoMap& tempoMap, const engine::AudioEngine& engine)
    {
        return tempoMap.ppqFromSamples(engine.playheadSamples());
    }
}

/** Hooks the recorder up to the engine. Called once, from the constructor. */
void MainComponent::initAutomationRecorder()
{
    automation_.onEngineLanesChanged = [this](int trackIndex)
    {
        const auto& tracks = history_.current().tracks;
        if (trackIndex >= 0 && trackIndex < (int) tracks.size())
            engine_.setTrackAutomation(trackIndex, engineAutomationFor(trackIndex, tracks[(size_t) trackIndex]));
    };
}

model::AutomationMode MainComponent::automationModeFor(int trackIndex) const
{
    return automation_.modeFor(trackIndex);
}

bool MainComponent::isWritingAutomation(const LaneKey& key) const
{
    return automation_.isWriting(key);
}

/** @p track's automation as the engine should play it: without the lanes
    being written, which the controls are driving instead. */
engine::TrackAutomation MainComponent::engineAutomationFor(int trackIndex, const model::Track& track) const
{
    auto curves = toTrackAutomation(track);
    automation_.withoutWrittenLanes(trackIndex, curves);
    return curves;
}

/** A control named by @p key moved to @p value, or was grabbed there
    (@p touching). Starts writing it if the mode and the transport say so. */
void MainComponent::automationControlMoved(const LaneKey& key, float value, bool touching)
{
    automation_.controlMoved(key, value, touching, engine_.isPlaying(), automationBeat(uiTempoMap_, engine_));
}

/** The control named by @p key was let go. In Touch mode that ends its
    writing; Latch and Write carry on with the last value. */
void MainComponent::automationControlReleased(const LaneKey& key)
{
    automation_.controlReleased(key, automationBeat(uiTempoMap_, engine_));
}

/** From the UI timer: carries every write along to the playhead, and closes
    the pass when playback has stopped. */
void MainComponent::tickAutomationWrites()
{
    if (automation_.tick(engine_.isPlaying(), automationBeat(uiTempoMap_, engine_)))
        refreshAfterAutomationPass();
}

/** Ends the pass in progress, if there is one, as one undo step. */
void MainComponent::closeAutomationPass()
{
    if (automation_.closePass())
        refreshAfterAutomationPass();
}

/** The lanes a pass wrote, or put back, go to the engine and the views. */
void MainComponent::refreshAfterAutomationPass()
{
    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    refreshAutomationPaneForSelected();
}

void MainComponent::setAutomationMode(model::AutomationMode mode)
{
    if (mode == automation_.mixMode())
        return;

    // Changing mode mid-pass ends the pass as it stands rather than
    // reinterpreting what's already being written.
    closeAutomationPass();
    automation_.setMixMode(mode);
    updateMixerStrips(); // strips following the mix show its mode
}

/** Offers a track its own automation mode, or following the mix's. */
void MainComponent::chooseTrackAutomationMode(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    static const char* const names[] = { "Read", "Touch", "Latch", "Write" };
    const int current = song.tracks[(size_t) trackIndex].automationMode;

    juce::PopupMenu menu;
    menu.addSectionHeader("Automation mode");
    menu.addItem(1, juce::String("As the mix (") + names[(int) automation_.mixMode()] + ")", true, current < 0);
    for (int m = 0; m < 4; ++m)
        menu.addItem(10 + m, names[m], true, current == m);

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), trackIndex](int result)
        {
            if (self == nullptr || result == 0)
                return;

            // As for the mix's mode: a pass in progress ends as it stands.
            self->closeAutomationPass();
            const int mode = result == 1 ? -1 : result - 10;
            self->history_.edit("Set track automation mode", [trackIndex, mode](model::Song& s)
            {
                if (trackIndex >= 0 && trackIndex < (int) s.tracks.size())
                    s.tracks[(size_t) trackIndex].automationMode = mode;
            });
            self->updateMixerStrips();
        });
}

} // namespace soundsplice
