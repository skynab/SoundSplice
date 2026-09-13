#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/Shortcuts.h>

#include <string>

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };
}

TEST_CASE("Every shortcut parses to a real key", "[gui][shortcuts]")
{
    // A description with a typo in it — a wrong modifier name, a token JUCE
    // doesn't know — parses to an invalid KeyPress that matches nothing and
    // prints no shortcut text. Nothing fails to build, and the menu still
    // shows the command; it just quietly stops having a shortcut.
    JuceFixture fixture;

    for (const auto& shortcut : looper::keys::all())
    {
        INFO("shortcut for " << shortcut.name);
        REQUIRE(shortcut.key.isValid());
    }
}

TEST_CASE("Every shortcut can describe itself to a menu", "[gui][shortcuts]")
{
    // The menu prints this. An empty description is the visible symptom of
    // the failure above, so it is checked in its own right.
    JuceFixture fixture;

    for (const auto& shortcut : looper::keys::all())
    {
        INFO("shortcut for " << shortcut.name);
        REQUIRE(shortcut.key.getTextDescription().isNotEmpty());
        REQUIRE(shortcut.key.getTextDescriptionWithIcons().isNotEmpty());
    }
}

TEST_CASE("No two shortcuts are the same key", "[gui][shortcuts]")
{
    // keyPressed tests them in order, so a duplicate means the later one can
    // never fire — and it fires the wrong command instead, which is worse
    // than doing nothing. With this many bindings it is not obvious by
    // reading, which is the reason for the test.
    JuceFixture fixture;

    const auto shortcuts = looper::keys::all();

    for (size_t i = 0; i < shortcuts.size(); ++i)
    {
        for (size_t j = i + 1; j < shortcuts.size(); ++j)
        {
            INFO(shortcuts[i].name << " vs " << shortcuts[j].name
                 << " (" << shortcuts[i].key.getTextDescription() << ")");
            REQUIRE_FALSE(shortcuts[i].key == shortcuts[j].key);
        }
    }
}

TEST_CASE("Modifiers are what they claim to be", "[gui][shortcuts]")
{
    // Guards the specific mistake of a modifier being dropped by the parser:
    // cmd+shift+S landing as plain cmd+S would take over Save and Save As
    // would never fire, which is the collision test's worst case arriving
    // through a typo rather than a duplicate.
    JuceFixture fixture;

    REQUIRE(looper::keys::save.getModifiers().isCommandDown());
    REQUIRE_FALSE(looper::keys::save.getModifiers().isShiftDown());

    REQUIRE(looper::keys::saveAs.getModifiers().isCommandDown());
    REQUIRE(looper::keys::saveAs.getModifiers().isShiftDown());

    REQUIRE(looper::keys::copyTrack.getModifiers().isCommandDown());
    REQUIRE(looper::keys::copyTrack.getModifiers().isAltDown());
    REQUIRE_FALSE(looper::keys::copyTrack.getModifiers().isShiftDown());

    // The transport keys are deliberately unmodified, so a focused text field
    // consumes them first and they can't interrupt typing.
    REQUIRE(looper::keys::playPause.getModifiers().getRawFlags() == 0);
    REQUIRE(looper::keys::deleteTrack.getModifiers().getRawFlags() == 0);
}

TEST_CASE("The zoom shortcuts avoid the description parser", "[gui][shortcuts]")
{
    // KeyPress's parser splits descriptions on '+', so "command + -" is
    // ambiguous to it. These are built from key codes for that reason, and
    // this pins it: switching them back to descriptions would produce keys
    // that match nothing.
    JuceFixture fixture;

    REQUIRE(looper::keys::zoomIn.isValid());
    REQUIRE(looper::keys::zoomOut.isValid());
    REQUIRE(looper::keys::zoomIn.getModifiers().isCommandDown());
    REQUIRE(looper::keys::zoomOut.getModifiers().isCommandDown());
    REQUIRE_FALSE(looper::keys::zoomIn == looper::keys::zoomOut);
}

TEST_CASE("The two delete keys are different keys", "[gui][shortcuts]")
{
    // They exist to cover both keycaps. If they resolved to the same key one
    // of them would be pointless, and the collision test above would be the
    // thing that noticed.
    JuceFixture fixture;
    REQUIRE_FALSE(looper::keys::deleteTrack == looper::keys::deleteTrackAlt);
    REQUIRE_FALSE(looper::keys::deleteTrack == looper::keys::deleteClip); // that one has cmd
}
