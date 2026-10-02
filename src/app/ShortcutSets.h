#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "CommandTable.h"

namespace soundsplice::shortcutsets
{
/**
    Customised keyboard shortcuts: the commands whose keys differ from
    their defaults, saved in the app's settings and to files that can be
    shared.

    Not juce::KeyPressMappingSet::createXml, for two reasons. It records
    commands by their numeric id, and ours are an enum that grows in the
    middle as commands are added - a saved set would quietly move keys to the
    wrong commands after an update. And it records keys by their platform
    key codes and modifier flags, so a set made on Windows means something
    else on a Mac. Here a command is recorded by its name and a key by a
    portable description ("Cmd+Shift+Left": Cmd is Ctrl on Windows and Linux).
*/

namespace detail
{
    struct NamedKey
    {
        const char* name;
        int         code;
    };

    inline const std::vector<NamedKey>& namedKeys()
    {
        using K = juce::KeyPress;
        static const std::vector<NamedKey> keys {
            { "Space", K::spaceKey }, { "Escape", K::escapeKey }, { "Return", K::returnKey }, { "Tab", K::tabKey },
            { "Delete", K::deleteKey }, { "Backspace", K::backspaceKey }, { "Insert", K::insertKey },
            { "Up", K::upKey }, { "Down", K::downKey }, { "Left", K::leftKey }, { "Right", K::rightKey },
            { "PageUp", K::pageUpKey }, { "PageDown", K::pageDownKey }, { "Home", K::homeKey }, { "End", K::endKey },
            { "F1", K::F1Key }, { "F2", K::F2Key }, { "F3", K::F3Key }, { "F4", K::F4Key }, { "F5", K::F5Key },
            { "F6", K::F6Key }, { "F7", K::F7Key }, { "F8", K::F8Key }, { "F9", K::F9Key }, { "F10", K::F10Key },
            { "F11", K::F11Key }, { "F12", K::F12Key }, { "F13", K::F13Key }, { "F14", K::F14Key },
            { "F15", K::F15Key }, { "F16", K::F16Key },
            { "Num0", K::numberPad0 }, { "Num1", K::numberPad1 }, { "Num2", K::numberPad2 }, { "Num3", K::numberPad3 },
            { "Num4", K::numberPad4 }, { "Num5", K::numberPad5 }, { "Num6", K::numberPad6 }, { "Num7", K::numberPad7 },
            { "Num8", K::numberPad8 }, { "Num9", K::numberPad9 }, { "NumPlus", K::numberPadAdd },
            { "NumMinus", K::numberPadSubtract }, { "NumMultiply", K::numberPadMultiply },
            { "NumDivide", K::numberPadDivide }, { "NumPoint", K::numberPadDecimalPoint },
            { "NumEquals", K::numberPadEquals },
            { "Plus", '+' },
            { "Play", K::playKey }, { "Stop", K::stopKey }, { "FastForward", K::fastForwardKey },
            { "Rewind", K::rewindKey },
        };
        return keys;
    }
}

/** @p key as "Cmd+Shift+Left": modifiers in a fixed order, then the key. */
inline juce::String encodeKey(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    juce::StringArray parts;
    if (mods.isCommandDown())
        parts.add("Cmd");
   #if JUCE_MAC
    // On a Mac, Control is its own modifier; elsewhere it is Cmd.
    if (mods.isCtrlDown())
        parts.add("Ctrl");
   #endif
    if (mods.isAltDown())
        parts.add("Alt");
    if (mods.isShiftDown())
        parts.add("Shift");

    const int code = key.getKeyCode();
    juce::String name;
    for (const auto& named : detail::namedKeys())
        if (named.code == code)
        {
            name = named.name;
            break;
        }
    if (name.isEmpty())
    {
        if (code <= 32 || code >= 127)
            return {};
        name = juce::String::charToString((juce::juce_wchar) juce::CharacterFunctions::toUpperCase((juce::juce_wchar) code));
    }
    parts.add(name);
    return parts.joinIntoString("+");
}

/** The key @p text describes, or an invalid KeyPress if it isn't one. */
inline juce::KeyPress decodeKey(const juce::String& text)
{
    auto parts = juce::StringArray::fromTokens(text.trim(), "+", {});
    parts.trim();
    if (parts.isEmpty() || parts[parts.size() - 1].isEmpty())
        return {};

    int mods = 0;
    for (int i = 0; i < parts.size() - 1; ++i)
    {
        const auto& m = parts[i];
        if (m.equalsIgnoreCase("Cmd"))        mods |= juce::ModifierKeys::commandModifier;
        else if (m.equalsIgnoreCase("Ctrl"))  mods |= juce::ModifierKeys::ctrlModifier;
        else if (m.equalsIgnoreCase("Alt"))   mods |= juce::ModifierKeys::altModifier;
        else if (m.equalsIgnoreCase("Shift")) mods |= juce::ModifierKeys::shiftModifier;
        else return {};
    }

    const auto last = parts[parts.size() - 1];
    int code = -1;
    for (const auto& named : detail::namedKeys())
        if (last.equalsIgnoreCase(named.name))
        {
            code = named.code;
            break;
        }
    if (code < 0)
    {
        if (last.length() != 1 || last[0] <= 32 || last[0] >= 127)
            return {};
        code = (int) juce::CharacterFunctions::toUpperCase(last[0]);
    }
    return juce::KeyPress(code, juce::ModifierKeys(mods), 0);
}

/** One command's keys, where they differ from its defaults. Empty keys: the
    command's default shortcuts were taken away. */
struct Binding
{
    std::string                 command; // the command's name in the table
    std::vector<juce::KeyPress> keys;

    bool operator==(const Binding& other) const
    {
        return command == other.command && keys == other.keys;
    }
};

namespace detail
{
    inline bool sameKeys(std::vector<juce::KeyPress> a, std::vector<juce::KeyPress> b)
    {
        auto byCode = [](const juce::KeyPress& x, const juce::KeyPress& y)
        {
            return std::pair(x.getKeyCode(), x.getModifiers().getRawFlags()) < std::pair(y.getKeyCode(), y.getModifiers().getRawFlags());
        };
        std::sort(a.begin(), a.end(), byCode);
        std::sort(b.begin(), b.end(), byCode);
        return a == b;
    }
}

/** The commands in @p mappings whose keys aren't their defaults. */
inline std::vector<Binding> customised(const juce::KeyPressMappingSet& mappings)
{
    std::vector<Binding> bindings;
    for (const auto& definition : commands::all())
    {
        std::vector<juce::KeyPress> keys;
        for (const auto& key : mappings.getKeyPressesAssignedToCommand(definition.id))
            keys.push_back(key);
        if (! detail::sameKeys(keys, definition.keys))
            bindings.push_back({ definition.name, keys });
    }
    return bindings;
}

/** Sets @p mappings to the defaults with @p bindings over them. A binding
    for a command that no longer exists is skipped. */
inline void apply(juce::KeyPressMappingSet& mappings, const std::vector<Binding>& bindings)
{
    mappings.resetToDefaultMappings();
    for (const auto& binding : bindings)
    {
        for (const auto& definition : commands::all())
        {
            if (binding.command != definition.name)
                continue;
            mappings.clearAllKeyPresses(definition.id);
            for (const auto& key : binding.keys)
                if (key.isValid())
                    mappings.addKeyPress(definition.id, key);
            break;
        }
    }
}

inline juce::String toXml(const std::vector<Binding>& bindings)
{
    juce::XmlElement root("SOUNDSPLICE_SHORTCUTS");
    root.setAttribute("version", 1);
    for (const auto& binding : bindings)
    {
        auto* element = root.createNewChildElement("COMMAND");
        element->setAttribute("name", juce::String(binding.command));
        juce::StringArray keys;
        for (const auto& key : binding.keys)
            if (const auto text = encodeKey(key); text.isNotEmpty())
                keys.add(text);
        element->setAttribute("keys", keys.joinIntoString(" "));
    }
    return root.toString();
}

/** The bindings in @p xml, or nothing if it isn't a shortcut set. Keys that
    can't be read are dropped. */
inline std::optional<std::vector<Binding>> fromXml(const juce::String& xml)
{
    const auto root = juce::parseXML(xml);
    if (root == nullptr || ! root->hasTagName("SOUNDSPLICE_SHORTCUTS"))
        return std::nullopt;

    std::vector<Binding> bindings;
    for (auto* element : root->getChildWithTagNameIterator("COMMAND"))
    {
        Binding binding;
        binding.command = element->getStringAttribute("name").toStdString();
        for (const auto& text : juce::StringArray::fromTokens(element->getStringAttribute("keys"), " ", {}))
            if (const auto key = decodeKey(text); key.isValid())
                binding.keys.push_back(key);
        if (! binding.command.empty())
            bindings.push_back(std::move(binding));
    }
    return bindings;
}

} // namespace soundsplice::shortcutsets
