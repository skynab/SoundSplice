#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ParametricEqView.h>

#include <cmath>

using namespace soundsplice;
using Type = engine::ParametricBand::Type;

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

TEST_CASE("The parametric EQ's points are dragged, added and removed on its curve", "[gui][eq]")
{
    juce::ScopedJuceInitialiser_GUI init;

    ParametricEqView view;
    view.setSize(400, 160);

    ParametricEqView::Bands bands {};
    bands[0] = { Type::Bell, 1000.0f, 0.0f, 1.0f };
    bands[1] = { Type::Notch, 60.0f, 0.0f, 10.0f };
    view.setBands(bands);

    ParametricEqView::Bands reported {};
    int                     changes = 0, starts = 0, ends = 0;
    view.onChanged   = [&](const ParametricEqView::Bands& b) { reported = b; ++changes; };
    view.onDragStart = [&] { ++starts; };
    view.onDragEnd   = [&] { ++ends; };

    // Drag the bell up and to the right: frequency and gain follow.
    const auto from = view.pointFor(0);
    const juce::Point<float> to(view.xForHz(4000.0f), view.yForDb(6.0f));
    view.mouseDown(eventAt(view, from, from));
    view.mouseDrag(eventAt(view, to, from));
    view.mouseUp(eventAt(view, to, from));

    REQUIRE(starts == 1);
    REQUIRE(ends == 1);
    REQUIRE(changes >= 1);
    REQUIRE(std::abs(reported[0].hz - 4000.0f) < 40.0f);
    REQUIRE(std::abs(reported[0].gainDb - 6.0f) < 0.3f);

    // A notch has no gain: dragging it up moves only its frequency.
    const auto notch = view.pointFor(1);
    const juce::Point<float> notchTo(view.xForHz(50.0f), view.yForDb(10.0f));
    view.mouseDown(eventAt(view, notch, notch));
    view.mouseDrag(eventAt(view, notchTo, notch));
    view.mouseUp(eventAt(view, notchTo, notch));
    REQUIRE(std::abs(reported[1].hz - 50.0f) < 2.0f);
    REQUIRE(reported[1].gainDb == 0.0f);

    // Double-click empty space: the next band that's off becomes a bell there.
    const juce::Point<float> empty(view.xForHz(300.0f), view.yForDb(-8.0f));
    view.mouseDoubleClick(eventAt(view, empty, empty, 2));
    REQUIRE(reported[2].type == Type::Bell);
    REQUIRE(std::abs(reported[2].hz - 300.0f) < 5.0f);
    REQUIRE(std::abs(reported[2].gainDb + 8.0f) < 0.3f);

    // Double-click a point: it turns off.
    const auto added = view.pointFor(2);
    view.mouseDoubleClick(eventAt(view, added, added, 2));
    REQUIRE(reported[2].type == Type::Off);
}
