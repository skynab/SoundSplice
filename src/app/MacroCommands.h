#pragma once

#include "CommandTable.h"
#include "Macros.h"

namespace soundsplice::macros
{
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
        case commands::preferences:
        case commands::projectInfo:
        case commands::exportCdImage:
        case commands::renderQueue:
        case commands::loadReference:
        case commands::transcribe:
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

} // namespace soundsplice::macros
