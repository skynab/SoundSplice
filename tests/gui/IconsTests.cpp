#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/Icons.h>

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    struct NamedIcon { const char* name; const char* svg; };

    /** Every icon the app embeds. Adding one here is the point: a new icon
        that doesn't parse is otherwise invisible until someone looks at the
        control it belongs to. */
    const NamedIcon kAllIcons[] = {
        { "Magnifier",       looper::icons::kMagnifier },
        { "AudioOn",         looper::icons::kAudioOn },
        { "AudioDisabled",   looper::icons::kAudioDisabled },
        { "StarOn",          looper::icons::kStarOn },
        { "StarOutlineOff",  looper::icons::kStarOutlineOff },
        { "SidebarOn",       looper::icons::kSidebarOn },
        { "SidebarOff",      looper::icons::kSidebarOff },
        { "Play",            looper::icons::kPlay },
        { "Pause",           looper::icons::kPause },
        { "FirstFrame",      looper::icons::kFirstFrame },
        { "LastFrame",       looper::icons::kLastFrame },
        { "NextFrame",       looper::icons::kNextFrame },
        { "PreviousFrame",   looper::icons::kPreviousFrame },
        { "RecordButton",    looper::icons::kRecordButton },
        { "RecordStopButton",looper::icons::kRecordStopButton },
    };
}

TEST_CASE("Every embedded icon parses into a drawable", "[gui][icons]")
{
    // fromSvg returns nullptr on a malformed string and the control it
    // belongs to then draws nothing at all — silently, and only where someone
    // happens to look.
    JuceFixture fixture;

    for (const auto& icon : kAllIcons)
    {
        INFO("icon " << icon.name);
        auto drawable = looper::icons::fromSvg(icon.svg);
        REQUIRE(drawable != nullptr);
    }
}

TEST_CASE("Every icon has something in it to draw", "[gui][icons]")
{
    // Parsing isn't enough: well-formed SVG with no usable geometry gives a
    // Drawable with empty bounds, which is just as blank on screen.
    JuceFixture fixture;

    for (const auto& icon : kAllIcons)
    {
        auto drawable = looper::icons::fromSvg(icon.svg);
        REQUIRE(drawable != nullptr);

        const auto bounds = drawable->getDrawableBounds();
        INFO("icon " << icon.name << " bounds " << bounds.toString());
        REQUIRE(bounds.getWidth() > 0.0f);
        REQUIRE(bounds.getHeight() > 0.0f);
    }
}

TEST_CASE("A malformed icon is reported rather than crashing", "[gui][icons]")
{
    JuceFixture fixture;
    REQUIRE(looper::icons::fromSvg("not svg at all") == nullptr);
    REQUIRE(looper::icons::fromSvg("") == nullptr);
}
