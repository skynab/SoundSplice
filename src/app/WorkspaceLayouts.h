#pragma once

#include <memory>
#include <string>
#include <vector>

#include "DockLayoutTree.h"

namespace looper::layouts
{
/**
    The named arrangements of panes the app ships with.

    Fourteen panes are registered, and composing music and editing audio want
    almost disjoint subsets of them. One saved arrangement can't express both,
    so switching jobs meant rearranging by hand every time.

    Layouts are built as DockLayoutNode trees rather than written out as
    layout *strings*: the tree is what the rest of the app already speaks, and
    hand-writing the grammar for a five-way split is exactly the kind of thing
    that ships as a typo which silently fails to load. Building trees keeps it
    readable, and writeDockLayout turns them into the same text a user's own
    drags produce.

    JUCE-free, like DockLayoutTree itself, so a layout can be parsed and
    checked in the headless tests.
*/
enum class Workspace
{
    MusicCreation, // synths, drums, guitar, the piano roll — composing
    AudioEditing   // the waveform editor and the mastering rack
};

inline constexpr int kNumWorkspaces = 2;

inline const char* workspaceName(Workspace workspace)
{
    switch (workspace)
    {
        case Workspace::MusicCreation: return "Music Creation";
        case Workspace::AudioEditing:  return "Audio Editing";
    }
    return "Music Creation";
}

namespace detail
{
    inline std::unique_ptr<DockLayoutNode> leaf(std::vector<std::string> panels,
                                                std::string active = {})
    {
        auto node         = std::make_unique<DockLayoutNode>();
        node->panels      = std::move(panels);
        node->activePanel = std::move(active);
        return node;
    }

    /** Side by side: @p ratio is the share given to @p first. */
    inline std::unique_ptr<DockLayoutNode> beside(double ratio,
                                                  std::unique_ptr<DockLayoutNode> first,
                                                  std::unique_ptr<DockLayoutNode> second)
    {
        auto node        = std::make_unique<DockLayoutNode>();
        node->horizontal = true;
        node->ratio      = ratio;
        node->first      = std::move(first);
        node->second     = std::move(second);
        return node;
    }

    /** Stacked: @p ratio is the share given to @p first (the top). */
    inline std::unique_ptr<DockLayoutNode> above(double ratio,
                                                 std::unique_ptr<DockLayoutNode> first,
                                                 std::unique_ptr<DockLayoutNode> second)
    {
        auto node        = std::make_unique<DockLayoutNode>();
        node->horizontal = false;
        node->ratio      = ratio;
        node->first      = std::move(first);
        node->second     = std::move(second);
        return node;
    }
}

/** The tree for @p workspace. */
inline std::unique_ptr<DockLayoutNode> buildWorkspaceLayout(Workspace workspace)
{
    using namespace detail;

    if (workspace == Workspace::AudioEditing)
    {
        // The waveform gets the most room, because that's what's being worked
        // on. Tracks stays visible above it so a clip can still be selected
        // without switching away, and the mastering rack sits down the right
        // where its curve and meter are readable at a glance.
        return beside(0.16,
                      leaf({ "Files" }),
                      beside(0.70,
                             above(0.25,
                                   leaf({ "Tracks" }),
                                   above(0.80,
                                         leaf({ "Audio" }),
                                         leaf({ "Transport", "Keyboard" }, "Transport"))),
                             above(0.45,
                                   leaf({ "Mastering" }),
                                   above(0.55,
                                         leaf({ "Analyser" }),
                                         leaf({ "Master" })))));
    }

    // Music Creation — the arrangement the app shipped with as its only
    // default, kept exactly so switching to it lands somewhere familiar.
    return beside(0.18,
                  leaf({ "Files" }),
                  beside(0.72,
                         above(0.45,
                               leaf({ "Tracks" }),
                               beside(0.62,
                                      above(0.68,
                                            leaf({ "Keys", "Synth", "Drums", "Session", "Guitar" },
                                                 "Keys"),
                                            leaf({ "Transport", "Keyboard" }, "Transport")),
                                      leaf({ "Track FX" }))),
                         beside(0.72,
                                leaf({ "Mixer" }),
                                leaf({ "Master" }))));
}

/** The layout string for @p workspace, in the same grammar a saved layout
    uses — so switching is just DockWorkspace::restoreLayout. */
inline std::string workspaceLayoutText(Workspace workspace)
{
    const auto tree = buildWorkspaceLayout(workspace);
    return writeDockLayout(*tree);
}

/** Every panel named anywhere in @p workspace's layout.

    Exists for the tests: a layout naming a panel that isn't registered is
    rejected wholesale by DockWorkspace, so a typo here would ship as a
    layout that silently refuses to load and falls back. Checking the names
    against the registered set catches that at build time instead. */
inline std::vector<std::string> panelsInWorkspace(Workspace workspace)
{
    std::vector<std::string> names;

    const auto collect = [&names](const DockLayoutNode& node, const auto& recurse) -> void
    {
        if (node.isLeaf())
        {
            for (const auto& panel : node.panels)
                names.push_back(panel);
            return;
        }
        if (node.first != nullptr)  recurse(*node.first, recurse);
        if (node.second != nullptr) recurse(*node.second, recurse);
    };

    const auto tree = buildWorkspaceLayout(workspace);
    collect(*tree, collect);
    return names;
}

} // namespace looper::layouts
