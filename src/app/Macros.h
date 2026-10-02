#pragma once

#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "CommandTable.h"
#include "model/EffectParams.h"
#include "model/Favorites.h"

namespace soundsplice::macros
{
/**
    Macros (Audacity's Macros, REAPER's custom actions): a named list of
    steps run one after another on the selection, or - when every step is an
    effect - over a folder of files.

    A step is either a command from the command table, recorded by name, or
    a chain of effects with all their settings (what Apply Effects or a
    Favorite applies). Only commands that act at once can be steps: one
    that asks something first ("Normalize...") would stop a macro to ask
    every time it ran, and an effect chain is how a macro carries settings.

    Kept in the app's settings, like Favorites, and for the same reason: a
    way of working is reached for across projects.
*/
struct Step
{
    std::string                    command; // a command's name; empty for an effects step
    std::vector<model::EffectSlot> effects;

    bool isEffects() const noexcept { return command.empty(); }
    bool operator==(const Step&) const = default;
};

struct Macro
{
    std::string       name;
    std::vector<Step> steps;

    bool operator==(const Macro&) const = default;
};

/** Whether @p definition can be a step: it acts without asking anything,
    and isn't about macros or the app's own settings. */
inline bool recordable(const commands::Definition& definition)
{
    if (juce::String(definition.name).trimEnd().endsWith("..."))
        return false;
    switch (definition.id)
    {
        case commands::newProject:
        case commands::commandPalette:
        case commands::keyboardShortcuts:
        case commands::recordMacro:
        case commands::manageMacros:
        case commands::runMacroOnFiles:
        case commands::runScript:
        case commands::followSystemOutput:
            return false;
        default:
            return true;
    }
}

/** The command a step runs, or nullptr for an effects step or a command
    that no longer exists. */
inline const commands::Definition* commandFor(const Step& step)
{
    if (step.isEffects())
        return nullptr;
    for (const auto& definition : commands::all())
        if (step.command == definition.name)
            return &definition;
    return nullptr;
}

/** "Effects: Compressor, Reverb", or the command's name. */
inline juce::String describe(const Step& step)
{
    if (! step.isEffects())
        return juce::String(step.command).upToFirstOccurrenceOf("   ", false, false);

    juce::StringArray names;
    for (const auto& slot : step.effects)
    {
        if (slot.kind == model::EffectKind::Plugin)
            names.add(slot.plugin.name);
        else if (const auto* descriptor = model::descriptorFor(slot.kind))
            names.add(descriptor->name);
    }
    return "Effects: " + (names.isEmpty() ? juce::String("none") : names.joinIntoString(", "));
}

/** The effects a macro applies, in order, if that's all it does - what it
    takes to run it over files - or nothing if it has a command step. */
inline std::optional<std::vector<model::EffectSlot>> effectsOnly(const Macro& macro)
{
    std::vector<model::EffectSlot> chain;
    for (const auto& step : macro.steps)
    {
        if (! step.isEffects())
            return std::nullopt;
        chain.insert(chain.end(), step.effects.begin(), step.effects.end());
    }
    return chain;
}

/** @p macros with @p macro added, replacing one of the same name. */
inline std::vector<Macro> with(std::vector<Macro> macros, Macro macro)
{
    for (auto& existing : macros)
        if (existing.name == macro.name)
        {
            existing = std::move(macro);
            return macros;
        }
    macros.push_back(std::move(macro));
    return macros;
}

/** Written as XML; an effects step's chain is in the project format (as
    Favorites are), so every setting and plugin state round-trips exactly. */
inline juce::String serialize(const std::vector<Macro>& macros)
{
    juce::XmlElement root("SOUNDSPLICE_MACROS");
    root.setAttribute("version", 1);
    for (const auto& macro : macros)
    {
        auto* element = root.createNewChildElement("MACRO");
        element->setAttribute("name", juce::String(macro.name));
        for (const auto& step : macro.steps)
        {
            auto* stepElement = element->createNewChildElement("STEP");
            if (step.isEffects())
                stepElement->addTextElement(juce::String(model::serializeFavorites({ { "effects", step.effects } })));
            else
                stepElement->setAttribute("command", juce::String(step.command));
        }
    }
    return root.toString();
}

/** The macros in @p text; none for text that isn't any. */
inline std::vector<Macro> deserialize(const juce::String& text)
{
    std::vector<Macro> macros;
    const auto root = juce::parseXML(text);
    if (root == nullptr || ! root->hasTagName("SOUNDSPLICE_MACROS"))
        return macros;

    for (auto* element : root->getChildWithTagNameIterator("MACRO"))
    {
        Macro macro;
        macro.name = element->getStringAttribute("name").toStdString();
        for (auto* stepElement : element->getChildWithTagNameIterator("STEP"))
        {
            Step step;
            if (stepElement->hasAttribute("command"))
                step.command = stepElement->getStringAttribute("command").toStdString();
            else
            {
                const auto favorites = model::deserializeFavorites(stepElement->getAllSubText().toStdString());
                if (favorites.empty())
                    continue;
                step.effects = favorites.front().chain;
            }
            macro.steps.push_back(std::move(step));
        }
        if (! macro.name.empty())
            macros.push_back(std::move(macro));
    }
    return macros;
}

} // namespace soundsplice::macros
