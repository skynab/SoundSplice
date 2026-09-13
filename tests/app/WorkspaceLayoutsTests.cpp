#include <catch2/catch_test_macros.hpp>

#include <app/WorkspaceLayouts.h>

#include <algorithm>
#include <set>
#include <string>

using namespace looper;

namespace
{
    constexpr layouts::Workspace kAll[] = { layouts::Workspace::MusicCreation,
                                            layouts::Workspace::AudioEditing };

    /** Every panel MainComponent registers. Restated here because the
        registration list lives in a JUCE-linked translation unit that these
        headless tests can't include — and the point of the test is exactly
        that the two agree. */
    const std::set<std::string> kRegisteredPanels {
        "Files", "Transport", "Tracks", "Keys", "Synth", "Drums", "Guitar",
        "Audio", "Mastering", "Analyser", "Session", "Track FX", "Mixer", "Master", "Keyboard"
    };
}

TEST_CASE("kNumWorkspaces covers every workspace", "[app][layouts]")
{
    // The menu builds itself by counting to kNumWorkspaces and casting, so a
    // layout added without bumping it is simply absent from the menu.
    REQUIRE((size_t) layouts::kNumWorkspaces == std::size(kAll));
}

TEST_CASE("Workspace names are distinct and non-empty", "[app][layouts]")
{
    std::set<std::string> names;
    for (auto workspace : kAll)
    {
        const std::string name = layouts::workspaceName(workspace);
        REQUIRE_FALSE(name.empty());
        names.insert(name);
    }
    REQUIRE(names.size() == std::size(kAll));
}

TEST_CASE("Every built-in layout round-trips through the grammar", "[app][layouts]")
{
    // The failure this prevents: a layout that doesn't parse is rejected by
    // DockWorkspace wholesale, so it would ship as a menu item that silently
    // does nothing and falls back to whatever was there.
    for (auto workspace : kAll)
    {
        const auto text = layouts::workspaceLayoutText(workspace);
        INFO(layouts::workspaceName(workspace) << ": " << text);

        REQUIRE_FALSE(text.empty());

        const auto parsed = parseDockLayout(text);
        REQUIRE(parsed != nullptr);

        // Writing what was parsed must give back the same text, or the tree
        // and the grammar disagree about something.
        REQUIRE(writeDockLayout(*parsed) == text);
    }
}

TEST_CASE("Every layout names only registered panels", "[app][layouts]")
{
    // A layout naming a panel that doesn't exist is rejected by
    // DockWorkspace::namesAreKnown before anything is applied — so a typo
    // here would be a menu item that quietly fails. Caught at build time
    // instead.
    for (auto workspace : kAll)
    {
        for (const auto& panel : layouts::panelsInWorkspace(workspace))
        {
            INFO(layouts::workspaceName(workspace) << " names '" << panel << "'");
            REQUIRE(kRegisteredPanels.count(panel) == 1);
        }
    }
}

TEST_CASE("No layout shows the same panel twice", "[app][layouts]")
{
    // A panel can only live in one region — DockWorkspace moves it rather
    // than duplicating it, so a layout listing one twice would quietly lose
    // the first placement.
    for (auto workspace : kAll)
    {
        const auto        panels = layouts::panelsInWorkspace(workspace);
        std::set<std::string> unique(panels.begin(), panels.end());

        INFO(layouts::workspaceName(workspace));
        REQUIRE(unique.size() == panels.size());
    }
}

TEST_CASE("Each layout opens the panes its job needs", "[app][layouts]")
{
    // The whole reason for having two: composing wants the instruments,
    // editing wants the waveform and the rack. A layout that opened
    // everything would be the crowded workspace this replaces.
    const auto music = layouts::panelsInWorkspace(layouts::Workspace::MusicCreation);
    const auto audio = layouts::panelsInWorkspace(layouts::Workspace::AudioEditing);

    const auto has = [](const std::vector<std::string>& panels, const char* name)
    {
        return std::find(panels.begin(), panels.end(), name) != panels.end();
    };

    for (const char* instrument : { "Synth", "Drums", "Guitar", "Keys" })
    {
        INFO(instrument);
        REQUIRE(has(music, instrument));
        REQUIRE_FALSE(has(audio, instrument));
    }

    REQUIRE(has(audio, "Audio"));
    REQUIRE(has(audio, "Mastering"));
    REQUIRE(has(audio, "Analyser"));

    // Both keep the timeline and the transport: you need to pick a clip and
    // to start playback whichever job you're doing.
    for (const char* shared : { "Tracks", "Transport", "Files" })
    {
        INFO(shared);
        REQUIRE(has(music, shared));
        REQUIRE(has(audio, shared));
    }
}

TEST_CASE("A layout's active tab is one of its own tabs", "[app][layouts]")
{
    // An activePanel naming something not in the group means "the first one"
    // — harmless, but always a mistake rather than an intention.
    const auto check = [](const DockLayoutNode& node, const auto& recurse) -> void
    {
        if (node.isLeaf())
        {
            if (! node.activePanel.empty())
            {
                INFO("active '" << node.activePanel << "'");
                REQUIRE(std::find(node.panels.begin(), node.panels.end(), node.activePanel)
                        != node.panels.end());
            }
            return;
        }
        if (node.first != nullptr)  recurse(*node.first, recurse);
        if (node.second != nullptr) recurse(*node.second, recurse);
    };

    for (auto workspace : kAll)
    {
        INFO(layouts::workspaceName(workspace));
        const auto tree = layouts::buildWorkspaceLayout(workspace);
        check(*tree, check);
    }
}
