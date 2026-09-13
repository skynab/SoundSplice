#include <catch2/catch_test_macros.hpp>

#include <app/DockLayoutTree.h>

using namespace looper;

namespace
{
    std::unique_ptr<DockLayoutNode> makeLeaf(std::vector<std::string> panels, std::string active = {})
    {
        auto leaf = std::make_unique<DockLayoutNode>();
        leaf->panels      = std::move(panels);
        leaf->activePanel = std::move(active);
        return leaf;
    }

    std::unique_ptr<DockLayoutNode> makeSplit(bool horizontal, double ratio,
                                              std::unique_ptr<DockLayoutNode> first,
                                              std::unique_ptr<DockLayoutNode> second)
    {
        auto node = std::make_unique<DockLayoutNode>();
        node->horizontal = horizontal;
        node->ratio      = ratio;
        node->first      = std::move(first);
        node->second     = std::move(second);
        return node;
    }

    /** Structural comparison — the round-trip only has to preserve meaning,
        and ratios go through a %.4f text form. */
    bool same(const DockLayoutNode& a, const DockLayoutNode& b)
    {
        if (a.isLeaf() != b.isLeaf())
            return false;

        if (a.isLeaf())
            return a.panels == b.panels && a.activePanel == b.activePanel;

        return a.horizontal == b.horizontal
            && std::abs(a.ratio - b.ratio) < 1.0e-4
            && same(*a.first, *b.first)
            && same(*a.second, *b.second);
    }
}

TEST_CASE("A single tab group round-trips", "[app][dock]")
{
    const auto original = makeLeaf({ "Tracks", "Keys", "Mixer" }, "Keys");

    const auto restored = parseDockLayout(writeDockLayout(*original));
    REQUIRE(restored != nullptr);
    REQUIRE(same(*restored, *original));
}

TEST_CASE("A nested split layout round-trips", "[app][dock]")
{
    // Files | ( (Tracks over Keys/Synth) | Mixer ) — the shape of the
    // app's default workspace.
    auto original = makeSplit(true, 0.18,
                              makeLeaf({ "Files" }, "Files"),
                              makeSplit(true, 0.72,
                                        makeSplit(false, 0.45,
                                                  makeLeaf({ "Tracks" }, "Tracks"),
                                                  makeLeaf({ "Keys", "Synth" }, "Synth")),
                                        makeLeaf({ "Mixer" }, "Mixer")));

    const auto restored = parseDockLayout(writeDockLayout(*original));
    REQUIRE(restored != nullptr);
    REQUIRE(same(*restored, *original));
}

TEST_CASE("An empty tab group round-trips", "[app][dock]")
{
    // A workspace emptied down to one bare region — still a valid layout,
    // and the one a user sees after dragging every panel out of it.
    const auto original = makeLeaf({});

    const auto text = writeDockLayout(*original);
    REQUIRE(text == "[|]");

    const auto restored = parseDockLayout(text);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->isLeaf());
    REQUIRE(restored->panels.empty());
}

TEST_CASE("Split orientation and ratio survive the round trip", "[app][dock]")
{
    const auto stacked = makeSplit(false, 0.25, makeLeaf({ "A" }), makeLeaf({ "B" }));

    const auto text = writeDockLayout(*stacked);
    REQUIRE(text.front() == 'V'); // stacked, not side by side

    const auto restored = parseDockLayout(text);
    REQUIRE(restored != nullptr);
    REQUIRE_FALSE(restored->horizontal);
    REQUIRE(std::abs(restored->ratio - 0.25) < 1.0e-4);
}

TEST_CASE("Out-of-range ratios are clamped on parse", "[app][dock]")
{
    // Hand-written/corrupted input: a ratio of 0 would give one half no space
    // at all and no way to drag it back.
    const auto restored = parseDockLayout("H0.0000([|A],[|B])");
    REQUIRE(restored != nullptr);
    REQUIRE(restored->ratio >= 0.05);
}

TEST_CASE("parseDockLayout rejects malformed input", "[app][dock]")
{
    REQUIRE(parseDockLayout("") == nullptr);
    REQUIRE(parseDockLayout("nonsense") == nullptr);
    REQUIRE(parseDockLayout("H0.5([|A]") == nullptr);          // unbalanced
    REQUIRE(parseDockLayout("H0.5([|A][|B])") == nullptr);     // missing comma
    REQUIRE(parseDockLayout("X0.5([|A],[|B])") == nullptr);    // unknown tag
    REQUIRE(parseDockLayout("[|A]trailing") == nullptr);       // trailing junk
    REQUIRE(parseDockLayout("[Aterminated") == nullptr);       // unterminated leaf
}

TEST_CASE("A reopened panel goes back to the group that kept its neighbours", "[app][dock]")
{
    // Synth was closed while sitting beside Keys. Reopening it must not land
    // it in the big arrangement pane just because that one has more room:
    // close-then-reopen should be as close to a no-op as the layout allows.
    const std::vector<DockLeafSummary> leaves {
        { { "Tracks" },        400000 }, // much the largest
        { { "Keys" },           60000 },
        { { "Files" },          40000 },
    };

    REQUIRE(chooseReopenLeaf(leaves, { "Keys" }) == 1);
}

TEST_CASE("The best-matching group wins, not merely a matching one", "[app][dock]")
{
    // A panel that was grouped with three others belongs with whichever group
    // inherited most of them, since that's the region its old one became.
    const std::vector<DockLeafSummary> leaves {
        { { "Mixer", "Session" }, 10000 },
        { { "Keys", "Drums", "Guitar" }, 10000 },
    };

    REQUIRE(chooseReopenLeaf(leaves, { "Keys", "Drums", "Guitar", "Mixer" }) == 1);
}

TEST_CASE("With no neighbours left open, the largest group takes it", "[app][dock]")
{
    // The fallback is the plain "somewhere sensible" rule — and it has to be,
    // because a panel opened for the very first time has no history at all.
    const std::vector<DockLeafSummary> leaves {
        { { "Files" },  1000 },
        { { "Tracks" }, 9000 },
        { { "Mixer" },  5000 },
    };

    REQUIRE(chooseReopenLeaf(leaves, {}) == 1);
    REQUIRE(chooseReopenLeaf(leaves, { "Keyboard", "Synth" }) == 1); // none of them open
}

TEST_CASE("An emptied workspace still offers somewhere to reopen into", "[app][dock]")
{
    // Closing every pane leaves one empty region. Reopening has to find it,
    // or the View menu would appear to do nothing and the workspace would be
    // stuck blank.
    const std::vector<DockLeafSummary> leaves { { {}, 500000 } };
    REQUIRE(chooseReopenLeaf(leaves, { "Keys" }) == 0);
}

TEST_CASE("Choosing a home reports failure rather than guessing", "[app][dock]")
{
    REQUIRE(chooseReopenLeaf({}, { "Keys" }) == -1);
}

TEST_CASE("A remembered neighbour is matched exactly, not by prefix", "[app][dock]")
{
    // "Track FX" and "Tracks" are both real panel names here; a sloppy
    // substring match would send one to the other's region.
    const std::vector<DockLeafSummary> leaves {
        { { "Tracks" },   90000 },
        { { "Track FX" }, 10000 },
    };

    REQUIRE(chooseReopenLeaf(leaves, { "Track FX" }) == 1);
}
