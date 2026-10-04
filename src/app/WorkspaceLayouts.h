#pragma once

#include <memory>
#include <string>
#include <vector>

#include "DockLayoutTree.h"

namespace soundsplice::layouts
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
    MusicCreation, // the piano roll, session grid and mixer — composing
    AudioEditing   // the waveform editor and the mastering rack
};

inline constexpr int kNumWorkspaces = 2;

/** What the toolbar and the menus call @p workspace: the mockups name them
    for their editor. */
inline const char* workspaceTitle(Workspace workspace)
{
    return workspace == Workspace::AudioEditing ? "Waveform" : "Multitrack";
}

/** The name @p workspace's arrangement is saved under. Kept as it was when
    the workspaces were named for their jobs, so a saved arrangement is still
    found. */
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
        // Waveform, as its mockup lays it out: the files down the left, the
        // editor taking the middle, and down the right the history over the
        // amplitude readings. The timeline is a tab behind the editor, so a
        // clip can still be picked without switching away; the mastering
        // rack is a tab behind the history, which the toolbar's Effects Rack
        // brings forward.
        return beside(0.17,
                      above(0.68,
                            leaf({ "Open Files", "Files" }, "Open Files"),
                            leaf({ "Diagnostics", "Transport" }, "Diagnostics")),
                      beside(0.78,
                             leaf({ "Audio", "Tracks", "Transcript", "Video" }, "Audio"),
                             above(0.60,
                                   leaf({ "History", "Mastering", "Essential Sound" }, "History"),
                                   leaf({ "Analyser", "Delivery", "Script", "Master" }, "Analyser"))));
    }

    // Multitrack, as its mockup lays it out: the files over the track's
    // effects rack down the left, the timeline taking the middle with the
    // mixer and the instruments under it, and down the right the master over
    // the history.
    return beside(0.18,
                  above(0.60,
                        leaf({ "Files" }),
                        leaf({ "Track FX" })),
                  beside(0.78,
                         above(0.68,
                               leaf({ "Tracks" }),
                               leaf({ "Mixer", "Keys", "Session", "Keyboard" }, "Mixer")),
                         above(0.55,
                               leaf({ "Master", "Transport" }, "Master"),
                               leaf({ "History" }))));
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

} // namespace soundsplice::layouts
