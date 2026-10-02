#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/PreferencesDialog.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(30); }
}

TEST_CASE("Preferences rows read their values, and write them through", "[gui][preferences]")
{
    JuceFixture fixture;

    bool   snap     = true;
    int    format   = 1;
    double latency  = 2.5;
    int    actions  = 0;

    std::vector<prefs::Page> pages;
    pages.push_back({ "Editing", {
        prefs::toggle("Snap", [&] { return snap; }, [&](bool on) { snap = on; }),
        prefs::choice("Format", { "A", "B", "C" }, [&] { return format; }, [&](int i) { format = i; }),
        prefs::number("Latency", -10.0, 10.0, 0.5, " ms", [&] { return latency; }, [&](double v) { latency = v; }),
    } });
    pages.push_back({ "Other", {
        prefs::heading("Things"),
        prefs::action("Do it", "Go", [&] { ++actions; }, [&] { return juce::String(actions) + " done"; }),
        prefs::folder("Home", [] { return juce::File::getSpecialLocation(juce::File::userHomeDirectory); }),
    } });

    PreferencesDialog dialog(std::move(pages));
    dialog.setSize(640, 520);
    REQUIRE(dialog.pageCount() == 2);

    auto& editing = dialog.pageForTesting(0);
    auto* toggle  = dynamic_cast<juce::ToggleButton*>(editing.controlForTesting(0));
    auto* combo   = dynamic_cast<juce::ComboBox*>(editing.controlForTesting(1));
    auto* slider  = dynamic_cast<juce::Slider*>(editing.controlForTesting(2));
    REQUIRE(toggle != nullptr);
    REQUIRE(combo != nullptr);
    REQUIRE(slider != nullptr);

    // Shown as they are.
    REQUIRE(toggle->getToggleState());
    REQUIRE(combo->getSelectedItemIndex() == 1);
    REQUIRE(slider->getValue() == 2.5);

    // Changed through the controls.
    toggle->triggerClick();
    pump();
    REQUIRE_FALSE(snap);

    combo->setSelectedItemIndex(2, juce::sendNotificationSync);
    REQUIRE(format == 2);

    slider->setValue(-4.0, juce::sendNotificationSync);
    REQUIRE(latency == -4.0);

    // Changed elsewhere (a menu), then shown again.
    snap   = true;
    format = 0;
    editing.refresh();
    REQUIRE(toggle->getToggleState());
    REQUIRE(combo->getSelectedItemIndex() == 0);

    // An action runs, and its status follows.
    auto& other  = dialog.pageForTesting(1);
    auto* button = dynamic_cast<juce::TextButton*>(other.controlForTesting(1));
    REQUIRE(button != nullptr);
    button->triggerClick();
    pump();
    REQUIRE(actions == 1);
}

TEST_CASE("A setter that refuses leaves the control showing what's true", "[gui][preferences]")
{
    // Recording's format can't change mid-take: the setter says no, and the
    // control has to go back rather than show a value that didn't happen.
    JuceFixture fixture;

    std::vector<prefs::Page> pages;
    pages.push_back({ "Recording", {
        prefs::choice("Bits", { "16", "24", "32" }, [] { return 1; }, [](int) { /* refused */ }),
    } });
    PreferencesDialog dialog(std::move(pages));
    auto* combo = dynamic_cast<juce::ComboBox*>(dialog.pageForTesting(0).controlForTesting(0));
    combo->setSelectedItemIndex(2, juce::sendNotificationSync);
    REQUIRE(combo->getSelectedItemIndex() == 1);
}

TEST_CASE("Preferences can be closed with a page showing a custom component", "[gui][preferences]")
{
    JuceFixture fixture;
    std::vector<prefs::Page> pages;
    pages.push_back({ "Devices", { prefs::custom([] { return std::make_unique<juce::Label>("x", "custom"); }, 120) } });
    pages.push_back({ "Empty", {} });
    {
        PreferencesDialog dialog(std::move(pages), 1);
        dialog.setSize(500, 400);
        REQUIRE(dialog.tabsForTesting().getCurrentTabIndex() == 1);
        REQUIRE(dialog.pageForTesting(0).controlForTesting(0) != nullptr);
    } // destroyed: the tabs' viewports go before the pages they show
    SUCCEED();
}
