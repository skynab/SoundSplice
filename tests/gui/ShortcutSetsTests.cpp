#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ShortcutSets.h>

#include <set>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** Registers every command with its default keys, as MainComponent does. */
    struct Target final : juce::ApplicationCommandTarget
    {
        juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }

        void getAllCommands(juce::Array<juce::CommandID>& ids) override
        {
            for (const auto& definition : commands::all())
                ids.add(definition.id);
        }

        void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override
        {
            const auto* definition = commands::find(id);
            info.setInfo(definition->name, definition->description, definition->category, 0);
            for (const auto& key : definition->keys)
                info.addDefaultKeypress(key.getKeyCode(), key.getModifiers());
        }

        bool perform(const InvocationInfo&) override { return true; }
    };

    juce::KeyPress key(int code, int mods) { return juce::KeyPress(code, juce::ModifierKeys(mods), 0); }

    constexpr int cmd   = juce::ModifierKeys::commandModifier;
    constexpr int shift = juce::ModifierKeys::shiftModifier;
    constexpr int alt   = juce::ModifierKeys::altModifier;
}

TEST_CASE("A key is written portably and read back the same", "[gui][shortcutsets]")
{
    JuceFixture fixture;

    REQUIRE(shortcutsets::encodeKey(key('P', cmd | shift)) == "Cmd+Shift+P");
    REQUIRE(shortcutsets::encodeKey(key(juce::KeyPress::leftKey, alt)) == "Alt+Left");
    REQUIRE(shortcutsets::encodeKey(key(juce::KeyPress::spaceKey, 0)) == "Space");
    REQUIRE(shortcutsets::encodeKey(key('+', cmd)) == "Cmd+Plus");

    // Every default shortcut survives the trip - the names with spaces in
    // them are the ones KeyPress::createFromDescription gets wrong.
    for (const auto& named : keys::all())
    {
        INFO(named.name);
        const auto text = shortcutsets::encodeKey(named.key);
        REQUIRE(text.isNotEmpty());
        REQUIRE(shortcutsets::decodeKey(text) == named.key);
    }
    for (int f = 0; f < 12; ++f)
    {
        const auto k = key(juce::KeyPress::F1Key + f, shift);
        REQUIRE(shortcutsets::decodeKey(shortcutsets::encodeKey(k)) == k);
    }

    REQUIRE(shortcutsets::decodeKey("cmd+shift+p") == key('P', cmd | shift)); // any case
    REQUIRE_FALSE(shortcutsets::decodeKey("").isValid());
    REQUIRE_FALSE(shortcutsets::decodeKey("Cmd+").isValid());
    REQUIRE_FALSE(shortcutsets::decodeKey("Hyper+P").isValid());
    REQUIRE_FALSE(shortcutsets::decodeKey("Cmd+Banana").isValid());
}

TEST_CASE("Every command has a name no other command has", "[gui][shortcutsets]")
{
    // Shortcut sets record a command by its name.
    JuceFixture fixture;
    std::set<std::string> names;
    for (const auto& definition : commands::all())
    {
        INFO(definition.name);
        REQUIRE(names.insert(definition.name).second);
    }
}

TEST_CASE("Only the changed shortcuts are saved, and they come back", "[gui][shortcutsets]")
{
    JuceFixture fixture;

    Target                          target;
    juce::ApplicationCommandManager manager;
    manager.registerAllCommandsForTarget(&target);
    auto& mappings = *manager.getKeyMappings();

    REQUIRE(shortcutsets::customised(mappings).empty());

    // Normalize gets a key; Undo loses its; Zoom In gets a second one.
    mappings.addKeyPress(commands::normalizePeak, key('N', cmd | alt));
    mappings.clearAllKeyPresses(commands::undo);
    mappings.addKeyPress(commands::zoomIn, key(juce::KeyPress::numberPadAdd, 0));

    const auto custom = shortcutsets::customised(mappings);
    REQUIRE(custom.size() == 3);

    const auto xml  = shortcutsets::toXml(custom);
    const auto read = shortcutsets::fromXml(xml);
    REQUIRE(read.has_value());
    REQUIRE(*read == custom);

    // Into a fresh set of defaults.
    juce::ApplicationCommandManager other;
    other.registerAllCommandsForTarget(&target);
    auto& fresh = *other.getKeyMappings();
    shortcutsets::apply(fresh, *read);

    REQUIRE(fresh.containsMapping(commands::normalizePeak, key('N', cmd | alt)));
    REQUIRE(fresh.getKeyPressesAssignedToCommand(commands::undo).isEmpty());
    REQUIRE(fresh.containsMapping(commands::zoomIn, key(juce::KeyPress::numberPadAdd, 0)));
    REQUIRE(fresh.containsMapping(commands::zoomIn, keys::zoomIn));
    REQUIRE(fresh.containsMapping(commands::redo, keys::redo)); // untouched ones keep their defaults
    REQUIRE(shortcutsets::customised(fresh) == custom);

    // Applying an empty set puts every default back.
    shortcutsets::apply(fresh, {});
    REQUIRE(shortcutsets::customised(fresh).empty());
}

TEST_CASE("A shortcut file from elsewhere is read carefully", "[gui][shortcutsets]")
{
    JuceFixture fixture;

    REQUIRE_FALSE(shortcutsets::fromXml("").has_value());
    REQUIRE_FALSE(shortcutsets::fromXml("<KEYMAPPINGS/>").has_value());

    const auto read = shortcutsets::fromXml(
        "<SOUNDSPLICE_SHORTCUTS version=\"1\">"
        "<COMMAND name=\"Zoom In\" keys=\"Cmd+Up Nonsense+Q\"/>"
        "<COMMAND name=\"A Command From The Future\" keys=\"F9\"/>"
        "</SOUNDSPLICE_SHORTCUTS>");
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 2);
    REQUIRE((*read)[0].keys.size() == 1); // the unreadable key is dropped

    Target                          target;
    juce::ApplicationCommandManager manager;
    manager.registerAllCommandsForTarget(&target);
    shortcutsets::apply(*manager.getKeyMappings(), *read); // the unknown command is skipped
    REQUIRE(manager.getKeyMappings()->containsMapping(commands::zoomIn, key(juce::KeyPress::upKey, cmd)));
    REQUIRE(shortcutsets::customised(*manager.getKeyMappings()).size() == 1);
}
