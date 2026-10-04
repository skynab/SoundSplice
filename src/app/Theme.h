#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundsplice
{
/**
    Themes: the colours of the app's own surfaces and JUCE's widgets, and an
    accent colour, chosen in Preferences > Display.

    All of them are dark. The panes paint their text and waveforms in light
    colours of their own, so a light theme would need every one of them
    reworked; what changes here is what they're painted on, the widgets, and
    the accent. Nocturne, the first, is the design system's and the default;
    a saved name that no longer exists (the old Dark) falls back to it. High
    Contrast is the accessibility theme: black, white, yellow, and a ring
    round whatever has keyboard focus.

    The panes ask for their surfaces by role (theme::surface), so a theme
    reaches them without each one knowing about themes.
*/
namespace theme
{
    /** The app's own colour roles, as LookAndFeel colour ids. */
    enum ColourIds
    {
        paneId        = 0x5e510001, // a pane's background
        insetId       = 0x5e510002, // a plot or list sunk into a pane
        workspaceId   = 0x5e510003, // between the panes
        tabActiveId   = 0x5e510004,
        tabInactiveId = 0x5e510005,
        popupId       = 0x5e510006, // the palette, the status banner
        focusRingId   = 0x5e510007,

        // What the panes draw, rather than what they're drawn on.
        accentId      = 0x5e510008, // a selection, a drop target: the chosen accent
        signalId      = 0x5e510009, // the audio itself: waveforms, spectra, curves, meter fills
        signalInkId   = 0x5e51000a, // the playhead, and marks drawn over the signal
        warnId        = 0x5e51000b, // solo, gain reduction, a meter running hot
        dangerId      = 0x5e51000c, // clipping, mute, record-arm: fills and marks
        dangerTextId  = 0x5e51000d, // the same, as text: errors, failures, overs
        clipTopId     = 0x5e51000e, // a clip's body on the timeline, shaded down
        clipBottomId  = 0x5e51000f, // to this
    };

    struct Theme
    {
        const char*                            name;
        juce::LookAndFeel_V4::ColourScheme     scheme;
        juce::Colour                           pane, inset, workspace, tabActive, tabInactive, popup;
        juce::Colour                           accent;     // its own, when the user hasn't picked one
        bool                                   focusRings; // always, rather than when asked for

        // The design system's signal colours, which every theme but High
        // Contrast shares: the audio is teal against the blurple accent, apart
        // in lightness as well as hue so the two stay distinct to colour-blind
        // eyes. Danger is a step darker for fills than for text, which needs
        // 4.5:1 on the panes.
        juce::Colour signal     { 0xff71cfd9 }; // color-signal
        juce::Colour signalInk  { 0xffc3eef3 }; // color-signal-ink
        juce::Colour warn       { 0xffe4b750 }; // color-warn
        juce::Colour danger     { 0xffe24947 }; // color-danger
        juce::Colour dangerText { 0xffff645f }; // color-danger-bright
        juce::Colour clipTop    { 0xff033d44 }; // color-clip-top
        juce::Colour clipBottom { 0xff00282f }; // color-clip-bottom
    };

    inline const std::vector<Theme>& all()
    {
        using Scheme = juce::LookAndFeel_V4::ColourScheme;
        static const std::vector<Theme> themes {
            // The design system's own (SoundSplice, after Cutline's Nocturne):
            // panes on color-bg, plots in color-inset, color-frame between the
            // panes, color-surface for the active tab and popups, and the
            // blurple accent. The scheme's outline is color-neutral-800 and its
            // highlight color-accent-800.
            { "Nocturne",
              Scheme(0xff161826, 0xff232532, 0xff232532, 0xff3f424d, 0xffe9e9ed,
                     0xff9184d9, 0xffe9e9ed, 0xff423a6a, 0xffe9e9ed),
              juce::Colour(0xff161826), juce::Colour(0xff0d0e16), juce::Colour(0xff0e0f18),
              juce::Colour(0xff232532), juce::Colour(0xff161826), juce::Colour(0xff232532),
              juce::Colour(0xff9184d9), false },
            { "Midnight", juce::LookAndFeel_V4::getMidnightColourScheme(),
              juce::Colour(0xff1c1c26), juce::Colour(0xff111118), juce::Colour(0xff24242e),
              juce::Colour(0xff3a3a4a), juce::Colour(0xff292934), juce::Colour(0xff262633),
              juce::Colour(0xffe9b83a), false },
            { "Grey", juce::LookAndFeel_V4::getGreyColourScheme(),
              juce::Colour(0xff383838), juce::Colour(0xff262626), juce::Colour(0xff444444),
              juce::Colour(0xff5c5c5c), juce::Colour(0xff404040), juce::Colour(0xff3c3c3c),
              juce::Colour(0xff6fb0e0), false },
            { "High Contrast",
              Scheme(0xff000000, 0xff000000, 0xff000000, 0xffffffff, 0xffffffff,
                     0xffffd400, 0xff000000, 0xffffd400, 0xffffffff),
              juce::Colour(0xff000000), juce::Colour(0xff000000), juce::Colour(0xff000000),
              juce::Colour(0xff3d3500), juce::Colour(0xff000000), juce::Colour(0xff000000),
              juce::Colour(0xffffd400), true,
              juce::Colour(0xff00ebff), juce::Colour(0xffffffff), juce::Colour(0xffffd400),
              juce::Colour(0xffff4040), juce::Colour(0xffff6b6b),
              juce::Colour(0xff002f33), juce::Colour(0xff001a1d) },
        };
        return themes;
    }

    inline const Theme& named(const juce::String& name)
    {
        for (const auto& theme : all())
            if (name == theme.name)
                return theme;
        return all().front();
    }

    /** The accents offered by name; Custom is any colour at all. */
    struct Accent
    {
        const char*  name;
        juce::Colour colour; // transparent: the theme's own
    };

    inline const std::vector<Accent>& accents()
    {
        static const std::vector<Accent> list {
            { "Theme's own", juce::Colours::transparentBlack },
            { "Blue", juce::Colour(0xff42a2c8) },
            { "Orange", juce::Colour(0xffff8c42) },
            { "Green", juce::Colour(0xff5ac46c) },
            { "Purple", juce::Colour(0xffa77be8) },
            { "Pink", juce::Colour(0xffe86fa8) },
            { "Gold", juce::Colour(0xffffd400) },
        };
        return list;
    }

    /** A pane's colour for @p role, from its look and feel, or Nocturne's when
        the look and feel isn't the app's (in a test, say). */
    inline juce::Colour colour(const juce::Component& component, int role)
    {
        auto& lookAndFeel = component.getLookAndFeel();
        if (lookAndFeel.isColourSpecified(role))
            return lookAndFeel.findColour(role);
        const auto& nocturne = all().front();
        switch (role)
        {
            case insetId:       return nocturne.inset;
            case workspaceId:   return nocturne.workspace;
            case tabActiveId:   return nocturne.tabActive;
            case tabInactiveId: return nocturne.tabInactive;
            case popupId:       return nocturne.popup;
            case focusRingId:   return nocturne.accent;
            case accentId:      return nocturne.accent;
            case signalId:      return nocturne.signal;
            case signalInkId:   return nocturne.signalInk;
            case warnId:        return nocturne.warn;
            case dangerId:      return nocturne.danger;
            case dangerTextId:  return nocturne.dangerText;
            case clipTopId:     return nocturne.clipTop;
            case clipBottomId:  return nocturne.clipBottom;
            default:            return nocturne.pane;
        }
    }

    /** colour(), for the roles a pane is painted on. */
    inline juce::Colour surface(const juce::Component& component, int role) { return colour(component, role); }
}

/** The app's look and feel: JUCE's, coloured by a theme, with a ring round
    the control that has keyboard focus when that's asked for (always, in
    High Contrast). */
class AppLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel() { apply(theme::all().front(), {}, false); }

    /** @p accent transparent: the theme's own. */
    void apply(const theme::Theme& theme, juce::Colour accent, bool focusRings)
    {
        const auto chosen = accent.isTransparent() ? theme.accent : accent;
        auto       scheme = theme.scheme;
        scheme.setUIColour(ColourScheme::defaultFill, chosen);
        setColourScheme(scheme);

        setColour(theme::paneId, theme.pane);
        setColour(theme::insetId, theme.inset);
        setColour(theme::workspaceId, theme.workspace);
        setColour(theme::tabActiveId, theme.tabActive);
        setColour(theme::tabInactiveId, theme.tabInactive);
        setColour(theme::popupId, theme.popup);
        setColour(theme::focusRingId, theme.focusRings ? juce::Colour(0xffffd400) : chosen);
        setColour(theme::accentId, chosen);
        setColour(theme::signalId, theme.signal);
        setColour(theme::signalInkId, theme.signalInk);
        setColour(theme::warnId, theme.warn);
        setColour(theme::dangerId, theme.danger);
        setColour(theme::dangerTextId, theme.dangerText);
        setColour(theme::clipTopId, theme.clipTop);
        setColour(theme::clipBottomId, theme.clipBottom);

        // The accent where JUCE's own widgets show something on or chosen.
        setColour(juce::TextButton::buttonOnColourId, chosen.withMultipliedSaturation(0.8f).darker(0.2f));
        setColour(juce::ToggleButton::tickColourId, chosen);
        setColour(juce::Slider::thumbColourId, chosen);
        setColour(juce::Slider::trackColourId, chosen.withAlpha(0.7f));
        setColour(juce::TextEditor::focusedOutlineColourId, chosen);
        setColour(juce::ComboBox::focusedOutlineColourId, chosen);
        setColour(juce::ListBox::backgroundColourId, theme.inset);

        // High Contrast: plain outlines everywhere, and selected text dark
        // on the yellow highlight.
        if (theme.focusRings)
        {
            setColour(juce::TextButton::buttonColourId, juce::Colours::black);
            setColour(juce::ComboBox::outlineColourId, juce::Colours::white);
            setColour(juce::TextEditor::outlineColourId, juce::Colours::white);
            setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            setColour(juce::TextButton::buttonOnColourId, chosen);
        }

        focusRings_ = focusRings || theme.focusRings;
        thickOutlines_ = theme.focusRings;
    }

    bool drawsFocusRings() const noexcept { return focusRings_; }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                              bool highlighted, bool down) override
    {
        LookAndFeel_V4::drawButtonBackground(g, button, background, highlighted, down);
        if (thickOutlines_)
        {
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawRoundedRectangle(button.getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);
        }
        ring(g, button, button.getLocalBounds());
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down) override
    {
        LookAndFeel_V4::drawToggleButton(g, button, highlighted, down);
        ring(g, button, button.getLocalBounds());
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool down, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox& box) override
    {
        LookAndFeel_V4::drawComboBox(g, width, height, down, buttonX, buttonY, buttonW, buttonH, box);
        ring(g, box, { 0, 0, width, height });
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float position, float minimum,
                          float maximum, juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, position, minimum, maximum, style, slider);
        ring(g, slider, { x, y, width, height });
    }

private:
    void ring(juce::Graphics& g, juce::Component& component, juce::Rectangle<int> area) const
    {
        if (! focusRings_ || ! component.hasKeyboardFocus(true))
            return;
        g.setColour(findColour(theme::focusRingId));
        g.drawRoundedRectangle(area.toFloat().reduced(1.0f), 4.0f, 2.0f);
    }

    bool focusRings_    = false;
    bool thickOutlines_ = false;
};

} // namespace soundsplice
