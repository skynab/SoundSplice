#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Glyphs.h"
#include "Theme.h"

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace soundsplice
{
/** Where within a DockRegion a dragged panel was dropped, which decides what
    happens to it: Centre adds it to that region as another tab, while the
    four edges split the region in that direction and put it in the new half
    (see DockWorkspace::movePanel). This is the "drag a tab to the edge of a
    pane to split it" gesture every modern editor uses. */
enum class DropZone { Centre, Left, Right, Top, Bottom };

/**
    A single tab header: click to activate its panel, drag it onto a different
    DockRegion's header strip to move the panel there. Deliberately not a
    juce::Button subclass — plain paint()/mouse* like this app's other custom
    widgets (MixerStrip, LevelMeter) — so the drag gesture (telling a click
    apart from a drag-past-threshold) is fully under our control.
*/
class DockTabHeader final : public juce::Component
{
public:
    std::function<void()> onClick;
    std::function<void()> onDragStarted;
    std::function<void()> onCloseClicked;
    std::function<void()> onContextMenu;

    void setText(const juce::String& text) { text_ = text; repaint(); }
    void setActive(bool active) { active_ = active; repaint(); }

    /** How wide the tab wants to be: its glyph, its name and room for the
        close cross, as the mockups size tabs to their labels. */
    int idealWidth() const
    {
        return kPadding + kGlyphSize + kGap
             + (int) std::ceil(juce::GlyphArrangement::getStringWidth(font(), text_)) + kGap + kCloseWidth;
    }

    void paint(juce::Graphics& g) override
    {
        // A tab is its glyph and name on the header, with no box of its own:
        // the active one is in the text colour and underlined in the accent,
        // the rest are muted until hovered.
        const auto ink = active_ || hovered_ ? theme::colour(*this, theme::textId)
                                             : theme::colour(*this, theme::textMutedId);

        auto area = getLocalBounds().withTrimmedLeft(kPadding);
        area.removeFromRight(closeBounds().getWidth());
        glyphs::draw(g, glyphs::forPanel(text_), area.removeFromLeft(kGlyphSize).toFloat(), ink, (float) kGlyphSize);
        area.removeFromLeft(kGap);

        g.setColour(ink);
        g.setFont(font());
        g.drawText(text_, area, juce::Justification::centredLeft, true);

        if (active_)
        {
            g.setColour(theme::colour(*this, theme::accentId));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced((float) kPadding - 1.0f, 0.0f)
                                       .removeFromBottom(2.0f),
                                   1.0f);
        }

        // Only drawn on the active tab or under the mouse: a cross on every
        // tab all the time reads as clutter, and the gesture is discoverable
        // from either state.
        if (closeVisible())
        {
            const auto box = closeBounds().toFloat().withSizeKeepingCentre(16.0f, 16.0f);
            if (closeHovered_)
            {
                g.setColour(theme::colour(*this, theme::textId).withAlpha(0.08f));
                g.fillRoundedRectangle(box, 4.0f);
            }
            glyphs::draw(g, glyphs::Glyph::cross, box, ink.withMultipliedAlpha(closeHovered_ ? 1.0f : 0.7f), 10.0f);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override { updateHover(true, e.position); }
    void mouseEnter(const juce::MouseEvent& e) override { updateHover(true, e.position); }
    void mouseExit(const juce::MouseEvent&) override    { updateHover(false, {}); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragStarted_ = false;

        // Handled on mouseDown, not mouseUp: a context menu that waits for the
        // button to come up feels broken, and it's the platform convention.
        if (e.mods.isPopupMenu() && onContextMenu)
            onContextMenu();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragStarted_ && e.getDistanceFromDragStart() > 8)
        {
            dragStarted_ = true;
            if (onDragStarted)
                onDragStarted();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        const bool wasClick = ! dragStarted_ && e.getDistanceFromDragStart() < 4;
        dragStarted_ = false;

        if (! wasClick || e.mods.isPopupMenu())
            return; // the popup was already opened on mouseDown

        // Closing takes precedence over activating: a click on the cross of an
        // inactive tab should close it, not merely bring it forward.
        if (closeVisible() && closeBounds().contains(e.getPosition()) && onCloseClicked)
            onCloseClicked();
        else if (onClick)
            onClick();
    }

private:
    static constexpr int kCloseWidth    = 18;
    static constexpr int kMinCloseWidth = 46; // below this a tab is all cross and no label
    static constexpr int kPadding       = 8;
    static constexpr int kGlyphSize     = 13;
    static constexpr int kGap           = 6;

    static juce::Font font() { return juce::Font(juce::FontOptions(11.5f)); }

    /** The cross's hit area, empty when the tab is too narrow to show one —
        so a cramped region degrades to plain tabs rather than to a tab whose
        label has been eaten. Closing is still available from the View menu. */
    juce::Rectangle<int> closeBounds() const
    {
        if (getWidth() < kMinCloseWidth)
            return {};
        return getLocalBounds().removeFromRight(kCloseWidth);
    }

    /** The cross is only drawn on the active or hovered tab, so it must only
        be *clickable* then too — an invisible hit target that closes a pane
        is worse than no shortcut at all. */
    bool closeVisible() const { return (active_ || hovered_) && ! closeBounds().isEmpty(); }

    void updateHover(bool overTab, juce::Point<float> position)
    {
        const bool overClose = overTab && closeBounds().contains(position.toInt());
        if (overTab == hovered_ && overClose == closeHovered_)
            return;

        hovered_      = overTab;
        closeHovered_ = overClose;
        repaint();
    }

    juce::String text_;
    bool active_       = false;
    bool dragStarted_  = false;
    bool hovered_      = false;
    bool closeHovered_ = false;
};

/**
    One dockable region of the workspace: a strip of tab headers (one per
    hosted panel) plus the currently-active panel's content below it. Panels
    can be dragged from one DockRegion's header strip onto another's to move
    them there — the actual re-parenting is coordinated by whoever owns two or
    more DockRegions (see DockWorkspace::movePanel), since a region only
    knows about its own panels, not the tree it sits in.

    A DockRegion never owns the panel Components passed to addPanel() (they're
    expected to outlive it, e.g. as members of the owning MainComponent) — it
    only parents/unparents them via addAndMakeVisible/removeChildComponent,
    same as any JUCE re-parenting.
*/
class DockRegion final : public juce::Component,
                         public juce::DragAndDropTarget
{
public:
    static constexpr int   kHeaderHeight = 30;
    static constexpr float kRadius       = 8.0f;

    // Fired when a panel dragged FROM ELSEWHERE is dropped on this region.
    // `panelName` identifies which panel (matches the name it was added under
    // in whichever region currently hosts it) and `zone` says whether it was
    // dropped in the middle (make it another tab here) or against an edge
    // (split this region and put it in the new half). The owner does the
    // actual re-homing — see DockWorkspace::movePanel — since a region only
    // knows about its own panels, not the tree it sits in.
    std::function<void(const juce::String& panelName, DockRegion& target, DropZone zone)> onForeignPanelDropped;

    /** Fired when a tab's close cross is clicked. A region can't close a panel
        on its own — the workspace has to hand the space back and may collapse
        the region entirely — so it only reports the request. */
    std::function<void(const juce::String& panelName)> onPanelCloseRequested;

    /** Fired when a tab is right-clicked. Like the close request, the region
        only reports it: every entry the menu wants to offer — closing, and
        reopening a pane that lives nowhere right now — is the workspace's to
        know about. */
    std::function<void(const juce::String& panelName, DockRegion& region)> onPanelContextMenuRequested;

    void addPanel(const juce::String& name, juce::Component& content)
    {
        auto header = std::make_unique<DockTabHeader>();
        header->setText(name);
        header->onClick       = [this, name] { showPanel(name); };
        header->onContextMenu = [this, name]
        {
            if (onPanelContextMenuRequested)
                onPanelContextMenuRequested(name, *this);
        };
        header->onCloseClicked = [this, name]
        {
            if (onPanelCloseRequested)
                onPanelCloseRequested(name);
        };
        header->onDragStarted = [this, name]
        {
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this))
                dnd->startDragging(name, this);
        };
        addAndMakeVisible(*header);
        addAndMakeVisible(content);
        content.setVisible(false);

        panels_.push_back({ name, &content, std::move(header) });
        showPanel(name);
    }

    // Detaches `name` from this region: removes its header, unparents its
    // content, and drops the bookkeeping entry. Does not delete the content
    // component — callers re-home it elsewhere via addPanel(), or it's simply
    // left unparented (and invisible) if nowhere else wants it yet.
    void removePanel(const juce::String& name)
    {
        for (size_t i = 0; i < panels_.size(); ++i)
        {
            if (panels_[i].name != name)
                continue;

            removeChildComponent(panels_[i].header.get());
            removeChildComponent(panels_[i].content);
            panels_.erase(panels_.begin() + (long) i);
            break;
        }

        if (! panels_.empty())
        {
            activeIndex_ = juce::jlimit(0, (int) panels_.size() - 1, activeIndex_);
            showPanel(panels_[(size_t) activeIndex_].name);
        }
        else
        {
            activeIndex_ = 0;
            resized();
        }
    }

    bool hasPanel(const juce::String& name) const
    {
        for (auto& p : panels_)
            if (p.name == name)
                return true;
        return false;
    }

    int numPanels() const { return (int) panels_.size(); }

    /** Every panel name currently hosted here, in tab order — used when
        saving the layout, and when emptying a region (see
        DockWorkspace::resetToSingleRegion). */
    std::vector<juce::String> panelNames() const
    {
        std::vector<juce::String> names;
        for (const auto& p : panels_)
            names.push_back(p.name);
        return names;
    }

    /** The content Component for @p name, or nullptr if this region doesn't
        host it — lets a caller move a panel between regions without needing
        its own separate name->Component lookup table. */
    juce::Component* contentFor(const juce::String& name) const
    {
        for (const auto& p : panels_)
            if (p.name == name)
                return p.content;
        return nullptr;
    }

    /** The name of whichever panel is currently visible, or an empty string
        if this region hosts none. Used to persist/restore the workspace
        layout (see MainComponent::saveDockLayout). */
    juce::String activePanelName() const
    {
        return (activeIndex_ >= 0 && activeIndex_ < (int) panels_.size())
                   ? panels_[(size_t) activeIndex_].name
                   : juce::String();
    }

    void showPanel(const juce::String& name)
    {
        for (size_t i = 0; i < panels_.size(); ++i)
        {
            const bool active = (panels_[i].name == name);
            panels_[i].content->setVisible(active);
            panels_[i].header->setActive(active);
            if (active)
                activeIndex_ = (int) i;
        }
        resized();
    }

    void paint(juce::Graphics& g) override
    {
        // A pane floats on the workspace: the surface, its header ruled off
        // below. The rounded corners and the border are drawn over the
        // content (see paintOverChildren), which paints itself square.
        g.fillAll(theme::surface(*this, theme::paneId));

        // A region with no tabs left is the one place the workspace shows
        // nothing at all. Say where the panes went, rather than leaving what
        // reads as a rendering failure.
        if (panels_.empty())
        {
            const auto body = getLocalBounds().withTrimmedTop(kHeaderHeight).reduced(10).toFloat();
            const float dashes[] = { 4.0f, 3.0f };
            juce::Path outline;
            outline.addRoundedRectangle(body, 8.0f);
            juce::Path dashed;
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, outline, dashes, 2);
            g.setColour(theme::colour(*this, theme::textId).withAlpha(0.16f));
            g.fillPath(dashed);

            g.setColour(theme::colour(*this, theme::textMutedId));
            g.setFont(12.0f);
            g.drawFittedText("No panes open\nReopen one from the View menu",
                             body.toNearestInt().reduced(12), juce::Justification::centred, 2);
        }
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        const auto bounds  = getLocalBounds().toFloat();
        const auto outline = bounds.reduced(0.5f);

        // The header's rule, then the corners: whatever the content painted
        // outside the rounded outline goes back to the workspace colour.
        g.setColour(theme::colour(*this, theme::dividerId));
        g.fillRect(bounds.getX(), (float) kHeaderHeight - 1.0f, bounds.getWidth(), 1.0f);

        juce::Path corners;
        corners.addRectangle(bounds);
        corners.addRoundedRectangle(outline, kRadius);
        corners.setUsingNonZeroWinding(false);
        g.setColour(theme::surface(*this, theme::workspaceId));
        g.fillPath(corners);

        g.setColour(theme::colour(*this, theme::dividerId));
        g.drawRoundedRectangle(outline, kRadius, 1.0f);

        // While a panel is being dragged over us, shade exactly the area it
        // would end up occupying — the whole region for a Centre drop, or the
        // half it would split off for an edge drop. Showing the actual
        // resulting shape is what makes the edge gesture discoverable.
        if (dragActive_)
        {
            const auto zone = highlightBounds().toFloat().reduced(3.0f);
            g.setColour(theme::colour(*this, theme::accentId).withAlpha(0.14f));
            g.fillRoundedRectangle(zone, 6.0f);
            g.setColour(theme::colour(*this, theme::accentId));
            g.drawRoundedRectangle(zone, 6.0f, 1.5f);
        }
    }

    /** Which zone a point in this region's local coordinates falls in. A drop
        anywhere on the header strip always means Centre — dropping onto a tab
        bar unambiguously means "put it here as a tab", and treating that as a
        Top-edge split would make the most natural gesture do the surprising
        thing. */
    DropZone zoneAt(juce::Point<int> localPosition) const
    {
        if (localPosition.y < kHeaderHeight)
            return DropZone::Centre;

        auto body = getLocalBounds().withTrimmedTop(kHeaderHeight);
        if (body.getWidth() <= 0 || body.getHeight() <= 0)
            return DropZone::Centre;

        const float x = (float) (localPosition.x - body.getX()) / (float) body.getWidth();
        const float y = (float) (localPosition.y - body.getY()) / (float) body.getHeight();

        // Distance to each edge, as a fraction; the nearest one wins unless
        // the point is comfortably inside, which means Centre.
        const float toLeft = x, toRight = 1.0f - x, toTop = y, toBottom = 1.0f - y;
        const float nearest = juce::jmin(toLeft, toRight, toTop, toBottom);
        if (nearest > kEdgeFraction)
            return DropZone::Centre;

        // `nearest` is by construction one of the four, so the first of these
        // that holds identifies it — compared with <= rather than == to keep
        // clear of exact float equality.
        if (toLeft <= nearest)  return DropZone::Left;
        if (toRight <= nearest) return DropZone::Right;
        if (toTop <= nearest)   return DropZone::Top;
        return DropZone::Bottom;
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto headerRow = area.removeFromTop(kHeaderHeight).withTrimmedLeft(2).withTrimmedRight(3);
        headerRow.removeFromBottom(1); // the rule under the tabs

        // Each tab as wide as its name, all shrunk alike when they don't fit.
        int wanted = 0;
        for (auto& p : panels_)
            wanted += p.header->idealWidth();
        const double fit = wanted > headerRow.getWidth() && wanted > 0
                               ? (double) headerRow.getWidth() / (double) wanted
                               : 1.0;
        for (auto& p : panels_)
            p.header->setBounds(headerRow.removeFromLeft((int) std::floor(p.header->idealWidth() * fit)));

        // The content stays inside the border, so it can't paint over it.
        area = area.withTrimmedLeft(1).withTrimmedRight(1).withTrimmedBottom(1);
        for (auto& p : panels_)
            if (p.content->isVisible())
                p.content->setBounds(area);
    }

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        // File drags (from a FileBrowserPanel's FileTreeComponent) are for
        // ArrangementView, not for regrouping dock panels — reject them here
        // so they fall through to whichever ArrangementView is underneath.
        if (dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr)
            return false;
        if (! details.description.isString())
            return false;

        // A panel we already host is still a valid drag *if* we hold more
        // than one, since dropping it on one of our edges splits it out into
        // its own pane beside us. Only a region's sole panel has nowhere to
        // go, and rejecting that keeps it from being dragged out into a split
        // that would immediately collapse back.
        return ! hasPanel(details.description.toString()) || numPanels() > 1;
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        dragActive_ = true;
        dropZone_   = zoneAt(details.localPosition);
        repaint();
    }

    void itemDragMove(const SourceDetails& details) override
    {
        const auto zone = zoneAt(details.localPosition);
        if (zone != dropZone_)
        {
            dropZone_ = zone;
            repaint();
        }
    }

    void itemDragExit(const SourceDetails&) override { dragActive_ = false; repaint(); }

    void itemDropped(const SourceDetails& details) override
    {
        const auto zone = zoneAt(details.localPosition);
        dragActive_ = false;
        repaint();
        if (onForeignPanelDropped)
            onForeignPanelDropped(details.description.toString(), *this, zone);
    }

private:
    static constexpr float kEdgeFraction = 0.25f; // how deep the edge zones reach in

    /** The area a drop would land in, used for the drag highlight. */
    juce::Rectangle<int> highlightBounds() const
    {
        auto body = getLocalBounds().withTrimmedTop(kHeaderHeight);
        switch (dropZone_)
        {
            case DropZone::Left:   return body.withWidth(body.getWidth() / 2);
            case DropZone::Right:  return body.withTrimmedLeft(body.getWidth() / 2);
            case DropZone::Top:    return body.withHeight(body.getHeight() / 2);
            case DropZone::Bottom: return body.withTrimmedTop(body.getHeight() / 2);
            case DropZone::Centre: break;
        }
        return getLocalBounds();
    }

    struct Panel
    {
        juce::String                   name;
        juce::Component*               content;
        std::unique_ptr<DockTabHeader> header;
    };

    std::vector<Panel> panels_;
    int                 activeIndex_ = 0;
    bool                dragActive_  = false;
    DropZone            dropZone_    = DropZone::Centre;
};

} // namespace soundsplice
