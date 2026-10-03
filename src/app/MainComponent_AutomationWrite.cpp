#include "MainComponentInternal.h"

// Recording automation by moving controls during playback, in the mode the
// master panel's Automation picker sets (see model::AutomationMode), or the
// track's own where it has one (the mode button on its mixer strip).
//
// A *pass* is one stretch of playback that writes anything. It opens when a
// control is first moved (or, in Write mode, when playback starts), and
// closes when playback stops or the mode goes back to Read. While a control
// is being written, its lane is kept from the engine - the engine plays the
// control's own value, so what you hear is what you're doing rather than the
// lane you're replacing - and its moves go into the document's lane as the
// playhead passes (model::LaneWriter). Closing the pass makes the whole of it
// one undo step.

namespace soundsplice
{
using automationlanes::laneFor;

/** The mode @p trackIndex's controls record in: its own, or the mix's.
    The master lane (-1) always uses the mix's. */
model::AutomationMode MainComponent::automationModeFor(int trackIndex) const
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return automationMode_;
    return model::effectiveAutomationMode(song.tracks[(size_t) trackIndex].automationMode, automationMode_);
}

/** True if any track is in Write mode, which writes for the whole of a pass. */
bool MainComponent::anyTrackInWriteMode() const
{
    for (int t = 0; t < (int) history_.current().tracks.size(); ++t)
        if (automationModeFor(t) == model::AutomationMode::Write)
            return true;
    return false;
}

bool MainComponent::isWritingAutomation(const LaneKey& key) const
{
    for (const auto& write : automationWrites_)
        if (write.key == key && write.writer.active())
            return true;
    return false;
}

/** @p track's automation as the engine should play it: without the lanes
    being written, which the controls are driving instead. */
engine::TrackAutomation MainComponent::engineAutomationFor(int trackIndex, const model::Track& track) const
{
    auto curves = toTrackAutomation(track);
    for (const auto& write : automationWrites_)
    {
        if (write.key.track != trackIndex || ! write.writer.active())
            continue;

        const auto& target = write.key.target;
        if (! target.isEffect())
            (target.trackParam == model::TrackParam::Gain ? curves.gain : curves.pan) = {};
        else
            curves.effects.erase(std::remove_if(curves.effects.begin(), curves.effects.end(),
                                                [&](const engine::EffectParamCurve& curve)
                                                {
                                                    return curve.slot == target.slot && curve.paramId == target.paramId;
                                                }),
                                 curves.effects.end());
    }
    return curves;
}

void MainComponent::openAutomationPass()
{
    if (automationPassOpen_)
        return;

    automationPassOpen_   = true;
    automationPassBefore_ = history_.current();
    automationPassBeat_   = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());

    // Write mode takes every such track's volume and pan for the whole pass.
    const auto& song = history_.current();
    for (int t = 0; t < (int) song.tracks.size(); ++t)
    {
        if (automationModeFor(t) == model::AutomationMode::Write)
        {
            automationControlMoved(LaneKey::trackParam(t, model::TrackParam::Gain),
                                   song.tracks[(size_t) t].gainDb, false);
            automationControlMoved(LaneKey::trackParam(t, model::TrackParam::Pan),
                                   song.tracks[(size_t) t].pan, false);
        }
    }
}

/** A control named by @p key moved to @p value, or was grabbed there
    (@p touching). Starts writing it if the mode and the transport say so. */
void MainComponent::automationControlMoved(const LaneKey& key, float value, bool touching)
{
    const auto mode = automationModeFor(key.track);
    if (mode == model::AutomationMode::Read || ! engine_.isPlaying())
        return;

    // In Touch mode only a held control writes: a nudge from the scroll
    // wheel or the keyboard has no release to end it.
    if (mode == model::AutomationMode::Touch && ! touching && ! isWritingAutomation(key))
        return;

    openAutomationPass();

    auto& song = history_.mutableCurrent();
    auto* lane = laneFor(song, key);
    if (lane == nullptr)
        return;

    auto it = std::find_if(automationWrites_.begin(), automationWrites_.end(),
                           [&](const AutomationWrite& write) { return write.key == key; });
    if (it == automationWrites_.end())
        it = automationWrites_.insert(automationWrites_.end(), AutomationWrite { key });

    it->value    = value;
    it->touching = it->touching || touching;

    if (! it->writer.active())
    {
        it->writer.begin(*lane, uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), value);

        // From here the control drives the sound, not the lane.
        if (key.track >= 0)
            engine_.setTrackAutomation(key.track, engineAutomationFor(key.track, song.tracks[(size_t) key.track]));
    }
}

/** The control named by @p key was let go. In Touch mode that ends its
    writing; Latch and Write carry on with the last value. */
void MainComponent::automationControlReleased(const LaneKey& key)
{
    for (auto it = automationWrites_.begin(); it != automationWrites_.end(); ++it)
    {
        if (! (it->key == key))
            continue;

        it->touching = false;
        if (automationModeFor(key.track) != model::AutomationMode::Touch || ! it->writer.active())
            return;

        auto& song = history_.mutableCurrent();
        if (auto* lane = laneFor(song, key))
            it->writer.end(*lane, uiTempoMap_.ppqFromSamples(engine_.playheadSamples()));
        automationWrites_.erase(it);

        if (key.track >= 0 && key.track < (int) song.tracks.size())
            engine_.setTrackAutomation(key.track, engineAutomationFor(key.track, song.tracks[(size_t) key.track]));
        return;
    }
}

/** From the UI timer: carries every write along to the playhead, and closes
    the pass when playback has stopped. */
void MainComponent::tickAutomationWrites()
{
    if (! engine_.isPlaying())
    {
        closeAutomationPass();
        return;
    }

    if (anyTrackInWriteMode())
        openAutomationPass();
    if (! automationPassOpen_)
        return;

    const double beat = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
    auto&        song = history_.mutableCurrent();
    for (auto& write : automationWrites_)
        if (auto* lane = laneFor(song, write.key); lane != nullptr && write.writer.active())
            write.writer.advance(*lane, beat, write.value);

    automationPassBeat_ = beat;
}

/** Ends every write where the playhead last was, and makes the pass one undo
    step: the lanes go back to how they were and the recorded ones are
    committed as a single edit. */
void MainComponent::closeAutomationPass()
{
    if (! automationPassOpen_)
        return;

    automationPassOpen_ = false;

    auto& song = history_.mutableCurrent();
    for (auto& write : automationWrites_)
        if (auto* lane = laneFor(song, write.key))
            write.writer.end(*lane, automationPassBeat_);
    automationWrites_.clear();

    const model::Song recorded = history_.current();
    automationlanes::copyLanes(automationPassBefore_, history_.mutableCurrent());
    automationPassBefore_ = {};

    // A pass that ended up writing nothing new isn't worth an undo step.
    if (automationlanes::lanesDiffer(recorded, history_.current()))
        history_.edit("Record automation", [&recorded](model::Song& s) { automationlanes::copyLanes(recorded, s); });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    refreshAutomationPaneForSelected();
}

void MainComponent::setAutomationMode(model::AutomationMode mode)
{
    if (mode == automationMode_)
        return;

    // Changing mode mid-pass ends the pass as it stands rather than
    // reinterpreting what's already being written.
    closeAutomationPass();
    automationMode_ = mode;
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
    menu.addItem(1, juce::String("As the mix (") + names[(int) automationMode_] + ")", true, current < 0);
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
