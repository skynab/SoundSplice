#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/PreviewStrip.h>

using namespace soundsplice;

TEST_CASE("The difference is what the effect took out", "[gui][preview]")
{
    const std::vector<std::vector<float>> original { { 1.0f, 0.5f, -0.25f }, { 0.0f, 0.0f, 1.0f } };
    const std::vector<std::vector<float>> processed { { 0.75f, 0.5f, 0.0f }, { 0.0f, 0.25f, 1.0f } };
    const auto diff = preview::difference(original, processed);
    REQUIRE(diff == std::vector<std::vector<float>> { { 0.25f, 0.0f, -0.25f }, { 0.0f, -0.25f, 0.0f } });

    // Nothing to subtract when the effect changed the length.
    REQUIRE(preview::difference(original, { { 1.0f }, { 1.0f } }).empty());
    REQUIRE(preview::difference(original, { { 1.0f, 0.5f, -0.25f } }).empty());
}

TEST_CASE("The Preview strip runs, switches, and stops when it goes", "[gui][preview]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    int previews = 0, switches = 0, stops = 0;
    preview::Mode last = preview::Processed;
    {
        PreviewStrip strip;
        strip.onPreview = [&](preview::Mode m) { ++previews; last = m; };
        strip.onSwitch  = [&](preview::Mode m) { ++switches; last = m; };
        strip.onStop    = [&] { ++stops; };

        const auto click = [&](const juce::String& text)
        {
            for (auto* child : strip.getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getButtonText() == text)
                    b->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        };

        // Switching before anything has been run plays nothing.
        click("Original");
        REQUIRE(switches == 0);
        REQUIRE(strip.mode() == preview::Original);

        click("Preview");
        REQUIRE(previews == 1);
        REQUIRE(last == preview::Original);

        click("Difference");
        REQUIRE(switches == 1);
        REQUIRE(last == preview::Difference);

        // An effect that changes the length has no difference to play.
        strip.setDifferenceAvailable(false);
        REQUIRE(strip.mode() == preview::Processed);

        click("Stop");
        REQUIRE(stops == 1);
    }
    REQUIRE(stops == 2); // and once more as the dialog goes
}
