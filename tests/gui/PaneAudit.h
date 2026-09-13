#pragma once

#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace paneaudit
{
/**
    A shared audit for the things that have actually gone wrong in this app's
    panes, applied to whichever pane a test hands over.

    Every UI bug found this session was one of a small number of shapes, none
    of which any headless test could see and none of which fails a build:

      - a control declared, laid out and shown, but never parented, so it
        draws nothing (the drive pedal);
      - a control with zero width or height after layout, present and
        hit-testing against nothing (the chord buttons in a narrow pane);
      - a control whose drawn position and clickable position disagree (the
        mute/gear pair);
      - a control positioned outside its own parent, so it is clipped away.

    Checking them generically means a new pane inherits the coverage rather
    than needing someone to remember each case.
*/
struct Finding
{
    std::string what;
    std::string detail;
};

/** Every interactive descendant: the kinds a user actually clicks. Labels are
    skipped — several are deliberately non-interactive and unparented text. */
inline void collectControls(juce::Component& root, std::vector<juce::Component*>& out)
{
    for (int i = 0; i < root.getNumChildComponents(); ++i)
    {
        auto* child = root.getChildComponent(i);
        if (child == nullptr)
            continue;

        if (dynamic_cast<juce::Slider*>(child) != nullptr
            || dynamic_cast<juce::Button*>(child) != nullptr
            || dynamic_cast<juce::ComboBox*>(child) != nullptr)
        {
            out.push_back(child);
        }

        collectControls(*child, out);
    }
}

/** True when a control would actually be drawn: itself visible, and every
    ancestor up to and including the pane visible too.

    Component::isVisible() is only the component's own flag — the children of
    a hidden parent still report true individually. Using it directly makes
    every control inside a hidden section look like a broken one, which is a
    finding about the audit rather than about the pane. isShowing() isn't
    usable either: it requires a real window, which a headless test has no. */
inline bool effectivelyVisible(juce::Component& pane, juce::Component* control)
{
    for (auto* c = control; c != nullptr; c = c->getParentComponent())
    {
        if (! c->isVisible())
            return false;
        if (c == &pane)
            return true;
    }
    return false; // not under this pane at all
}

/** Reports every drawn control that can't be used: no size, or positioned
    outside the parent that clips it. A hidden control is not a finding —
    panes deliberately hide the controls that don't apply. */
inline std::vector<Finding> audit(juce::Component& pane)
{
    std::vector<Finding> findings;

    std::vector<juce::Component*> controls;
    collectControls(pane, controls);

    for (auto* control : controls)
    {
        if (! effectivelyVisible(pane, control))
            continue;

        const auto name   = control->getName().isEmpty() ? juce::String("(unnamed)")
                                                         : control->getName();
        const auto bounds = control->getBounds();

        if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0)
        {
            findings.push_back({ "zero-sized control",
                                 (name + " " + bounds.toString()).toStdString() });
            continue;
        }

        if (auto* parent = control->getParentComponent())
        {
            if (! parent->getLocalBounds().intersects(bounds))
            {
                findings.push_back({ "control outside its parent",
                                     (name + " " + bounds.toString() + " in "
                                      + parent->getLocalBounds().toString()).toStdString() });
            }
        }
    }

    return findings;
}

/** Fails with every finding named, rather than on the first. */
inline void requireUsable(juce::Component& pane, const juce::String& paneName)
{
    const auto findings = audit(pane);

    for (const auto& finding : findings)
        UNSCOPED_INFO(paneName << ": " << finding.what << " - " << finding.detail);

    INFO(paneName << " had " << findings.size() << " unusable control(s)");
    REQUIRE(findings.empty());
}

/**
    Reports every interactive control that nothing is listening to.

    The other half of the layout audit. A control can be parented, sized and
    hit-testable and still do nothing at all, because whoever added it never
    assigned its callback — which is how the drive pedal's controls existed for
    a whole commit while being unreachable.

    A deliberately empty handler counts as wired. Several controls here are read
    on demand rather than reacted to — the strum spread is read when a chord is
    stamped, not when it moves — and those are given an empty lambda on
    purpose. The shape being caught is "nobody ever assigned anything", not
    "the handler does little".

    Buttons are optional, because a button whose state is only read is a normal
    thing while a slider nobody reads is not.
*/
inline std::vector<Finding> auditWiring(juce::Component& pane, bool includeButtons)
{
    std::vector<Finding> findings;

    std::vector<juce::Component*> controls;
    collectControls(pane, controls);

    for (auto* control : controls)
    {
        const auto name = control->getName().isEmpty() ? juce::String("(unnamed)")
                                                       : control->getName();

        if (auto* slider = dynamic_cast<juce::Slider*>(control))
        {
            if (slider->onValueChange == nullptr && slider->onDragEnd == nullptr)
                findings.push_back({ "slider with no handler", name.toStdString() });
        }
        else if (auto* box = dynamic_cast<juce::ComboBox*>(control))
        {
            if (box->onChange == nullptr)
                findings.push_back({ "combo box with no handler", name.toStdString() });
        }
        else if (includeButtons)
        {
            if (auto* button = dynamic_cast<juce::Button*>(control))
                if (button->onClick == nullptr && button->onStateChange == nullptr)
                    findings.push_back({ "button with no handler", name.toStdString() });
        }
    }

    return findings;
}

inline void requireWired(juce::Component& pane, const juce::String& paneName,
                         bool includeButtons = true)
{
    const auto findings = auditWiring(pane, includeButtons);

    for (const auto& finding : findings)
        UNSCOPED_INFO(paneName << ": " << finding.what << " - " << finding.detail);

    INFO(paneName << " had " << findings.size() << " unwired control(s)");
    REQUIRE(findings.empty());
}

/** Every control the pane manages must be a child of it. A component that was
    never parented lays out, shows and hides perfectly while drawing nothing —
    which is how a whole pedal shipped invisible. */
inline void requireAllParented(juce::Component& pane, juce::Component* const* controls,
                               size_t count, const juce::String& paneName)
{
    for (size_t i = 0; i < count; ++i)
    {
        INFO(paneName << " control " << i << " is not a descendant of the pane");
        REQUIRE(pane.isParentOf(controls[i]));
    }
}

} // namespace paneaudit
