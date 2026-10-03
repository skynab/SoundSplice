#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "app/AutomationLanes.h"
#include "engine/AutomationCurve.h"
#include "model/AutomationWriter.h"
#include "model/History.h"
#include "model/Song.h"

namespace soundsplice::app
{
/**
    Recording automation by moving controls during playback, in the mode the
    master panel's Automation picker sets (see model::AutomationMode), or the
    track's own where it has one (the mode button on its mixer strip).
    JUCE-free and engine-free, so it is tested: the caller says whether the
    transport is playing and where the playhead is.

    A *pass* is one stretch of playback that writes anything. It opens when a
    control is first moved (or, in Write mode, when playback starts), and
    closes when playback stops or the mode goes back to Read. While a control
    is being written, its lane is kept from the engine - the engine plays the
    control's own value, so what you hear is what you're doing rather than the
    lane you're replacing - and its moves go into the document's lane as the
    playhead passes (model::LaneWriter). Closing the pass makes the whole of it
    one undo step.
*/
class AutomationRecorder
{
public:
    explicit AutomationRecorder(model::History<model::Song>& history) : history_(history) {}

    /** A track's lanes started or stopped being kept from the engine: what
        the engine plays for it has to be pushed again (withoutWrittenLanes). */
    std::function<void(int trackIndex)> onEngineLanesChanged;

    model::AutomationMode mixMode() const noexcept { return mixMode_; }

    /** Sets the mix's mode. The caller closes the pass first: changing mode
        mid-pass ends the pass as it stands rather than reinterpreting what's
        already being written. */
    void setMixMode(model::AutomationMode mode) noexcept { mixMode_ = mode; }

    /** The mode @p trackIndex's controls record in: its own, or the mix's.
        The master lane (-1) always uses the mix's. */
    model::AutomationMode modeFor(int trackIndex) const
    {
        const auto& song = history_.current();
        if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
            return mixMode_;
        return model::effectiveAutomationMode(song.tracks[(size_t) trackIndex].automationMode, mixMode_);
    }

    /** True if any track is in Write mode, which writes for the whole of a pass. */
    bool anyTrackInWriteMode() const
    {
        for (int t = 0; t < (int) history_.current().tracks.size(); ++t)
            if (modeFor(t) == model::AutomationMode::Write)
                return true;
        return false;
    }

    bool isPassOpen() const noexcept { return passOpen_; }

    bool isWriting(const LaneKey& key) const
    {
        for (const auto& write : writes_)
            if (write.key == key && write.writer.active())
                return true;
        return false;
    }

    /** The keys on @p trackIndex's effect slot @p slot whose controls are
        held, for letting all of them go when a drag on the slot ends. */
    std::vector<LaneKey> heldOnSlot(int trackIndex, int slot) const
    {
        std::vector<LaneKey> held;
        for (const auto& write : writes_)
            if (write.key.track == trackIndex && write.key.target.slot == slot && write.touching)
                held.push_back(write.key);
        return held;
    }

    /** Takes out of @p curves, @p trackIndex's automation as the engine
        would play it, the lanes being written: the controls drive those. */
    void withoutWrittenLanes(int trackIndex, engine::TrackAutomation& curves) const
    {
        for (const auto& write : writes_)
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
    }

    /** A control named by @p key moved to @p value, or was grabbed there
        (@p touching), with the playhead at @p beat. Starts writing it if the
        mode and the transport say so. */
    void controlMoved(const LaneKey& key, float value, bool touching, bool playing, double beat)
    {
        const auto mode = modeFor(key.track);
        if (mode == model::AutomationMode::Read || ! playing)
            return;

        // In Touch mode only a held control writes: a nudge from the scroll
        // wheel or the keyboard has no release to end it.
        if (mode == model::AutomationMode::Touch && ! touching && ! isWriting(key))
            return;

        openPass(beat);

        auto& song = history_.mutableCurrent();
        auto* lane = automationlanes::laneFor(song, key);
        if (lane == nullptr)
            return;

        auto it = std::find_if(writes_.begin(), writes_.end(), [&](const Write& write) { return write.key == key; });
        if (it == writes_.end())
            it = writes_.insert(writes_.end(), Write { key });

        it->value    = value;
        it->touching = it->touching || touching;

        if (! it->writer.active())
        {
            it->writer.begin(*lane, beat, value);

            // From here the control drives the sound, not the lane.
            if (key.track >= 0)
                engineLanesChanged(key.track);
        }
    }

    /** The control named by @p key was let go, with the playhead at @p beat.
        In Touch mode that ends its writing; Latch and Write carry on with the
        last value. */
    void controlReleased(const LaneKey& key, double beat)
    {
        for (auto it = writes_.begin(); it != writes_.end(); ++it)
        {
            if (! (it->key == key))
                continue;

            it->touching = false;
            if (modeFor(key.track) != model::AutomationMode::Touch || ! it->writer.active())
                return;

            auto& song = history_.mutableCurrent();
            if (auto* lane = automationlanes::laneFor(song, key))
                it->writer.end(*lane, beat);
            writes_.erase(it);

            if (key.track >= 0 && key.track < (int) song.tracks.size())
                engineLanesChanged(key.track);
            return;
        }
    }

    /** From the UI timer: carries every write along to the playhead, and
        closes the pass when playback has stopped. True if that closed one. */
    bool tick(bool playing, double beat)
    {
        if (! playing)
            return closePass();

        if (anyTrackInWriteMode())
            openPass(beat);
        if (! passOpen_)
            return false;

        auto& song = history_.mutableCurrent();
        for (auto& write : writes_)
            if (auto* lane = automationlanes::laneFor(song, write.key); lane != nullptr && write.writer.active())
                write.writer.advance(*lane, beat, write.value);

        passBeat_ = beat;
        return false;
    }

    /** Ends every write where the playhead last was, and makes the pass one
        undo step: the lanes go back to how they were and the recorded ones
        are committed as a single edit. False if no pass was open. The caller
        pushes the document to the engine and the views afterwards. */
    bool closePass()
    {
        if (! passOpen_)
            return false;

        passOpen_ = false;

        auto& song = history_.mutableCurrent();
        for (auto& write : writes_)
            if (auto* lane = automationlanes::laneFor(song, write.key))
                write.writer.end(*lane, passBeat_);
        writes_.clear();

        const model::Song recorded = history_.current();
        automationlanes::copyLanes(before_, history_.mutableCurrent());
        before_ = {};

        // A pass that ended up writing nothing new isn't worth an undo step.
        if (automationlanes::lanesDiffer(recorded, history_.current()))
            history_.edit("Record automation", [&recorded](model::Song& s) { automationlanes::copyLanes(recorded, s); });
        return true;
    }

private:
    /** A control being written into its lane. */
    struct Write
    {
        LaneKey           key;
        model::LaneWriter writer;
        float             value    = 0.0f;
        bool              touching = false;
    };

    void openPass(double beat)
    {
        if (passOpen_)
            return;

        passOpen_ = true;
        before_   = history_.current();
        passBeat_ = beat;

        // Write mode takes every such track's volume and pan for the whole pass.
        const auto& song = history_.current();
        for (int t = 0; t < (int) song.tracks.size(); ++t)
        {
            if (modeFor(t) == model::AutomationMode::Write)
            {
                controlMoved(LaneKey::trackParam(t, model::TrackParam::Gain), song.tracks[(size_t) t].gainDb, false, true, beat);
                controlMoved(LaneKey::trackParam(t, model::TrackParam::Pan), song.tracks[(size_t) t].pan, false, true, beat);
            }
        }
    }

    void engineLanesChanged(int trackIndex)
    {
        if (onEngineLanesChanged)
            onEngineLanesChanged(trackIndex);
    }

    model::History<model::Song>& history_;
    model::AutomationMode        mixMode_  = model::AutomationMode::Read;
    std::vector<Write>           writes_;
    bool                         passOpen_ = false;
    model::Song                  before_;
    double                       passBeat_ = 0.0;
};

} // namespace soundsplice::app
