#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/CommandPalette.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    palette::Entry entry(const char* name, const char* category = "Edit", bool enabled = true)
    {
        palette::Entry e;
        e.name     = name;
        e.category = category;
        e.enabled  = enabled;
        e.key      = juce::String("cmd:") + name;
        return e;
    }

    std::vector<juce::String> names(const std::vector<palette::Entry>& entries, const std::vector<int>& order)
    {
        std::vector<juce::String> result;
        for (const int i : order)
            result.push_back(entries[(size_t) i].name);
        return result;
    }
}

TEST_CASE("The palette matches a command by the letters of its name, in order", "[gui][palette]")
{
    JuceFixture fixture;

    REQUIRE(palette::score("save", "Save Project") > 0);
    REQUIRE(palette::score("SAV AS", "Save Project As...") > 0);
    REQUIRE(palette::score("nrm", "Normalize...") > 0);
    REQUIRE(palette::score("", "Anything") == 0);

    REQUIRE(palette::score("sa", "Normalize...") < 0);     // there is no s
    REQUIRE(palette::score("zx", "Zoom In") < 0);
    REQUIRE(palette::score("a long query", "Undo") < 0);   // longer than the text
}

TEST_CASE("The palette ranks word starts, runs and short names first", "[gui][palette]")
{
    JuceFixture fixture;

    // Word starts beat letters in the middle of words.
    REQUIRE(palette::score("zi", "Zoom In") > palette::score("zi", "Normalize Inputs"));
    // A run beats scattered letters; initials count as well as a run.
    REQUIRE(palette::score("loop", "Loop") > palette::score("loop", "Ballroom Tip"));
    REQUIRE(palette::score("fp", "Fit Project") > palette::score("fp", "Soft Clip"));
    // The shorter of two otherwise equal matches comes first.
    REQUIRE(palette::score("zoom in", "Zoom In") > palette::score("zoom in", "Zoom In Further"));
}

TEST_CASE("Filtering orders by score, and the category can be searched too", "[gui][palette]")
{
    JuceFixture fixture;

    const std::vector<palette::Entry> entries { entry("Zoom to Selection", "View"), entry("Zoom In", "View"),
                                                entry("Normalize...", "Edit"), entry("Play / Pause", "Transport") };

    const auto zoom = names(entries, palette::filter(entries, "zoom in"));
    REQUIRE(zoom.size() >= 1);
    REQUIRE(zoom.front() == "Zoom In");

    const auto transport = names(entries, palette::filter(entries, "transport play"));
    REQUIRE(transport == std::vector<juce::String> { "Play / Pause" });

    REQUIRE(palette::filter(entries, "qqq").empty());
    REQUIRE(palette::filter(entries, "").size() == entries.size());
}

TEST_CASE("With nothing typed, the recently run commands come first", "[gui][palette]")
{
    JuceFixture fixture;

    const std::vector<palette::Entry> entries { entry("Undo"), entry("Redo"), entry("Normalize...") };
    auto recent = palette::noteRecent({}, "cmd:Redo");
    recent      = palette::noteRecent(recent, "cmd:Normalize...");

    REQUIRE(names(entries, palette::filter(entries, "", recent))
            == std::vector<juce::String> { "Normalize...", "Redo", "Undo" });

    // Running one again moves it to the front without a duplicate.
    recent = palette::noteRecent(recent, "cmd:Redo");
    REQUIRE(recent == juce::StringArray { "cmd:Redo", "cmd:Normalize..." });

    // And the list is kept short.
    juce::StringArray many;
    for (int i = 0; i < 30; ++i)
        many = palette::noteRecent(many, juce::String(i));
    REQUIRE(many.size() == 12);
    REQUIRE(many[0] == "29");
}

TEST_CASE("The palette runs the highlighted command, skipping unavailable ones", "[gui][palette]")
{
    JuceFixture fixture;

    juce::Component parent;
    parent.setSize(800, 600);
    CommandPalette palette;
    parent.addChildComponent(palette);
    palette.setBounds(80, 40, 640, 400);

    int ran = 0;
    std::vector<palette::Entry> entries { entry("Undo", "Edit", false), entry("Redo") };
    entries[0].run = [&] { ran = 1; };
    entries[1].run = [&] { ran = 2; };
    palette.onChosen = [](const palette::Entry& e) { e.run(); };

    palette.open(entries, {});
    REQUIRE(palette.isVisible());
    REQUIRE(palette.visibleCount() == 2);
    REQUIRE(palette.selectedRow() == 1); // Undo is unavailable, so Redo is highlighted

    palette.runSelected();
    REQUIRE(ran == 2);
    REQUIRE_FALSE(palette.isVisible());

    // Typing narrows the list; an unavailable entry can't be run.
    palette.open(entries, {});
    palette.searchBoxForTesting().setText("und", true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50); // the text change is posted
    REQUIRE(palette.visibleCount() == 1);
    ran = 0;
    palette.runSelected();
    REQUIRE(ran == 0);
    REQUIRE(palette.isVisible());
}
