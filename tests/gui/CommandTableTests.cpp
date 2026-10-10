#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/CommandTable.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    using KeyId = std::pair<int, int>; // key code, modifier flags

    KeyId keyId(const juce::KeyPress& key)
    {
        return { key.getKeyCode(), key.getModifiers().getRawFlags() };
    }

    int indexOf(commands::Id id)
    {
        const auto& table = commands::all();
        for (int i = 0; i < (int) table.size(); ++i)
            if (table[(size_t) i].id == id)
                return i;
        return -1;
    }
}

TEST_CASE("Every command has a unique id, a name, a category and a description", "[gui][commands]")
{
    JuceFixture fixture;

    const std::set<std::string> categories { "File", "Edit", "Transport", "Markers", "View", "Generate", "Analyze", "Tools" };
    std::set<int>               ids;

    for (const auto& command : commands::all())
    {
        INFO(command.name);
        REQUIRE(ids.insert(command.id).second);
        REQUIRE(command.id >= commands::kFirstId);
        REQUIRE_FALSE(std::string(command.name).empty());
        REQUIRE_FALSE(std::string(command.description).empty());
        REQUIRE(categories.count(command.category) == 1);
        REQUIRE(commands::find(command.id) == &command);
    }

    REQUIRE(commands::find(commands::kFirstId - 1) == nullptr);
}

TEST_CASE("Every shortcut belongs to a command, and every command key is a named shortcut", "[gui][commands]")
{
    // A shortcut in Shortcuts.h that no command uses is a key that does
    // nothing; a command key that isn't there escapes the shortcut tests.
    JuceFixture fixture;

    std::set<KeyId> commandKeys;
    for (const auto& command : commands::all())
        for (const auto& key : command.keys)
            commandKeys.insert(keyId(key));

    std::set<KeyId> namedKeys;
    for (const auto& shortcut : keys::all())
    {
        INFO("shortcut " << shortcut.name);
        namedKeys.insert(keyId(shortcut.key));
        REQUIRE(commandKeys.count(keyId(shortcut.key)) == 1);
    }

    for (const auto& command : commands::all())
    {
        for (const auto& key : command.keys)
        {
            INFO(command.name << " uses " << key.getTextDescription());
            REQUIRE(namedKeys.count(keyId(key)) == 1);
        }
    }
}

TEST_CASE("A shared shortcut goes to its context-specific command first", "[gui][commands]")
{
    // The command manager gives a key to the first enabled command in table
    // order. Several commands on one key is only right for the chains designed
    // that way, and only narrowest first: the other way round, the bare delete
    // key would take the track a selected clip is on.
    JuceFixture fixture;

    const std::vector<std::vector<commands::Id>> designedChains {
        { commands::deleteAudio, commands::deleteSelectedClip, commands::deleteTrack },
    };

    std::map<KeyId, std::vector<commands::Id>> owners;
    for (const auto& command : commands::all())
        for (const auto& key : command.keys)
            owners[keyId(key)].push_back(command.id);

    for (const auto& [key, ids] : owners)
    {
        if (ids.size() < 2)
            continue;

        INFO("key " << juce::KeyPress(key.first, juce::ModifierKeys(key.second), 0).getTextDescription()
             << " is shared by " << ids.size() << " commands");

        const auto chain = std::find(designedChains.begin(), designedChains.end(), ids);
        REQUIRE(chain != designedChains.end());
        for (size_t i = 1; i < chain->size(); ++i)
            REQUIRE(indexOf((*chain)[i - 1]) < indexOf((*chain)[i]));
    }
}
