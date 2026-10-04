#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/Theme.h>

#include <set>
#include <string>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };
}

TEST_CASE("Every theme has a distinct name and readable text", "[gui][theme]")
{
    JuceFixture fixture;

    std::set<std::string> names;
    for (const auto& t : theme::all())
    {
        INFO(t.name);
        REQUIRE(names.insert(t.name).second);
        REQUIRE(&theme::named(t.name) == &t);

        // The panes paint light text of their own: every surface has to be dark
        // enough for it, which is why there's no light theme.
        for (const auto surface : { t.pane, t.inset, t.workspace, t.tabInactive, t.popup })
            REQUIRE(surface.getPerceivedBrightness() < 0.45f);
        REQUIRE(t.scheme.getUIColour(juce::LookAndFeel_V4::ColourScheme::defaultText).getPerceivedBrightness() > 0.8f);
    }
    REQUIRE(&theme::named("No Such Theme") == &theme::all().front());
}

TEST_CASE("Panes take their surfaces from the theme", "[gui][theme]")
{
    JuceFixture fixture;

    juce::Component pane;

    // Without the app's look and feel (a test, a headless render): Nocturne's.
    REQUIRE(theme::surface(pane, theme::paneId) == theme::all().front().pane);
    REQUIRE(theme::surface(pane, theme::insetId) == theme::all().front().inset);

    AppLookAndFeel look;
    pane.setLookAndFeel(&look);
    for (const auto& t : theme::all())
    {
        INFO(t.name);
        look.apply(t, {}, false);
        REQUIRE(theme::surface(pane, theme::paneId) == t.pane);
        REQUIRE(theme::surface(pane, theme::workspaceId) == t.workspace);
        REQUIRE(theme::surface(pane, theme::tabActiveId) == t.tabActive);
        REQUIRE(look.findColour(juce::ResizableWindow::backgroundColourId)
                == t.scheme.getUIColour(juce::LookAndFeel_V4::ColourScheme::windowBackground));
        REQUIRE(look.drawsFocusRings() == t.focusRings);
    }
    pane.setLookAndFeel(nullptr);
}

TEST_CASE("Panes take their signal, warn and danger colours from the theme", "[gui][theme]")
{
    JuceFixture fixture;

    juce::Component pane;
    REQUIRE(theme::colour(pane, theme::signalId) == theme::all().front().signal);
    REQUIRE(theme::colour(pane, theme::dangerId) == theme::all().front().danger);

    AppLookAndFeel look;
    pane.setLookAndFeel(&look);
    for (const auto& t : theme::all())
    {
        INFO(t.name);
        look.apply(t, {}, false);
        REQUIRE(theme::colour(pane, theme::signalId) == t.signal);
        REQUIRE(theme::colour(pane, theme::signalInkId) == t.signalInk);
        REQUIRE(theme::colour(pane, theme::warnId) == t.warn);
        REQUIRE(theme::colour(pane, theme::dangerId) == t.danger);
        REQUIRE(theme::colour(pane, theme::dangerTextId) == t.dangerText);
        REQUIRE(theme::colour(pane, theme::accentId) == t.accent);
        REQUIRE(theme::colour(pane, theme::okId) == t.ok);
        REQUIRE(theme::colour(pane, theme::clipTopId) == t.clipTop);

        // Drawn on the panes, so they have to stand off them.
        for (const auto c : { t.signal, t.signalInk, t.warn, t.dangerText, t.ok })
            REQUIRE(c.getPerceivedBrightness() > t.pane.getPerceivedBrightness() + 0.3f);
    }

    look.apply(theme::all().front(), juce::Colour(0xffff8c42), false);
    REQUIRE(theme::colour(pane, theme::accentId) == juce::Colour(0xffff8c42));
    pane.setLookAndFeel(nullptr);
}

TEST_CASE("The accent is the theme's own until one is chosen", "[gui][theme]")
{
    JuceFixture fixture;
    AppLookAndFeel look;

    const auto& dark = theme::named("Nocturne");
    look.apply(dark, {}, false);
    REQUIRE(look.findColour(juce::Slider::thumbColourId) == dark.accent);

    look.apply(dark, juce::Colour(0xffff8c42), false);
    REQUIRE(look.findColour(juce::Slider::thumbColourId) == juce::Colour(0xffff8c42));
    REQUIRE(look.findColour(juce::ToggleButton::tickColourId) == juce::Colour(0xffff8c42));

    // Focus rings: asked for in any theme, always in High Contrast.
    REQUIRE_FALSE(look.drawsFocusRings());
    look.apply(dark, {}, true);
    REQUIRE(look.drawsFocusRings());
    look.apply(theme::named("High Contrast"), {}, false);
    REQUIRE(look.drawsFocusRings());
}
