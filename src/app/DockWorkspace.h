#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DockLayoutTree.h"
#include "DockRegion.h"

namespace looper
{
/** The draggable bar between the two halves of a split. Owns no state — it
    reports drags to the workspace, which turns them into a new split ratio
    (see DockWorkspace::dragDivider). Written by hand rather than reusing
    juce::StretchableLayoutResizerBar because that class is built around a
    flat, index-addressed StretchableLayoutManager, which a recursive tree
    of independent two-way splits doesn't map onto. */
class SplitDivider final : public juce::Component
{
public:
    std::function<void(const juce::MouseEvent&)> onDrag;

    explicit SplitDivider(bool horizontal) : horizontal_(horizontal)
    {
        setMouseCursor(horizontal_ ? juce::MouseCursor::LeftRightResizeCursor
                                   : juce::MouseCursor::UpDownResizeCursor);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff26262a));
        g.setColour(juce::Colours::white.withAlpha(hovered_ ? 0.30f : 0.10f));

        // A short grip mark in the middle, so the bar reads as draggable.
        auto centre = getLocalBounds().toFloat().getCentre();
        if (horizontal_)
            g.fillRect(centre.x - 0.5f, centre.y - 12.0f, 1.0f, 24.0f);
        else
            g.fillRect(centre.x - 12.0f, centre.y - 0.5f, 24.0f, 1.0f);
    }

    void mouseEnter(const juce::MouseEvent&) override { hovered_ = true;  repaint(); }
    void mouseExit(const juce::MouseEvent&) override  { hovered_ = false; repaint(); }
    void mouseDrag(const juce::MouseEvent& e) override { if (onDrag) onDrag(e); }

private:
    bool horizontal_;
    bool hovered_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SplitDivider)
};

/**
    The dockable workspace: a binary tree whose leaves are DockRegions (tab
    groups) and whose interior nodes are two-way splits, each with its own
    draggable divider and ratio.

    This replaces the previous fixed Left/Centre/Bottom/Right slots. Any
    region can be split any number of times in either direction, so the
    layout is whatever the user builds rather than a shape baked into the
    code — drag a panel's tab onto the middle of a region to add it there, or
    onto one of its edges to split that region and drop it into the new half
    (see DockRegion::zoneAt). Emptying a region collapses it and gives its
    space back to its sibling, which is what keeps the tree from filling up
    with dead panes — and, unlike the old auto-hiding Right slot, means there
    is never a region that exists but can't be dropped into.

    The workspace owns the panel *content* only by reference: callers
    register each panel's Component once (registerPanel) and it is re-parented
    between regions from then on, never copied or deleted.
*/
class DockWorkspace final : public juce::Component,
                            private juce::AsyncUpdater
{
public:
    /** Fired after any change the user made to the layout, so the owner can
        persist it (see saveLayout). */
    std::function<void()> onLayoutChanged;

    DockWorkspace()
    {
        root_ = makeLeaf();
    }

    ~DockWorkspace() override { cancelPendingUpdate(); }

    /** Tells the workspace which Component backs a panel name. Must be called
        for every panel before it can be placed or restored. */
    void registerPanel(const juce::String& name, juce::Component& content)
    {
        panels_[name] = &content;
    }

    /** The region a fresh workspace starts with — the one every default
        layout is built out from. */
    DockRegion& rootRegion()
    {
        jassert(root_ != nullptr && root_->region != nullptr);
        return *root_->region;
    }

    /** Adds @p name to @p region (which must already be in this workspace).
        Used to build the default layout; user-driven moves go through
        movePanel instead. */
    void addPanel(DockRegion& region, const juce::String& name)
    {
        auto* content = contentFor(name);
        if (content != nullptr)
            region.addPanel(name, *content);
    }

    /** Every panel name the workspace knows about, whether open or not — the
        View menu lists these so a closed pane can be found again. Sorted, since
        it comes from a map keyed by name. */
    std::vector<juce::String> registeredPanels() const
    {
        std::vector<juce::String> names;
        names.reserve(panels_.size());
        for (const auto& [name, content] : panels_)
            names.push_back(name);
        return names;
    }

    bool isPanelOpen(const juce::String& name) const { return findRegionWithPanel(name) != nullptr; }

    /** True if the panel is not just open but the *visible* tab of its region.
        Lets a menu tell "buried behind another tab" apart from "in front", so
        clicking it can reveal rather than close. */
    bool isPanelActive(const juce::String& name) const
    {
        auto* region = findRegionWithPanel(name);
        return region != nullptr && region->activePanelName() == name;
    }

    /** Closes a panel, handing its space back: the region collapses if that
        emptied it, exactly as when the last tab is dragged away. The panel
        itself isn't destroyed — it's a long-lived component the owner holds —
        so reopening it later restores the same object with its state intact. */
    void closePanel(const juce::String& name)
    {
        auto* region = findRegionWithPanel(name);
        if (region == nullptr)
            return;

        rememberNeighbours(name, region->panelNames());
        region->removePanel(name);
        collapseEmptyRegions();
        resized();
        notifyLayoutChanged();
    }

    /** Closes every tab of @p region except @p keep. The panels all remember
        the same group, so reopening any of them brings it back here. */
    void closeOtherPanels(DockRegion& region, const juce::String& keep)
    {
        const auto names = region.panelNames(); // snapshot: removing mutates it

        for (const auto& name : names)
        {
            if (name == keep)
                continue;

            // Every one of them remembers the group as it was *before* the
            // first removal, so reopening the third doesn't find only the
            // remnants left by closing the first two.
            rememberNeighbours(name, names);
            region.removePanel(name);
        }

        collapseEmptyRegions();
        resized();
        notifyLayoutChanged();
    }

    /** Reopens a closed panel, putting it back with the group that still holds
        most of the tabs it sat beside — see chooseReopenLeaf, which is where
        that decision lives and is tested. Does nothing if it's already open;
        the caller should activate it instead. */
    void openPanel(const juce::String& name)
    {
        if (isPanelOpen(name) || contentFor(name) == nullptr)
            return;

        const auto regions = allRegions();

        std::vector<DockLeafSummary> leaves;
        leaves.reserve(regions.size());
        for (auto* region : regions)
        {
            DockLeafSummary leaf;
            for (const auto& panel : region->panelNames())
                leaf.panels.push_back(panel.toStdString());
            leaf.area = (long long) region->getWidth() * (long long) region->getHeight();
            leaves.push_back(std::move(leaf));
        }

        std::vector<std::string> remembered;
        if (const auto it = lastNeighbours_.find(name); it != lastNeighbours_.end())
            for (const auto& neighbour : it->second)
                remembered.push_back(neighbour.toStdString());

        const int index = chooseReopenLeaf(leaves, remembered);
        if (index >= 0)
            openPanelIn(*regions[(size_t) index], name);
    }

    /** Reopens a closed panel as a tab of one specific region — what the tab
        context menu's "Open Here" does, where the user has named the place
        and no guess is wanted. */
    void openPanelIn(DockRegion& region, const juce::String& name)
    {
        auto* content = contentFor(name);
        if (content == nullptr || isPanelOpen(name))
            return;

        region.addPanel(name, *content);
        resized();
        notifyLayoutChanged();
    }

    /** Brings an already-open panel to the front of its region. */
    void revealPanel(const juce::String& name)
    {
        if (auto* region = findRegionWithPanel(name))
            region->showPanel(name);
    }

    /** Splits @p region in the given direction and returns the new, empty
        region on that side. Used both by the drag-to-edge gesture and when
        building a default layout in code.

        @p ratio is the share given to the *first* (left/top) half, whichever
        of the two that ends up being — so a Left split at 0.2 makes the new
        region a fifth of the width, while a Right split at 0.8 does the same
        thing from the other end. */
    DockRegion* splitRegion(DockRegion& region, DropZone zone, double ratio = 0.5)
    {
        if (zone == DropZone::Centre)
            return &region;

        DockNode* leaf = findLeaf(&region);
        if (leaf == nullptr)
            return nullptr;

        // The leaf becomes a split node: its region moves down into one new
        // child, and the other child is a fresh empty region.
        auto existing = std::make_unique<DockNode>();
        existing->region = std::move(leaf->region);

        auto fresh = makeLeaf();
        auto* freshRegion = fresh->region.get();

        const bool horizontal = (zone == DropZone::Left || zone == DropZone::Right);
        const bool freshFirst = (zone == DropZone::Left || zone == DropZone::Top);

        leaf->horizontal = horizontal;
        leaf->ratio      = juce::jlimit(0.05, 0.95, ratio);
        leaf->first      = freshFirst ? std::move(fresh) : std::move(existing);
        leaf->second     = freshFirst ? std::move(existing) : std::move(fresh);
        leaf->first->parent  = leaf;
        leaf->second->parent = leaf;
        leaf->divider    = makeDivider(leaf, horizontal);

        resized();
        return freshRegion;
    }

    /** Re-homes @p panelName into @p target, either as another tab there
        (Centre) or into a new region split off @p target's given edge. The
        region it came from is collapsed if that empties it. */
    void movePanel(const juce::String& panelName, DockRegion& target, DropZone zone)
    {
        auto* content = contentFor(panelName);
        if (content == nullptr)
            return;

        DockRegion* source = findRegionWithPanel(panelName);
        if (source == nullptr)
            return;

        // Dropping a region's only panel onto that same region would split it
        // and immediately collapse the empty half back — a no-op with a
        // flicker, so skip it.
        if (source == &target && (zone == DropZone::Centre || source->numPanels() <= 1))
            return;

        source->removePanel(panelName);

        DockRegion* destination = (zone == DropZone::Centre) ? &target : splitRegion(target, zone);
        if (destination == nullptr)
        {
            source->addPanel(panelName, *content); // split failed; put it back
            return;
        }

        destination->addPanel(panelName, *content);
        resized();

        // Collapsing the region this panel came from would DESTROY it — and
        // this runs inside that drag's own drop dispatch, with the drag
        // machinery still holding the source region as the drag's source
        // component. So the structural cleanup is deferred to the next
        // message-loop pass, once the drag has fully unwound.
        triggerAsyncUpdate();
    }

    /** Empties the whole workspace back to a single region, so a default
        layout can be rebuilt into it. */
    void resetToSingleRegion()
    {
        for (auto* region : allRegions())
            for (const auto& name : region->panelNames())
                region->removePanel(name);

        root_ = makeLeaf();
        resized();
    }

    // ---- persistence ----------------------------------------------------
    /** Serialises the layout via the JUCE-free DockLayoutTree grammar, which
        is where the text format lives (and is unit-tested). */
    juce::String saveLayout() const { return juce::String(writeDockLayout(*describe(root_.get()))); }

    /** Rebuilds the tree from saveLayout()'s output. Returns false (leaving
        the workspace untouched) if the text doesn't parse or names a panel
        this workspace doesn't know — a stale saved layout should fall back to
        the default, not produce a half-built workspace. */
    bool restoreLayout(const juce::String& text)
    {
        auto described = parseDockLayout(text.toStdString());
        if (described == nullptr || ! namesAreKnown(*described))
            return false;

        root_ = build(*described);
        resized();
        return true;
    }

    void resized() override
    {
        if (root_ != nullptr)
            layoutNode(*root_, getLocalBounds());
    }

private:
    /** One node: either a leaf holding a region, or a split holding two
        children plus the divider between them. Exactly one of `region` and
        `first`/`second` is ever set. */
    struct DockNode
    {
        std::unique_ptr<DockRegion>    region;          // leaf
        std::unique_ptr<DockNode>      first, second;   // split
        std::unique_ptr<SplitDivider>  divider;
        bool                           horizontal = true;
        double                         ratio      = 0.5;
        DockNode*                      parent     = nullptr;
        juce::Rectangle<int>           area;            // last laid-out bounds, for divider drags

        bool isLeaf() const noexcept { return region != nullptr; }
    };

    static constexpr int kDividerThickness = 8;
    static constexpr int kMinPaneSize      = 80; // keeps a divider drag from crushing a pane away

    /** Deferred structural cleanup after a move — see movePanel. */
    void handleAsyncUpdate() override
    {
        collapseEmptyRegions();
        resized();
        notifyLayoutChanged();
    }

    /** Collapses every empty region except the root, which is kept so the
        workspace always has somewhere to drop into. Scans repeatedly rather
        than tracking which region was emptied: collapsing one can only ever
        make the tree smaller, so this terminates, and it also tidies up any
        region emptied by some other path. */
    void collapseEmptyRegions()
    {
        for (bool changed = true; changed;)
        {
            changed = false;
            for (auto* region : allRegions())
            {
                if (region->numPanels() > 0)
                    continue;

                DockNode* leaf = findLeaf(region);
                if (leaf == nullptr || leaf->parent == nullptr)
                    continue; // the last remaining region stays

                collapseIfEmpty(leaf);
                changed = true;
                break; // allRegions() is stale now — rescan
            }
        }
    }

    std::unique_ptr<DockNode> makeLeaf()
    {
        auto node = std::make_unique<DockNode>();
        node->region = std::make_unique<DockRegion>();
        node->region->onPanelCloseRequested = [this](const juce::String& name) { closePanel(name); };
        node->region->onPanelContextMenuRequested = [this](const juce::String& name, DockRegion& region)
        {
            showPanelContextMenu(name, region);
        };
        node->region->onForeignPanelDropped = [this](const juce::String& name, DockRegion& target, DropZone zone)
        {
            movePanel(name, target, zone);
        };
        addAndMakeVisible(*node->region);
        return node;
    }

    std::unique_ptr<SplitDivider> makeDivider(DockNode* node, bool horizontal)
    {
        auto divider = std::make_unique<SplitDivider>(horizontal);
        divider->onDrag = [this, node](const juce::MouseEvent& e) { dragDivider(node, e); };
        addAndMakeVisible(*divider);
        return divider;
    }

    /** Turns a divider drag into a new ratio for its node. The mouse position
        is taken relative to the workspace so it stays correct however deeply
        nested the split is. */
    void dragDivider(DockNode* node, const juce::MouseEvent& e)
    {
        if (node == nullptr || node->isLeaf())
            return;

        const auto  position = e.getEventRelativeTo(this).getPosition();
        const auto& area     = node->area;
        const int   span     = (node->horizontal ? area.getWidth() : area.getHeight()) - kDividerThickness;
        if (span <= 0)
            return;

        const int offset = node->horizontal ? position.x - area.getX() : position.y - area.getY();
        const double minRatio = (double) kMinPaneSize / (double) span;

        node->ratio = juce::jlimit(juce::jmin(minRatio, 0.5), juce::jmax(1.0 - minRatio, 0.5),
                                   (double) offset / (double) span);
        resized();
        notifyLayoutChanged();
    }

    void layoutNode(DockNode& node, juce::Rectangle<int> area)
    {
        node.area = area;

        if (node.isLeaf())
        {
            node.region->setBounds(area);
            return;
        }

        auto remaining = area;
        const int span      = (node.horizontal ? area.getWidth() : area.getHeight()) - kDividerThickness;
        const int firstSize = juce::jmax(0, (int) std::lround((double) span * node.ratio));

        if (node.horizontal)
        {
            layoutNode(*node.first, remaining.removeFromLeft(firstSize));
            node.divider->setBounds(remaining.removeFromLeft(kDividerThickness));
        }
        else
        {
            layoutNode(*node.first, remaining.removeFromTop(firstSize));
            node.divider->setBounds(remaining.removeFromTop(kDividerThickness));
        }
        layoutNode(*node.second, remaining);
    }

    /** Replaces an emptied leaf's parent with that leaf's sibling, so the
        space is handed back rather than left as a dead pane. The root leaf is
        kept even when empty — the workspace always has somewhere to drop
        into. */
    void collapseIfEmpty(DockNode* leaf)
    {
        if (leaf == nullptr || ! leaf->isLeaf() || leaf->region->numPanels() > 0)
            return;

        DockNode* parent = leaf->parent;
        if (parent == nullptr)
            return; // the last remaining region

        // Take the sibling out from under the parent, then overwrite the
        // parent with the sibling's contents — the parent slot is owned by
        // the grandparent, so this is how a node is replaced in place.
        std::unique_ptr<DockNode> sibling = (parent->first.get() == leaf) ? std::move(parent->second)
                                                                          : std::move(parent->first);
        parent->first.reset();  // destroys the empty leaf and its region
        parent->second.reset();
        parent->divider.reset();

        parent->region     = std::move(sibling->region);
        parent->first      = std::move(sibling->first);
        parent->second     = std::move(sibling->second);
        parent->divider    = std::move(sibling->divider);
        parent->horizontal = sibling->horizontal;
        parent->ratio      = sibling->ratio;

        if (parent->first != nullptr)  parent->first->parent  = parent;
        if (parent->second != nullptr) parent->second->parent = parent;

        // The promoted divider's callback still captures the sibling node,
        // which is about to be destroyed — re-point it at its new owner.
        if (parent->divider != nullptr)
            parent->divider->onDrag = [this, parent](const juce::MouseEvent& e) { dragDivider(parent, e); };
    }

    juce::Component* contentFor(const juce::String& name) const
    {
        const auto it = panels_.find(name);
        return it != panels_.end() ? it->second : nullptr;
    }

    std::vector<DockRegion*> allRegions() const
    {
        std::vector<DockRegion*> regions;
        collectRegions(root_.get(), regions);
        return regions;
    }

    static void collectRegions(DockNode* node, std::vector<DockRegion*>& out)
    {
        if (node == nullptr)
            return;
        if (node->isLeaf())
        {
            out.push_back(node->region.get());
            return;
        }
        collectRegions(node->first.get(), out);
        collectRegions(node->second.get(), out);
    }

    DockNode* findLeaf(DockRegion* region) const { return findLeaf(root_.get(), region); }

    static DockNode* findLeaf(DockNode* node, DockRegion* region)
    {
        if (node == nullptr)
            return nullptr;
        if (node->isLeaf())
            return node->region.get() == region ? node : nullptr;
        if (auto* found = findLeaf(node->first.get(), region))
            return found;
        return findLeaf(node->second.get(), region);
    }

    DockRegion* findRegionWithPanel(const juce::String& name) const
    {
        for (auto* region : allRegions())
            if (region->hasPanel(name))
                return region;
        return nullptr;
    }

    void notifyLayoutChanged() const
    {
        if (onLayoutChanged)
            onLayoutChanged();
    }

    /** Records the tab group @p name is leaving, so reopening it can find its
        way back. @p group may include @p name itself; it's filtered out, since
        a panel is never its own neighbour. */
    void rememberNeighbours(const juce::String& name, const std::vector<juce::String>& group)
    {
        std::vector<juce::String> neighbours;
        for (const auto& panel : group)
            if (panel != name)
                neighbours.push_back(panel);

        lastNeighbours_[name] = std::move(neighbours);
    }

    /** The right-click menu on a tab. Built here rather than in DockRegion
        because every useful entry needs to know about panels the region
        doesn't host — the closed ones, and the fact that closing may collapse
        the region entirely. */
    void showPanelContextMenu(const juce::String& name, DockRegion& region)
    {
        std::vector<juce::String> closed;
        for (const auto& panel : registeredPanels())
            if (! isPanelOpen(panel))
                closed.push_back(panel);

        juce::PopupMenu reopen;
        for (size_t i = 0; i < closed.size(); ++i)
            reopen.addItem((int) i + 10, closed[i]);

        juce::PopupMenu menu;
        menu.addItem(1, "Close \"" + name + "\"");
        menu.addItem(2, "Close Other Tabs", region.numPanels() > 1);
        menu.addSeparator();
        menu.addSubMenu("Open Here", reopen, ! closed.empty());

        // Both the region and the workspace can be gone by the time the menu
        // closes — a region collapses the moment its last tab leaves — so the
        // callback holds neither by raw pointer.
        juce::Component::SafePointer<DockWorkspace> self(this);
        juce::Component::SafePointer<DockRegion>    target(&region);

        menu.showMenuAsync(juce::PopupMenu::Options(), [self, target, name, closed](int result)
        {
            if (self == nullptr || target == nullptr || result == 0)
                return;

            if (result == 1)
                self->closePanel(name);
            else if (result == 2)
                self->closeOtherPanels(*target, name);
            else if (const size_t index = (size_t) (result - 10); index < closed.size())
                self->openPanelIn(*target, closed[index]);
        });
    }

    // ---- persistence helpers -------------------------------------------
    /** Live tree -> plain description (see DockLayoutTree.h). */
    static std::unique_ptr<DockLayoutNode> describe(const DockNode* node)
    {
        auto described = std::make_unique<DockLayoutNode>();
        if (node == nullptr)
            return described; // an empty leaf

        if (node->isLeaf())
        {
            for (const auto& name : node->region->panelNames())
                described->panels.push_back(name.toStdString());
            described->activePanel = node->region->activePanelName().toStdString();
            return described;
        }

        described->horizontal = node->horizontal;
        described->ratio      = node->ratio;
        described->first      = describe(node->first.get());
        described->second     = describe(node->second.get());
        return described;
    }

    /** True if every panel the description names is registered here — checked
        before building anything, so a layout mentioning a panel this build
        doesn't have is rejected cleanly rather than part-way through. */
    bool namesAreKnown(const DockLayoutNode& described) const
    {
        if (described.isLeaf())
        {
            for (const auto& name : described.panels)
                if (contentFor(juce::String(name)) == nullptr)
                    return false;
            return true;
        }
        return namesAreKnown(*described.first) && namesAreKnown(*described.second);
    }

    /** Plain description -> live tree of regions and dividers. */
    std::unique_ptr<DockNode> build(const DockLayoutNode& described)
    {
        if (described.isLeaf())
        {
            auto leaf = makeLeaf();
            for (const auto& name : described.panels)
            {
                const juce::String panelName(name);
                if (auto* content = contentFor(panelName))
                    leaf->region->addPanel(panelName, *content);
            }
            const juce::String active(described.activePanel);
            if (active.isNotEmpty() && leaf->region->hasPanel(active))
                leaf->region->showPanel(active);
            return leaf;
        }

        auto node = std::make_unique<DockNode>();
        node->horizontal = described.horizontal;
        node->ratio      = described.ratio;
        node->first      = build(*described.first);
        node->second     = build(*described.second);
        node->first->parent  = node.get();
        node->second->parent = node.get();
        node->divider        = makeDivider(node.get(), node->horizontal);
        return node;
    }

    std::unique_ptr<DockNode>                 root_;
    std::map<juce::String, juce::Component*>  panels_;

    // Which tabs each closed panel sat beside, so reopening can put it back
    // rather than dropping it wherever there happens to be room. Deliberately
    // not persisted: it's a hint for undoing a close the user just made, and a
    // restored workspace has its saved arrangement instead.
    std::map<juce::String, std::vector<juce::String>> lastNeighbours_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DockWorkspace)
};

} // namespace looper
