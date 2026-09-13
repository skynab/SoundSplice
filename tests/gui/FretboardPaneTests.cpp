#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/FretboardPane.h>

using namespace looper;

namespace
{
    /** JUCE needs initialising once for any Component to be constructed. */
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** A pane in the state the app puts it in when a guitar track is selected. */
    std::unique_ptr<FretboardPane> makeReadyPane(int width, int height)
    {
        auto pane = std::make_unique<FretboardPane>();

        // A standalone Component is not visible by default, and
        // getComponentAt bails at the top if the component it is asked about
        // isn't. In the app the pane is visible inside a dock region; without
        // this the hit-test below reports nullptr for reasons that have
        // nothing to do with the pane.
        pane->setVisible(true);
        pane->setSize(width, height);
        pane->setSettings(model::GuitarSettings {}); // this is what reveals the content
        pane->resized();
        return pane;
    }

    /** Every chord-palette button the pane owns, found through the component
        tree rather than through a member — the point is to see what the pane
        actually presents. Matched by text against the known chord shapes
        rather than "every TextButton child", since the pane also owns
        unrelated TextButtons (the tone-template row) that aren't part of
        the chord palette this helper is about. */
    std::vector<juce::TextButton*> chordButtonsOf(juce::Component& pane)
    {
        std::vector<juce::TextButton*> found;
        for (int i = 0; i < pane.getNumChildComponents(); ++i)
        {
            if (auto* button = dynamic_cast<juce::TextButton*>(pane.getChildComponent(i)))
            {
                for (int s = 0; s < engine::kNumChordShapes; ++s)
                    if (button->getButtonText() == juce::String(engine::kChordShapes[s].name))
                    {
                        found.push_back(button);
                        break;
                    }
            }
        }
        return found;
    }
}

TEST_CASE("The chord buttons exist as children of the pane", "[gui][fretboard]")
{
    // The drive pedal shipped invisible because its controls were laid out and
    // shown while never being children at all. Nothing in the headless suite
    // could see that. This can.
    JuceFixture fixture;
    auto pane = makeReadyPane(900, 600);

    REQUIRE(chordButtonsOf(*pane).size() == (size_t) engine::kNumChordShapes);
}

TEST_CASE("Every chord button has a clickable area", "[gui][fretboard]")
{
    // A button with zero width or height is present, hit-tests against
    // nothing, and is indistinguishable from a button that doesn't work —
    // which is exactly what "the chord buttons don't do anything" describes.
    JuceFixture fixture;

    for (int width : { 900, 600, 400, 300, 240 })
    {
        auto pane = makeReadyPane(width, 600);
        const auto buttons = chordButtonsOf(*pane);
        REQUIRE(buttons.size() == (size_t) engine::kNumChordShapes);

        for (size_t i = 0; i < buttons.size(); ++i)
        {
            INFO("pane width " << width << ", button " << i
                 << " bounds " << buttons[i]->getBounds().toString());
            REQUIRE(buttons[i]->getWidth() > 0);
            REQUIRE(buttons[i]->getHeight() > 0);
            REQUIRE(buttons[i]->isVisible());
        }
    }
}

TEST_CASE("A click on a chord button reaches the pane's callback", "[gui][fretboard]")
{
    JuceFixture fixture;
    auto pane = makeReadyPane(900, 600);

    int stamped = 0;
    pane->onChordStamped = [&stamped](const engine::ChordShape&, int, const engine::StrumSettings&)
    {
        ++stamped;
    };

    // triggerClick() posts an async message, so the queue has to be pumped
    // before the callback runs. Button::mouseDown is protected, so a
    // synthesised press isn't available from outside — hit-testing is covered
    // separately by the getComponentAt test below, and between the two the
    // whole path from a click landing to the callback firing is accounted for.
    for (auto* button : chordButtonsOf(*pane))
        button->triggerClick();

    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);

    REQUIRE(stamped == engine::kNumChordShapes);
}

TEST_CASE("A chord button is what the pane hit-tests at its own centre", "[gui][fretboard]")
{
    // Bounds and visibility aren't enough on their own: something drawn over
    // the button would take the click instead, and the button would still
    // look fine by every other measure.
    JuceFixture fixture;
    auto pane = makeReadyPane(900, 600);

    for (auto* button : chordButtonsOf(*pane))
    {
        const auto centre = button->getBounds().getCentre();
        INFO("button at " << button->getBounds().toString());
        REQUIRE(pane->getComponentAt(centre) == button);
    }
}

TEST_CASE("The tuning row sits below the track header", "[gui][fretboard]")
{
    // paintTrackHeader() draws into the pane's own top strip (see paint());
    // this checks resized() actually reserved that space for it rather than
    // laying the tuning row underneath where the header is drawn.
    JuceFixture fixture;
    auto pane = makeReadyPane(700, 400);

    bool foundAny = false;
    for (int i = 0; i < pane->getNumChildComponents(); ++i)
    {
        if (auto* box = dynamic_cast<juce::ComboBox*>(pane->getChildComponent(i)))
        {
            foundAny = true;
            REQUIRE(box->getY() >= kTrackHeaderHeight);
        }
    }
    REQUIRE(foundAny); // otherwise the loop above would have proven nothing
}

TEST_CASE("The pane stays sane when it is far too small to draw", "[gui][fretboard]")
{
    // A docked pane can be dragged to any size. Nothing here should assert,
    // produce negative bounds, or crash.
    JuceFixture fixture;

    for (int height : { 400, 200, 120, 80, 40, 10 })
    {
        auto pane = makeReadyPane(400, height);
        for (auto* button : chordButtonsOf(*pane))
        {
            INFO("pane height " << height << " button " << button->getBounds().toString());
            REQUIRE(button->getWidth() >= 0);
            REQUIRE(button->getHeight() >= 0);
        }
    }
}

TEST_CASE("Chord buttons survive the heights a docked pane actually gets", "[gui][fretboard]")
{
    // The pane is a dock tab: its height is whatever the user's layout gives
    // it. If the chord row collapses to nothing at a plausible size, the
    // buttons are present, invisible and unclickable — which is precisely
    // what "the chord buttons don't do anything" would look like.
    JuceFixture fixture;

    for (int height : { 600, 400, 300, 250, 220, 200 })
    {
        auto pane = makeReadyPane(700, height);
        const auto buttons = chordButtonsOf(*pane);

        for (size_t i = 0; i < buttons.size(); ++i)
        {
            INFO("pane height " << height << ", button " << i
                 << " bounds " << buttons[i]->getBounds().toString());
            REQUIRE(buttons[i]->getHeight() > 0);
            REQUIRE(buttons[i]->getWidth() > 0);
            REQUIRE(buttons[i]->getBounds().getBottom() <= height);
        }
    }
}
