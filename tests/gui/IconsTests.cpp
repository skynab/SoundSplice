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
        { "Magnifier",       soundsplice::icons::kMagnifier },
        { "AudioOn",         soundsplice::icons::kAudioOn },
        { "AudioDisabled",   soundsplice::icons::kAudioDisabled },
        { "StarOn",          soundsplice::icons::kStarOn },
        { "StarOutlineOff",  soundsplice::icons::kStarOutlineOff },
        { "SidebarOn",       soundsplice::icons::kSidebarOn },
        { "SidebarOff",      soundsplice::icons::kSidebarOff },
        { "Play",            soundsplice::icons::kPlay },
        { "Pause",           soundsplice::icons::kPause },
        { "FirstFrame",      soundsplice::icons::kFirstFrame },
        { "LastFrame",       soundsplice::icons::kLastFrame },
        { "NextFrame",       soundsplice::icons::kNextFrame },
        { "PreviousFrame",   soundsplice::icons::kPreviousFrame },
        { "RecordButton",    soundsplice::icons::kRecordButton },
        { "RecordStopButton",soundsplice::icons::kRecordStopButton },
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
        auto drawable = soundsplice::icons::fromSvg(icon.svg);
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
        auto drawable = soundsplice::icons::fromSvg(icon.svg);
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
    REQUIRE(soundsplice::icons::fromSvg("not svg at all") == nullptr);
    REQUIRE(soundsplice::icons::fromSvg("") == nullptr);
}
