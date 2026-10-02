#pragma once

#include <vector>

#include <juce_core/juce_core.h>

namespace soundsplice::screensets
{
/**
    Screensets (REAPER's): an arrangement of the panes saved under a name and
    put back from View > Layout, beside the built-in layouts. A screenset is
    the dock layout text DockWorkspace already saves and restores
    (DockLayoutTree.h's grammar), so it's whatever the workspace can show.

    Kept in the app's settings: an arrangement of the screen belongs to the
    person and the screen, not to a project.
*/
struct Screenset
{
    juce::String name;
    juce::String layout;

    bool operator==(const Screenset&) const = default;
};

/** @p list with @p set added, replacing one of the same name. */
inline std::vector<Screenset> with(std::vector<Screenset> list, Screenset set)
{
    for (auto& existing : list)
        if (existing.name == set.name)
        {
            existing = std::move(set);
            return list;
        }
    list.push_back(std::move(set));
    return list;
}

inline juce::String serialize(const std::vector<Screenset>& list)
{
    juce::XmlElement root("SCREENSETS");
    for (const auto& set : list)
    {
        auto* element = root.createNewChildElement("SCREENSET");
        element->setAttribute("name", set.name);
        element->setAttribute("layout", set.layout);
    }
    return root.toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
}

/** The screensets in @p text; none for text that isn't any. */
inline std::vector<Screenset> deserialize(const juce::String& text)
{
    std::vector<Screenset> list;
    const auto root = juce::parseXML(text);
    if (root == nullptr || ! root->hasTagName("SCREENSETS"))
        return list;
    for (auto* element : root->getChildWithTagNameIterator("SCREENSET"))
    {
        Screenset set { element->getStringAttribute("name"), element->getStringAttribute("layout") };
        if (set.name.isNotEmpty() && set.layout.isNotEmpty())
            list.push_back(std::move(set));
    }
    return list;
}

} // namespace soundsplice::screensets
