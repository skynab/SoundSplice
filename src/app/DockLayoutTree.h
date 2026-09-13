#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace looper
{
/**
    A pure description of a docking layout: which panels are grouped together,
    how those groups are split, and in what proportion — with no reference to
    the live Components that implement it (see DockWorkspace, which converts
    between this and its real tree of DockRegions).

    Deliberately JUCE-free so the text round-trip can be unit-tested headless,
    exactly like model::Serialization and PianoRollGeometry. Getting this
    grammar wrong silently resets everyone's saved workspace, so it's worth
    testing on its own rather than only through the UI.
*/
struct DockLayoutNode
{
    // Leaf: a tab group. `panels` in tab order; `activePanel` is whichever is
    // showing (empty, or not present in `panels`, simply means the first).
    std::vector<std::string> panels;
    std::string              activePanel;

    // Split: two children, `horizontal` = side by side (false = stacked), and
    // `ratio` = the share given to `first`.
    std::unique_ptr<DockLayoutNode> first, second;
    bool                            horizontal = true;
    double                          ratio      = 0.5;

    bool isLeaf() const noexcept { return first == nullptr && second == nullptr; }
};

namespace detail
{
    /** Ratios are always 0..1, so four decimals is ample and keeps the saved
        string short and readable. */
    inline std::string ratioText(double value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.4f", value);
        return buffer;
    }

    inline constexpr double kMinRatio = 0.05;
    inline constexpr double kMaxRatio = 0.95;

    inline double clampRatio(double value)
    {
        return value < kMinRatio ? kMinRatio : (value > kMaxRatio ? kMaxRatio : value);
    }
}

/**
    Serialises a layout to one line.

        node := 'H' ratio '(' node ',' node ')'   -- side by side
              | 'V' ratio '(' node ',' node ')'   -- stacked
              | '[' active '|' name ';' name ... ']'

    Panel names never contain this punctuation (they're short fixed
    identifiers like "Mixer"), so nothing needs escaping.
*/
inline std::string writeDockLayout(const DockLayoutNode& node)
{
    if (node.isLeaf())
    {
        std::string joined;
        for (size_t i = 0; i < node.panels.size(); ++i)
        {
            if (i > 0)
                joined += ';';
            joined += node.panels[i];
        }
        return "[" + node.activePanel + "|" + joined + "]";
    }

    return std::string(node.horizontal ? "H" : "V") + detail::ratioText(node.ratio)
         + "(" + writeDockLayout(*node.first) + "," + writeDockLayout(*node.second) + ")";
}

namespace detail
{
    /** Recursive-descent counterpart to writeDockLayout. Returns nullptr on
        anything malformed, leaving @p index wherever it gave up. */
    inline std::unique_ptr<DockLayoutNode> readDockNode(const std::string& text, size_t& index)
    {
        if (index >= text.size())
            return nullptr;

        const char tag = text[index];

        if (tag == '[')
        {
            const size_t bar = text.find('|', index);
            const size_t end = text.find(']', index);
            if (bar == std::string::npos || end == std::string::npos || bar > end)
                return nullptr;

            auto leaf = std::make_unique<DockLayoutNode>();
            leaf->activePanel = text.substr(index + 1, bar - index - 1);

            const std::string joined = text.substr(bar + 1, end - bar - 1);
            for (size_t start = 0; start < joined.size();)
            {
                const size_t separator = joined.find(';', start);
                const size_t stop      = (separator == std::string::npos) ? joined.size() : separator;
                if (stop > start)
                    leaf->panels.push_back(joined.substr(start, stop - start));
                start = stop + 1;
            }

            index = end + 1;
            return leaf;
        }

        if (tag != 'H' && tag != 'V')
            return nullptr;

        const size_t open = text.find('(', index);
        if (open == std::string::npos)
            return nullptr;

        auto node = std::make_unique<DockLayoutNode>();
        node->horizontal = (tag == 'H');
        node->ratio      = clampRatio(std::strtod(text.substr(index + 1, open - index - 1).c_str(), nullptr));
        index = open + 1;

        node->first = readDockNode(text, index);
        if (node->first == nullptr || index >= text.size() || text[index] != ',')
            return nullptr;
        ++index;

        node->second = readDockNode(text, index);
        if (node->second == nullptr || index >= text.size() || text[index] != ')')
            return nullptr;
        ++index;

        return node;
    }
}

/** Parses writeDockLayout()'s output. Returns nullptr if the text is
    malformed or has anything trailing — a stale or corrupt saved layout
    should fall back to the default rather than half-build a workspace. */
inline std::unique_ptr<DockLayoutNode> parseDockLayout(const std::string& text)
{
    size_t index = 0;
    auto   node  = detail::readDockNode(text, index);
    return (node != nullptr && index == text.size()) ? std::move(node) : nullptr;
}

/** One tab group, as much of it as choosing a home for a panel depends on. */
struct DockLeafSummary
{
    std::vector<std::string> panels;
    long long                area = 0; // on-screen pixels; ties are broken by this
};

/**
    Which tab group a reopened panel should join.

    Closing a pane and reopening it should land it back where it was, or the
    close/reopen pair silently rearranges the workspace — which is worse than
    not being able to close it at all. The layout has no slots to remember,
    though: regions are created and collapsed as the tree changes, so the
    region a panel was closed from may no longer exist.

    What survives is the *company it kept*. A panel is put back with whichever
    group still holds the most of its former tab-mates, since that group is
    the same region if it survived, and the region that inherited its
    neighbours if it didn't.

    With no former neighbours recorded, or none of them still open, every
    group scores zero and the largest wins — the plain "put it somewhere
    sensible" fallback, which is all a first-ever open can do.

    Returns an index into @p leaves, or -1 if there are none.
*/
inline int chooseReopenLeaf(const std::vector<DockLeafSummary>& leaves,
                            const std::vector<std::string>&     formerNeighbours)
{
    int       best      = -1;
    int       bestScore = -1;
    long long bestArea  = -1;

    for (size_t i = 0; i < leaves.size(); ++i)
    {
        int score = 0;
        for (const auto& neighbour : formerNeighbours)
            for (const auto& panel : leaves[i].panels)
                if (panel == neighbour)
                    ++score;

        if (score > bestScore || (score == bestScore && leaves[i].area > bestArea))
        {
            best      = (int) i;
            bestScore = score;
            bestArea  = leaves[i].area;
        }
    }

    return best;
}

} // namespace looper
