#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"
#include "app/AutomationGeometry.h"
#include "app/TimelineGeometry.h"
#include "app/TrackColours.h"
#include "model/AutomationLane.h"
#include "model/Track.h"

namespace soundsplice
{
/**
    Draws and edits one track's automation curve.

    Automation has been real in the engine for a long time — recorded by
    touching a fader with Rec Auto armed, exported sample-accurately — but it
    was **write-only and invisible**: nothing drew a curve, nothing could move
    a breakpoint, and the only way to fix a bad move was to clear the lane and
    perform it again. This is the missing half.

    Its own pane rather than a row in the arrangement, deliberately. The
    arrangement already carries three drag protocols (clips, files from the
    browser, dock panels) whose separation is load-bearing enough to be
    documented; adding a fourth that shares a coordinate space with clip
    dragging is how you get a click that moves a clip when it meant to add a
    breakpoint. A pane has its own coordinate space and its own gestures, and
    the dockable workspace exists precisely so it can be on screen next to the
    arrangement rather than instead of it.

    The x axis comes from the same TimelineGeometry the arrangement uses, so a
    curve lines up with the bars above it rather than approximately with them.
*/
class AutomationPane final : public juce::Component
{
public:
    /** A gesture finished and the lane should be committed as one undo step.
        The lane is passed whole rather than as a delta: the edits here are
        add/move/remove on a sorted list, and replaying those against the
        document would mean implementing the same sort twice. */
    std::function<void(const AutomationTarget&, const model::AutomationLane&)> onLaneEdited;

    /** A drag is starting/ending. Used to make a whole drag one undo step, the
        same technique the mixer faders use. */
    std::function<void()> onEditGestureStart;
    std::function<void()> onEditGestureEnd;

    /** One entry in the parameter picker. */
    struct Target
    {
        AutomationTarget target;
        juce::String     name;
        juce::String     heading; // the picker's section, "" for the track's own
        AutomationRange  range;
    };

    /** The track's own parameters: what the picker offers before the owner
        hands over a track's effects. */
    static std::vector<Target> trackTargets()
    {
        return { { AutomationTarget::track(model::TrackParam::Gain), "Volume", {},
                   automationRangeFor(model::TrackParam::Gain) },
                 { AutomationTarget::track(model::TrackParam::Pan), "Pan", {},
                   automationRangeFor(model::TrackParam::Pan) } };
    }

    AutomationPane()
    {
        paramBox_.onChange = [this]
        {
            const int id = paramBox_.getSelectedId();
            if (id < 1 || id > (int) targets_.size())
                return;
            selected_ = (size_t) (id - 1);
            if (onParamChanged)
                onParamChanged(target());
            repaint();
        };
        setTargets(trackTargets());
        addAndMakeVisible(paramBox_);

        clearButton_.setButtonText("Clear");
        clearButton_.setTooltip("Remove every point on this lane");
        clearButton_.onClick = [this]
        {
            if (! hasTrack_)
                return;
            lane_.clear();
            commit();
        };
        addAndMakeVisible(clearButton_);

        hintLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        hintLabel_.setInterceptsMouseClicks(false, false);
        hintLabel_.setText("Click to add a point - drag to move - right-click for its curve - Alt-click to remove",
                           juce::dontSendNotification);
        addAndMakeVisible(hintLabel_);

        // Which track this is editing. The same complaint the arrangement's
        // track-identity headers answered: a curve with no name on it is a
        // curve you cannot be sure belongs to the track you think it does.
        nameLabel_.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        nameLabel_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(nameLabel_);
    }

    /** Fired when the parameter picker changes, so the owner can hand over
        that parameter's lane. */
    std::function<void(const AutomationTarget&)> onParamChanged;

    /** Replaces what the picker offers - the track's own parameters and its
        effects'. The selection stays on the same target if it's still
        offered, and falls back to the first otherwise. */
    void setTargets(std::vector<Target> targets)
    {
        const auto previous = targets_.empty() ? AutomationTarget {} : target();
        targets_  = targets.empty() ? trackTargets() : std::move(targets);
        selected_ = 0;
        for (size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].target == previous)
                selected_ = i;

        paramBox_.clear(juce::dontSendNotification);
        juce::String heading;
        for (size_t i = 0; i < targets_.size(); ++i)
        {
            if (targets_[i].heading.isNotEmpty() && targets_[i].heading != heading)
                paramBox_.addSectionHeading(targets_[i].heading);
            heading = targets_[i].heading;
            paramBox_.addItem(targets_[i].name, (int) i + 1);
        }
        paramBox_.setSelectedId((int) selected_ + 1, juce::dontSendNotification);
        repaint();
    }

    const AutomationTarget& target() const noexcept { return targets_[selected_].target; }

    /** Selects @p wanted if the picker offers it. */
    void selectTarget(const AutomationTarget& wanted)
    {
        for (size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].target == wanted)
                paramBox_.setSelectedId((int) i + 1, juce::sendNotificationSync);
    }

    /** Shows @p lane for @p trackName. @p totalBeats sizes the view. */
    void setLane(const juce::String& trackName, model::TrackType type,
                 const model::AutomationLane& lane, double totalBeats)
    {
        trackName_  = trackName;
        trackType_  = type;
        nameLabel_.setText(juce::String(trackTypeTag(type)) + "  " + trackName,
                           juce::dontSendNotification);
        lane_       = lane;
        totalBeats_ = juce::jmax(4.0, totalBeats);
        hasTrack_   = true;
        repaint();
    }

    void setNoTrackSelected()
    {
        nameLabel_.setText({}, juce::dontSendNotification);
        hasTrack_ = false;
        lane_.clear();
        repaint();
    }

    /** Keeps the horizontal scale in step with the arrangement. */
    void setZoom(float zoom)
    {
        timeline_.zoom = juce::jlimit(0.1f, 16.0f, zoom);
        repaint();
    }

    /** The playhead, in beats, so the curve can be read against what is
        currently sounding. */
    void setPlayheadBeat(double beat)
    {
        if (std::abs(beat - playheadBeat_) < 1.0e-9)
            return;
        playheadBeat_ = beat;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll(theme::surface(*this, theme::paneId));

        bounds.removeFromTop(kToolbarHeight);

        if (! hasTrack_)
        {
            g.setColour(juce::Colours::grey);
            g.drawText("Select a track to edit its automation", bounds,
                       juce::Justification::centred);
            return;
        }

        const auto lane  = laneBounds();
        const auto& range = currentRange();

        g.setColour(theme::surface(*this, theme::insetId));
        g.fillRect(lane);

        drawGrid(g, lane, range);
        drawCurve(g, lane, range);
        drawPlayhead(g, lane);

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(juce::String(range.topLabel), lane.getX() + 4, lane.getY() + 2, 120, 14,
                   juce::Justification::centredLeft);
        g.drawText(juce::String(range.bottomLabel), lane.getX() + 4, lane.getBottom() - 16, 120, 14,
                   juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).reduced(4, 3);
        paramBox_.setBounds(toolbar.removeFromLeft(190));
        toolbar.removeFromLeft(6);
        clearButton_.setBounds(toolbar.removeFromLeft(60));
        toolbar.removeFromLeft(10);
        nameLabel_.setBounds(toolbar.removeFromLeft(140));
        hintLabel_.setBounds(toolbar);
    }

    // ---- editing ----
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (! hasTrack_ || ! laneBounds().contains(event.getPosition()))
            return;

        const auto& range = currentRange();
        const auto lane  = laneBounds();

        const double beat  = beatForX((float) event.position.x);
        const int    index = pointUnder(event.position, lane, range);

        // Right-click offers the point's curve and its removal; Alt-click
        // removes straight away. Both checked before the add path so neither
        // can add a point on top of the one it was aimed at.
        if (event.mods.isPopupMenu())
        {
            if (index >= 0)
                showPointMenu(index);
            return;
        }

        if (event.mods.isAltDown())
        {
            if (index >= 0)
            {
                lane_.removePointAt(index);
                commit();
            }
            return;
        }

        if (onEditGestureStart)
            onEditGestureStart();

        if (index >= 0)
        {
            dragging_ = index;
        }
        else
        {
            // A click on empty space adds a point where the cursor is, and
            // leaves it grabbed — so the common gesture (click, then drag to
            // the value you wanted) is one movement rather than two.
            const float value = automation_.valueForY((float) event.position.y, range,
                                                      (float) lane.getY(),
                                                      (float) lane.getHeight());

            lane_.addPoint(beat, value);
            dragging_ = lane_.indexNear(beat, 1.0e-6);
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragging_ < 0)
            return;

        const auto& range = currentRange();
        const auto lane  = laneBounds();

        const double beat  = juce::jmax(0.0, beatForX((float) event.position.x));
        const float  value = automation_.valueForY((float) event.position.y, range,
                                                 (float) lane.getY(), (float) lane.getHeight());

        // The point may have been reordered past a neighbour, so the index it
        // moved to is what the rest of the drag has to follow.
        dragging_ = lane_.movePoint(dragging_, beat, value);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging_ < 0)
            return;

        dragging_ = -1;
        commit();

        if (onEditGestureEnd)
            onEditGestureEnd();
    }

    /** What the point menu's items do, by the ids showPointMenu gives them:
        kRemovePoint, or kShapeBase plus a CurveShape. Public as a testing
        seam, since the menu itself only answers asynchronously. */
    static constexpr int kRemovePoint = 1;
    static constexpr int kShapeBase   = 10;

    void applyPointMenuChoice(int index, int choice)
    {
        if (index < 0 || index >= (int) lane_.points().size())
            return;

        if (choice == kRemovePoint)
            lane_.removePointAt(index);
        else if (choice >= kShapeBase && choice <= kShapeBase + (int) model::CurveShape::SCurve)
            lane_.setShape(index, (model::CurveShape) (choice - kShapeBase));
        else
            return;

        commit();
    }

    // ---- testing seams ----
    const model::AutomationLane& laneForTesting() const noexcept { return lane_; }
    juce::Rectangle<int>         laneBoundsForTesting() const { return laneBounds(); }
    float                        xForBeatForTesting(double beat) const
    {
        return timeline_.xForBeat(beat);
    }

private:
    static constexpr int kToolbarHeight = 28;

    const AutomationRange& currentRange() const noexcept { return targets_[selected_].range; }

    juce::Rectangle<int> laneBounds() const
    {
        return getLocalBounds().withTrimmedTop(kToolbarHeight).reduced(2);
    }

    double beatForX(float x) const { return timeline_.beatForX(x); }

    int pointUnder(juce::Point<float> position, juce::Rectangle<int> lane,
                   const AutomationRange& range) const
    {
        const auto& points = lane_.points();

        for (int i = 0; i < (int) points.size(); ++i)
        {
            const float px = timeline_.xForBeat(points[(size_t) i].beat);
            const float py = automation_.yForValue(points[(size_t) i].value, range,
                                                 (float) lane.getY(), (float) lane.getHeight());

            if (automation_.hitsPoint(position.x, position.y, px, py))
                return i;
        }

        return -1;
    }

    void showPointMenu(int index)
    {
        const auto current = lane_.points()[(size_t) index].shape;

        juce::PopupMenu menu;
        menu.addItem(kRemovePoint, "Remove Point");
        menu.addSeparator();
        menu.addSectionHeader("Curve to the next point");
        const std::pair<model::CurveShape, const char*> shapes[] {
            { model::CurveShape::Linear, "Linear" },       { model::CurveShape::Hold, "Hold (step)" },
            { model::CurveShape::FastStart, "Fast Start" }, { model::CurveShape::SlowStart, "Slow Start" },
            { model::CurveShape::SCurve, "S-Curve" }
        };
        for (const auto& [shape, name] : shapes)
            menu.addItem(kShapeBase + (int) shape, name, true, shape == current);

        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                           [safe = juce::Component::SafePointer<AutomationPane>(this), index](int choice)
                           {
                               if (safe != nullptr)
                                   safe->applyPointMenuChoice(index, choice);
                           });
    }

    void commit()
    {
        repaint();
        if (onLaneEdited)
            onLaneEdited(target(), lane_);
    }

    void drawGrid(juce::Graphics& g, juce::Rectangle<int> lane, const AutomationRange& range) const
    {
        // The value the parameter has with no automation at all: the line a
        // flat lane sits on, and the one a curve is read as departing from.
        const float defaultY = automation_.yForValue(range.defaultValue, range,
                                                   (float) lane.getY(), (float) lane.getHeight());
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawHorizontalLine((int) defaultY, (float) lane.getX(), (float) lane.getRight());

        // Bar lines, matching the arrangement's.
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        for (int bar = 0; ; ++bar)
        {
            const float x = timeline_.xForBeat((double) bar * 4.0);
            if (x > (float) lane.getRight())
                break;
            if (x >= (float) lane.getX())
                g.drawVerticalLine((int) x, (float) lane.getY(), (float) lane.getBottom());
        }
    }

    void drawCurve(juce::Graphics& g, juce::Rectangle<int> lane, const AutomationRange& range) const
    {
        const auto& points = lane_.points();
        const auto  colour = juce::Colour(0xffffa726);

        if (points.empty())
        {
            // A flat line at the default, so an empty lane reads as "no
            // automation, sitting here" rather than as an empty panel that
            // might be broken.
            const float y = automation_.yForValue(range.defaultValue, range,
                                                (float) lane.getY(), (float) lane.getHeight());
            g.setColour(colour.withAlpha(0.35f));
            g.drawHorizontalLine((int) y, (float) lane.getX(), (float) lane.getRight());
            return;
        }

        juce::Path path;
        bool       started = false;

        const auto yFor = [&](float value)
        {
            return automation_.yForValue(value, range, (float) lane.getY(), (float) lane.getHeight());
        };

        // Before the first point the lane holds the first value, and after the
        // last it holds the last — drawn, because that hold is what actually
        // plays and a curve that started at the first point would misrepresent
        // the whole span before it.
        path.startNewSubPath((float) lane.getX(), yFor(points.front().value));
        started = true;

        // A shaped segment is drawn from the lane's own valueAt, sampled a few
        // pixels apart, so the line on screen is the one that plays.
        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto& point = points[i];
            const float x     = timeline_.xForBeat(point.beat);

            if (i > 0 && points[i - 1].shape == model::CurveShape::Hold)
            {
                path.lineTo(x, yFor(points[i - 1].value));
            }
            else if (i > 0 && points[i - 1].shape != model::CurveShape::Linear)
            {
                const auto& from  = points[i - 1];
                const float fromX = timeline_.xForBeat(from.beat);
                const int   steps = juce::jlimit(2, 256, (int) ((x - fromX) / 3.0f));
                for (int k = 1; k < steps; ++k)
                {
                    const double beat = from.beat + (point.beat - from.beat) * k / steps;
                    path.lineTo(timeline_.xForBeat(beat), yFor(lane_.valueAt(beat)));
                }
            }
            path.lineTo(x, yFor(point.value));
        }

        path.lineTo((float) lane.getRight(), yFor(points.back().value));

        if (started)
        {
            g.setColour(colour);
            g.strokePath(path, juce::PathStrokeType(1.5f));
        }

        for (int i = 0; i < (int) points.size(); ++i)
        {
            const float px = timeline_.xForBeat(points[(size_t) i].beat);
            const float py = yFor(points[(size_t) i].value);

            g.setColour(i == dragging_ ? juce::Colours::white : colour);
            g.fillEllipse(px - 3.5f, py - 3.5f, 7.0f, 7.0f);
        }
    }

    void drawPlayhead(juce::Graphics& g, juce::Rectangle<int> lane) const
    {
        const float x = timeline_.xForBeat(playheadBeat_);
        if (x < (float) lane.getX() || x > (float) lane.getRight())
            return;

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.drawVerticalLine((int) x, (float) lane.getY(), (float) lane.getBottom());
    }

    juce::ComboBox   paramBox_;
    juce::TextButton clearButton_;
    juce::Label      hintLabel_;
    juce::Label      nameLabel_;

    // One geometry per axis: beats-to-pixels is the arrangement's (so a curve
    // lines up with the bars above it), value-to-pixels is this pane's own.
    TimelineGeometry   timeline_;
    AutomationGeometry automation_;

    std::vector<Target>  targets_;
    size_t               selected_ = 0;
    model::AutomationLane lane_;
    juce::String         trackName_;
    model::TrackType     trackType_ = model::TrackType::Instrument;
    double               totalBeats_   = 64.0;
    double               playheadBeat_ = 0.0;
    bool                 hasTrack_     = false;
    int                  dragging_     = -1;
};

} // namespace soundsplice
