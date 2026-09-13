#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace looper
{
/**
    Positions a control, or hides it when the space it was given is unusable.

    A pane is a dock tab, so its size is whatever the user's layout gives it —
    including sizes nobody designed for. Laying out from a fixed sequence of
    removeFromTop/removeFromLeft calls means the last few controls get whatever
    is left, and when that is nothing they end up with zero width or height:
    present, drawn as nothing, and hit-testing against nothing. That is
    indistinguishable from a broken control, which is the failure this codebase
    has produced most often.

    Hidden is the honest version of the same state. Neither can be used, but a
    hidden control doesn't claim to be there, and it comes back the moment the
    pane is given room.

    Only ever called on controls the caller intends to show: it sets visibility
    both ways, so passing one that was deliberately hidden would un-hide it.
*/
inline void setBoundsOrHide(juce::Component& control, juce::Rectangle<int> bounds,
                            int minWidth = 1, int minHeight = 1)
{
    const bool usable = bounds.getWidth() >= minWidth && bounds.getHeight() >= minHeight;

    control.setVisible(usable);
    if (usable)
        control.setBounds(bounds);
}

/**
    Lays out a label and the control it names side by side, dropping the label
    when the row is too narrow to hold both.

    The label is the part worth losing. A row of "Send [slider]" squeezed to
    30 pixels gave the whole width to the label and left the slider zero wide
    — the caption survived and the control it captions did not, which is
    exactly backwards. @p minControlWidth is how much the control needs before
    the label is worth any of the row.
*/
inline void layoutLabelledRow(juce::Rectangle<int> row, juce::Label& label,
                              juce::Component& control, int labelWidth,
                              int minControlWidth = 24)
{
    const bool roomForLabel = row.getWidth() >= labelWidth + minControlWidth;

    label.setVisible(roomForLabel);
    if (roomForLabel)
        label.setBounds(row.removeFromLeft(labelWidth));

    setBoundsOrHide(control, row);
}

} // namespace looper
