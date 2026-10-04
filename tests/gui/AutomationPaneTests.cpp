#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/AutomationPane.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** A pane with a track loaded and a real size, which is what makes its
        lane bounds non-empty and its mouse handling reachable. */
    void prepare(AutomationPane& pane, const model::AutomationLane& lane = {})
    {
        pane.setSize(800, 240);
        pane.setLane("Bass", model::TrackType::Audio, lane, 64.0);
    }

    juce::MouseEvent eventAt(juce::Component& component, juce::Point<float> position,
                             juce::ModifierKeys mods = juce::ModifierKeys())
    {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                                position, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                &component, &component, juce::Time::getCurrentTime(),
                                position, juce::Time::getCurrentTime(), 1, false);
    }
}

TEST_CASE("A click on empty lane space adds a point", "[gui][automation]")
{
    // The gesture the whole pane exists for: automation was write-only before
    // this, and there was no way to put a breakpoint anywhere by hand.
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    REQUIRE(pane.laneForTesting().points().empty());

    const auto lane = pane.laneBoundsForTesting();
    pane.mouseDown(eventAt(pane, { (float) lane.getCentreX(), (float) lane.getCentreY() }));

    REQUIRE(pane.laneForTesting().points().size() == 1);
}

TEST_CASE("A click outside the lane adds nothing", "[gui][automation]")
{
    // The toolbar is part of the component; clicking the parameter picker must
    // not drop a breakpoint behind it.
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    pane.mouseDown(eventAt(pane, { 40.0f, 6.0f })); // in the toolbar

    REQUIRE(pane.laneForTesting().points().empty());
}

TEST_CASE("Clicking with no track selected does nothing", "[gui][automation]")
{
    JuceFixture fixture;
    AutomationPane pane;
    pane.setSize(800, 240);
    pane.setNoTrackSelected();

    const auto lane = pane.laneBoundsForTesting();
    pane.mouseDown(eventAt(pane, { (float) lane.getCentreX(), (float) lane.getCentreY() }));

    REQUIRE(pane.laneForTesting().points().empty());
}

TEST_CASE("Dragging moves the point that was grabbed", "[gui][automation]")
{
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    const auto lane = pane.laneBoundsForTesting();
    const juce::Point<float> start { (float) lane.getCentreX(), (float) lane.getCentreY() };

    pane.mouseDown(eventAt(pane, start));
    const double beatBefore = pane.laneForTesting().points()[0].beat;
    const float  valueBefore = pane.laneForTesting().points()[0].value;

    pane.mouseDrag(eventAt(pane, start.translated(60.0f, -40.0f)));

    REQUIRE(pane.laneForTesting().points().size() == 1); // moved, not added
    const auto& moved = pane.laneForTesting().points()[0];
    REQUIRE(moved.beat > beatBefore);
    REQUIRE(moved.value > valueBefore); // dragged up = louder
}

TEST_CASE("Alt-clicking a point removes it", "[gui][automation]")
{
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    const auto lane = pane.laneBoundsForTesting();
    const juce::Point<float> at { (float) lane.getCentreX(), (float) lane.getCentreY() };

    pane.mouseDown(eventAt(pane, at));
    REQUIRE(pane.laneForTesting().points().size() == 1);

    pane.mouseDown(eventAt(pane, at, juce::ModifierKeys(juce::ModifierKeys::altModifier)));
    REQUIRE(pane.laneForTesting().points().empty());
}

TEST_CASE("A point's menu sets its curve or removes it, as one edit each", "[gui][automation]")
{
    JuceFixture fixture;
    AutomationPane pane;

    model::AutomationLane existing;
    existing.addPoint(0.0, -12.0f);
    existing.addPoint(8.0, 0.0f);
    prepare(pane, existing);

    int edits = 0;
    pane.onLaneEdited = [&](const AutomationTarget&, const model::AutomationLane&) { ++edits; };

    pane.applyPointMenuChoice(0, AutomationPane::kShapeBase + (int) model::CurveShape::Hold);
    REQUIRE(pane.laneForTesting().points()[0].shape == model::CurveShape::Hold);
    REQUIRE(pane.laneForTesting().valueAt(7.9) == -12.0f);
    REQUIRE(edits == 1);

    pane.applyPointMenuChoice(1, AutomationPane::kRemovePoint);
    REQUIRE(pane.laneForTesting().points().size() == 1);
    REQUIRE(edits == 2);

    pane.applyPointMenuChoice(5, AutomationPane::kRemovePoint); // no such point
    pane.applyPointMenuChoice(0, 0);                           // dismissed
    REQUIRE(edits == 2);
}

TEST_CASE("Right-clicking empty space adds nothing", "[gui][automation]")
{
    // Otherwise "remove" would silently become "add" whenever you missed.
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    const auto lane = pane.laneBoundsForTesting();
    pane.mouseDown(eventAt(pane, { (float) lane.getCentreX(), (float) lane.getCentreY() },
                           juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier)));

    REQUIRE(pane.laneForTesting().points().empty());
}

TEST_CASE("An edit is reported once, when the gesture ends", "[gui][automation]")
{
    // One undo step per gesture, not per pixel of drag — the same rule the
    // mixer faders follow.
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    int edits = 0;
    int starts = 0;
    int ends   = 0;
    pane.onLaneEdited       = [&](const AutomationTarget&, const model::AutomationLane&) { ++edits; };
    pane.onEditGestureStart = [&] { ++starts; };
    pane.onEditGestureEnd   = [&] { ++ends; };

    const auto lane = pane.laneBoundsForTesting();
    const juce::Point<float> at { (float) lane.getCentreX(), (float) lane.getCentreY() };

    pane.mouseDown(eventAt(pane, at));
    pane.mouseDrag(eventAt(pane, at.translated(20.0f, -10.0f)));
    pane.mouseDrag(eventAt(pane, at.translated(40.0f, -20.0f)));
    REQUIRE(edits == 0); // nothing committed mid-drag

    pane.mouseUp(eventAt(pane, at.translated(40.0f, -20.0f)));

    REQUIRE(starts == 1);
    REQUIRE(ends == 1);
    REQUIRE(edits == 1);
}

TEST_CASE("Clear empties the lane and reports it", "[gui][automation]")
{
    JuceFixture fixture;
    AutomationPane pane;

    model::AutomationLane existing;
    existing.addPoint(0.0, 0.0f);
    existing.addPoint(8.0, -6.0f);
    prepare(pane, existing);

    REQUIRE(pane.laneForTesting().points().size() == 2);

    int edits = 0;
    pane.onLaneEdited = [&](const AutomationTarget&, const model::AutomationLane& lane)
    {
        ++edits;
        REQUIRE(lane.empty());
    };

    // Reaching the button through the component tree, so this covers it being
    // parented and hooked up rather than merely constructed.
    juce::Button* clear = nullptr;
    for (auto* child : pane.getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child))
            clear = button;

    REQUIRE(clear != nullptr);
    clear->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);

    REQUIRE(pane.laneForTesting().points().empty());
    REQUIRE(edits == 1);
}

TEST_CASE("The pane parents and sizes its controls", "[gui][automation]")
{
    // The failure this project has hit before: controls laid out and shown
    // while never being parented, passing every other check.
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    REQUIRE(pane.getNumChildComponents() >= 4);

    for (auto* child : pane.getChildren())
    {
        REQUIRE(child->getParentComponent() == &pane);
        REQUIRE(child->isVisible());
        REQUIRE_FALSE(child->getBounds().isEmpty());
    }

    REQUIRE_FALSE(pane.laneBoundsForTesting().isEmpty());
}

TEST_CASE("The picker offers effect parameters and reports edits against them", "[gui][automation]")
{
    JuceFixture fixture;
    AutomationPane pane;
    prepare(pane);

    const auto cutoff = AutomationTarget::effect(0, model::EffectKind::Filter, "cutoff");
    auto       targets = AutomationPane::trackTargets();
    targets.push_back({ cutoff, "Cutoff", "1. Filter", { 20.0f, 18000.0f, "18000 Hz", "20 Hz", 1000.0f } });
    pane.setTargets(targets);
    REQUIRE(pane.target() == AutomationTarget::track(model::TrackParam::Gain));

    pane.selectTarget(cutoff);
    REQUIRE(pane.target() == cutoff);

    AutomationTarget edited;
    pane.onLaneEdited = [&](const AutomationTarget& target, const model::AutomationLane&) { edited = target; };
    const auto lane = pane.laneBoundsForTesting();
    const juce::Point<float> at { (float) lane.getCentreX(), (float) lane.getY() + 8.0f };
    pane.mouseDown(eventAt(pane, at));
    pane.mouseUp(eventAt(pane, at));
    REQUIRE(edited == cutoff);
    REQUIRE(pane.laneForTesting().points().front().value > 17000.0f); // near the top of its own range

    // Rebuilt with the same effect still there, the selection stays; with it
    // gone, it falls back to Volume rather than pointing at nothing.
    pane.setTargets(targets);
    REQUIRE(pane.target() == cutoff);
    pane.setTargets(AutomationPane::trackTargets());
    REQUIRE(pane.target() == AutomationTarget::track(model::TrackParam::Gain));
}
