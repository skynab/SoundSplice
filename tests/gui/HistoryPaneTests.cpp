#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/HistoryPane.h>

using namespace soundsplice;

TEST_CASE("The History pane goes to steps, compares and switches branches", "[gui][historypane]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    HistoryPane pane;
    pane.setSize(400, 500);
    pane.setSteps({ { "Opened" }, { "Add track", true }, { "Delete clip", false, true } },
                  { { "Move clip", 2, 0 }, { "Old idea", 1, -1 } });

    int wentTo = -1, switched = -1, compared = -1;
    pane.onGoTo         = [&](int step) { wentTo = step; };
    pane.onSwitchBranch = [&](int branch) { switched = branch; };
    pane.onCompare      = [&](int step)
    {
        compared = step;
        return juce::StringArray { "Tempo 120 -> 96 BPM" };
    };

    const auto click = [&](const juce::String& text)
    {
        for (auto* child : pane.getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getButtonText() == text)
            {
                b->triggerClick();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
                return b->isEnabled();
            }
        return false;
    };

    pane.selectStepForTesting(0);
    click("Go Here");
    REQUIRE(wentTo == 0);

    click("Compare to Now");
    REQUIRE(compared == 0);
    REQUIRE(pane.comparisonForTesting().contains("Tempo 120 -> 96 BPM"));

    // The step that's now can't be gone to: it's where we are.
    wentTo = -1;
    pane.selectStepForTesting(1);
    click("Go Here");
    REQUIRE(wentTo == -1);

    // Branches: one off this line, one not.
    REQUIRE(switched == -1);
}
