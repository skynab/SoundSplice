#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/TransferCurveView.h>

#include <cmath>

using namespace soundsplice;

namespace
{
    juce::MouseEvent eventAt(juce::Component& target, juce::Point<float> position, juce::Point<float> down,
                             int clicks = 1)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now    = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target,
                                &target, now, down, now, clicks, false);
    }
}

TEST_CASE("The transfer curve's points are dragged, added and removed", "[gui][dynamics]")
{
    juce::ScopedJuceInitialiser_GUI init;

    TransferCurveView view;
    view.setSize(220, 220);
    view.setCurve({}); // -80 -> -80 and 0 -> 0

    engine::TransferCurve reported;
    int                   starts = 0, ends = 0;
    view.onChanged   = [&](const engine::TransferCurve& curve) { reported = curve; };
    view.onDragStart = [&] { ++starts; };
    view.onDragEnd   = [&] { ++ends; };

    // Pull the top point down: 0 in to -10 out.
    const auto top = view.pointFor(1);
    const juce::Point<float> to(view.xForDb(0.0f), view.yForDb(-10.0f));
    view.mouseDown(eventAt(view, top, top));
    view.mouseDrag(eventAt(view, to, top));
    view.mouseUp(eventAt(view, to, top));
    REQUIRE(starts == 1);
    REQUIRE(ends == 1);
    REQUIRE(std::abs(reported.points[1].outDb + 10.0f) <= 1.0f);
    REQUIRE(std::abs(reported.points[1].inDb) <= 1.0f);

    // Add a knee at -20/-20.
    const juce::Point<float> knee(view.xForDb(-20.0f), view.yForDb(-20.0f));
    view.mouseDoubleClick(eventAt(view, knee, knee, 2));
    REQUIRE(reported.count == 3);
    REQUIRE(std::abs(reported.points[2].inDb + 20.0f) <= 1.0f);
    REQUIRE(std::abs(reported.outputDb(-10.0f) + 15.0f) <= 1.0f); // a 2:1 compressor now

    // Remove it again; two points are the least a curve keeps.
    view.mouseDoubleClick(eventAt(view, view.pointFor(2), view.pointFor(2), 2));
    REQUIRE(reported.count == 2);
    view.mouseDoubleClick(eventAt(view, view.pointFor(1), view.pointFor(1), 2));
    REQUIRE(reported.count == 2);
}
