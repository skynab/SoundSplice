#pragma once

#include <cmath>
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
        okId          = 0x5e510010, // a pass, a healthy reading: always beside a word

        // Text and rules: what was white at one strength or another.
        textId        = 0x5e510011, // anything read: names, values, the selected tab
        textMutedId   = 0x5e510012, // secondary text, hints, idle labels
        textFaintId   = 0x5e510013, // axis labels and kickers: nothing anyone must act on
        dividerId     = 0x5e510014, // pane borders, the 0 dB line, separators
        dividerSoftId = 0x5e510015, // grid lines, rules between rows

        markerId      = 0x5e510016, // marker pins and their swatches
        meterMidId    = 0x5e510017, // a meter's middle, between ok and danger
        envelopeId    = 0x5e510018, // a clip's volume envelope and its points
    };

    struct Theme
    {
        const char*                            name;
        juce::LookAndFeel_V4::ColourScheme     scheme;
        juce::Colour                           pane, inset, workspace, tabActive, tabInactive, popup;
        juce::Colour                           accent;     // its own, when the user hasn't picked one
        bool                                   focusRings; // always, rather than when asked for

        // SoundSplice's signal colours, which every theme but High Contrast
        // shares. The audio is drawn in the accent's own family a step lighter
        // (color-accent-400), so a waveform reads as the thing being edited;
        // the playhead is lighter still (color-accent-200). Markers are amber,
        // meters run green to yellow to red, and danger is a step darker for
        // fills than for text, which needs 4.5:1 on the panes.
        juce::Colour signal     { 0xffb5abfc }; // color-accent-400
        juce::Colour signalInk  { 0xffe7e5fe }; // color-accent-200
        juce::Colour warn       { 0xffe4b750 }; // oklch(0.80 0.13 85)
        juce::Colour danger     { 0xffe9504d }; // oklch(0.64 0.19 25)
        juce::Colour dangerText { 0xffff948c }; // oklch(0.80 0.15 25)
        juce::Colour clipTop    { 0xff2b2741 }; // color-accent-900
        juce::Colour clipBottom { 0xff1a1c2a }; // the plot
        juce::Colour ok         { 0xff6fc082 }; // oklch(0.74 0.12 150)

        // The design system's text and rules are its text colour at a strength,
        // so a tint of text over any pane reads the same; High Contrast's are
        // solid, its text at 7:1 or better and its rules at 3:1 on black.
        juce::Colour text        { 0xffe9e9ed }; // color-text
        juce::Colour textMuted   { 0x8ce9e9ed }; // color-text-muted, 55%
        juce::Colour textFaint   { 0x61e9e9ed }; // color-text-faint, 38%
        juce::Colour divider     { 0x29e9e9ed }; // color-divider, 16%
        juce::Colour dividerSoft { 0x12e9e9ed }; // color-divider-soft, 7%

        juce::Colour marker   { 0xffe4b750 }; // oklch(0.80 0.13 85)
        juce::Colour meterMid { 0xffdec358 }; // oklch(0.82 0.13 95)
        juce::Colour envelope { 0xfff2ce59 }; // oklch(0.86 0.14 92)
    };

    inline const std::vector<Theme>& all()
    {
        using Scheme = juce::LookAndFeel_V4::ColourScheme;
        static const std::vector<Theme> themes {
            // The design system's own, as the SoundSplice mockups use it: the
            // window's chrome (title, toolbar, transport) on color-bg, panes
            // floating on #0d0e16 in color-surface, plots sunk to color-bg
            // 70% into the surface, and the blurple accent. Controls sit on
            // color-bg; the scheme's outline is color-neutral-800 and its
            // highlight color-accent-800.
            { "Nocturne",
              Scheme(0xff161826, 0xff161826, 0xff232532, 0xff3f424d, 0xffe9e9ed,
                     0xff9184d9, 0xffe9e9ed, 0xff423a6a, 0xffe9e9ed),
              juce::Colour(0xff232532), juce::Colour(0xff1a1c2a), juce::Colour(0xff0d0e16),
              juce::Colour(0xff232532), juce::Colour(0xff232532), juce::Colour(0xff232532),
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
              juce::Colour(0xff002f33), juce::Colour(0xff001a1d), juce::Colour(0xff7dff8a),
              juce::Colour(0xffffffff), juce::Colour(0xffd6d6d6), juce::Colour(0xffbdbdbd),
              juce::Colour(0xffffffff), juce::Colour(0xff8a8a8a) },
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
            case okId:          return nocturne.ok;
            case textId:        return nocturne.text;
            case textMutedId:   return nocturne.textMuted;
            case textFaintId:   return nocturne.textFaint;
            case dividerId:     return nocturne.divider;
            case dividerSoftId: return nocturne.dividerSoft;
            case markerId:      return nocturne.marker;
            case meterMidId:    return nocturne.meterMid;
            case envelopeId:    return nocturne.envelope;
            default:            return nocturne.pane;
        }
    }

    /** colour(), for the roles a pane is painted on. */
    inline juce::Colour surface(const juce::Component& component, int role) { return colour(component, role); }

    /** A step of @p accent's tonal ramp, as the design system numbers them:
        500 is the accent itself, 100-400 lighter (for text and marks on the
        dark panes), 600-900 darker (for tinted fills). Derived rather than
        stored, so a chosen accent gets a ramp of its own. */
    inline juce::Colour accentStep(juce::Colour accent, int step)
    {
        switch (step)
        {
            case 100: return accent.interpolatedWith(juce::Colour(0xfff5f4ff), 0.95f);
            case 200: return accent.interpolatedWith(juce::Colour(0xfff5f4ff), 0.80f);
            case 300: return accent.interpolatedWith(juce::Colour(0xfff5f4ff), 0.60f);
            case 400: return accent.interpolatedWith(juce::Colour(0xfff5f4ff), 0.40f);
            case 600: return accent.interpolatedWith(juce::Colour(0xff161826), 0.20f);
            case 700: return accent.interpolatedWith(juce::Colour(0xff161826), 0.42f);
            case 800: return accent.interpolatedWith(juce::Colour(0xff161826), 0.64f);
            case 900: return accent.interpolatedWith(juce::Colour(0xff161826), 0.83f);
            default:  return accent;
        }
    }

    /** accentStep() of the accent @p component is drawn with. */
    inline juce::Colour accentStep(const juce::Component& component, int step)
    {
        return accentStep(colour(component, accentId), step);
    }

    /** The face the interface is set in: Inter, the design system's, where
        it's installed, and the platform's own UI face otherwise. */
    inline juce::String uiTypefaceName()
    {
        static const juce::String name = []
        {
            const auto installed = juce::Font::findAllTypefaceNames();
            for (const auto* wanted : { "Inter", "Segoe UI" })
                if (installed.contains(wanted))
                    return juce::String(wanted);
            return juce::String();
        }();
        return name;
    }

    /** The interface face at @p size, given as the mockups give it: a CSS
        font size, which is the em - what JUCE calls a point height - rather
        than JUCE's own height, the whole line, which would set every size
        about a fifth too small. */
    inline juce::Font font(float size, bool bold = false)
    {
        juce::Font f(juce::FontOptions().withPointHeight(size));
        return bold ? f.boldened() : f;
    }

    /** The face times, levels and other figures are set in, so their digits
        line up as they change, at a CSS-style @p size as font() takes. */
    inline juce::Font monoFont(float size)
    {
        static const juce::String name = []
        {
            const auto installed = juce::Font::findAllTypefaceNames();
            for (const auto* wanted : { "Cascadia Mono", "Consolas", "SF Mono", "Menlo" })
                if (installed.contains(wanted))
                    return juce::String(wanted);
            return juce::Font::getDefaultMonospacedFontName();
        }();
        return juce::Font(juce::FontOptions(name, size, juce::Font::plain).withPointHeight(size));
    }

    /** How a TextButton is drawn, set as its "style" property (see
        setStyle). Secondary, an outline in the divider colour, is what a
        button with no style gets. */
    namespace buttonStyle
    {
        inline constexpr const char* primary   = "primary";   // the one action that matters: an accent outline
        inline constexpr const char* secondary = "secondary"; // an outline in the divider colour
        inline constexpr const char* ghost     = "ghost";     // accent text, no outline until hovered
        inline constexpr const char* chip      = "chip";      // a tool: no outline, lit in the accent when on
    }

    inline void setStyle(juce::Component& button, const char* style)
    {
        button.getProperties().set("style", style);
        button.repaint();
    }

    inline juce::String styleOf(const juce::Component& button)
    {
        const auto style = button.getProperties()["style"].toString();
        return style.isEmpty() ? juce::String(buttonStyle::secondary) : style;
    }
}

/** The app's look and feel: JUCE's widgets drawn to the design system -
    outlined buttons, slim sliders with round thumbs, rounded inputs and
    menus on the surface - coloured by a theme, with a ring round the control
    that has keyboard focus when that's asked for (always, in High Contrast,
    which keeps JUCE's plainer drawing with heavy outlines). */
class AppLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel()
    {
        if (theme::uiTypefaceName().isNotEmpty())
            setDefaultSansSerifTypefaceName(theme::uiTypefaceName());
        apply(theme::all().front(), {}, false);
    }

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
        setColour(theme::okId, theme.ok);
        setColour(theme::textId, theme.text);
        setColour(theme::textMutedId, theme.textMuted);
        setColour(theme::textFaintId, theme.textFaint);
        setColour(theme::dividerId, theme.divider);
        setColour(theme::dividerSoftId, theme.dividerSoft);
        setColour(theme::markerId, theme.marker);
        setColour(theme::meterMidId, theme.meterMid);
        setColour(theme::envelopeId, theme.envelope);

        const auto ground = scheme.getUIColour(ColourScheme::widgetBackground);

        // The accent where JUCE's own widgets show something on or chosen.
        setColour(juce::TextButton::buttonOnColourId, chosen.withMultipliedSaturation(0.8f).darker(0.2f));
        setColour(juce::TextButton::textColourOffId, theme.text);
        setColour(juce::TextButton::textColourOnId, theme::accentStep(chosen, 200));
        setColour(juce::ToggleButton::tickColourId, chosen);
        setColour(juce::ToggleButton::textColourId, theme.text);
        setColour(juce::Slider::thumbColourId, theme::accentStep(chosen, 300));
        setColour(juce::Slider::trackColourId, chosen);
        setColour(juce::Slider::backgroundColourId, theme.text.withAlpha(0.10f));
        setColour(juce::Slider::rotarySliderFillColourId, theme::accentStep(chosen, 300));
        setColour(juce::Slider::rotarySliderOutlineColourId, theme.text.withAlpha(0.22f));
        setColour(juce::Slider::textBoxBackgroundColourId, ground);
        setColour(juce::Slider::textBoxOutlineColourId, theme.divider);
        setColour(juce::Slider::textBoxTextColourId, theme.text);
        setColour(juce::TextEditor::backgroundColourId, ground);
        setColour(juce::TextEditor::outlineColourId, theme.divider);
        setColour(juce::TextEditor::focusedOutlineColourId, chosen);
        setColour(juce::TextEditor::highlightColourId, chosen.withAlpha(0.30f));
        setColour(juce::CaretComponent::caretColourId, chosen);
        setColour(juce::ComboBox::backgroundColourId, ground);
        setColour(juce::ComboBox::outlineColourId, theme.divider);
        setColour(juce::ComboBox::focusedOutlineColourId, chosen);
        setColour(juce::ComboBox::arrowColourId, theme.textMuted);
        setColour(juce::PopupMenu::backgroundColourId, theme.popup);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, chosen.withAlpha(0.18f));
        setColour(juce::PopupMenu::highlightedTextColourId, theme::accentStep(chosen, 100));
        setColour(juce::ListBox::backgroundColourId, theme.pane);
        setColour(juce::TreeView::backgroundColourId, theme.pane);
        setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff3f424d));
        setColour(juce::TooltipWindow::backgroundColourId, theme.popup);
        setColour(juce::TooltipWindow::textColourId, theme.text);
        setColour(juce::TooltipWindow::outlineColourId, juce::Colour(0xff595d6c));
        setColour(juce::AlertWindow::backgroundColourId, theme.popup);
        setColour(juce::AlertWindow::outlineColourId, juce::Colour(0xff595d6c));
        setColour(juce::Label::textColourId, theme.text);
        setColour(juce::ProgressBar::foregroundColourId, chosen);
        setColour(juce::ProgressBar::backgroundColourId, theme.text.withAlpha(0.08f));

        // High Contrast: plain outlines everywhere, and selected text dark
        // on the yellow highlight.
        if (theme.focusRings)
        {
            setColour(juce::TextButton::buttonColourId, juce::Colours::black);
            setColour(juce::ComboBox::outlineColourId, juce::Colours::white);
            setColour(juce::TextEditor::outlineColourId, juce::Colours::white);
            setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            setColour(juce::TextButton::buttonOnColourId, chosen);
            setColour(juce::PopupMenu::highlightedBackgroundColourId, chosen);
            setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
        }

        focusRings_    = focusRings || theme.focusRings;
        thickOutlines_ = theme.focusRings;
    }

    bool drawsFocusRings() const noexcept { return focusRings_; }

    //==============================================================================
    // Buttons

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
    {
        return theme::font(juce::jmin(12.0f, (float) buttonHeight * 0.44f));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                              bool highlighted, bool down) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawButtonBackground(g, button, background, highlighted, down);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawRoundedRectangle(button.getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);
            ring(g, button, button.getLocalBounds());
            return;
        }

        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto style  = theme::styleOf(button);
        const auto accent = findColour(theme::accentId);
        const auto text   = findColour(theme::textId);
        const bool on     = button.getToggleState();
        const float r     = juce::jmin(kRadius, bounds.getHeight() * 0.5f);

        juce::Colour fill   = juce::Colours::transparentBlack;
        juce::Colour border = juce::Colours::transparentBlack;

        if (on && hasOwnOnColour(button))
        {
            // A lit state with a meaning of its own (mute, solo, arm): the
            // button's colour as a tint and an edge, the way the mockups
            // light a track's M, S and record buttons.
            const auto lit = button.findColour(juce::TextButton::buttonOnColourId);
            fill   = lit.withAlpha(down ? 0.40f : 0.26f);
            border = lit.withAlpha(0.9f);
        }
        else if (style == theme::buttonStyle::primary)
        {
            border = accent;
            fill   = accent.withAlpha(down ? 0.22f : (highlighted || on ? 0.12f : 0.0f));
        }
        else if (style == theme::buttonStyle::ghost)
        {
            fill = accent.withAlpha(down ? 0.18f : (highlighted ? 0.10f : 0.0f));
        }
        else if (style == theme::buttonStyle::chip)
        {
            fill = on ? accent.withAlpha(down ? 0.26f : 0.18f)
                      : (down ? accent.withAlpha(0.22f) : text.withAlpha(highlighted ? 0.08f : 0.0f));
        }
        else
        {
            border = on ? accent.withAlpha(0.7f) : findColour(theme::dividerId);
            fill   = on ? accent.withAlpha(down ? 0.26f : 0.18f)
                        : text.withAlpha(down ? 0.14f : (highlighted ? 0.07f : 0.0f));
        }

        if (! button.isEnabled())
        {
            fill   = fill.withMultipliedAlpha(0.45f);
            border = border.withMultipliedAlpha(0.45f);
        }

        g.setColour(fill);
        g.fillRoundedRectangle(bounds, r);
        if (! border.isTransparent())
        {
            g.setColour(border);
            g.drawRoundedRectangle(bounds, r, 1.0f);
        }
        ring(g, button, button.getLocalBounds());
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawButtonText(g, button, highlighted, down);
            return;
        }

        const auto style  = theme::styleOf(button);
        const auto accent = findColour(theme::accentId);
        const bool on     = button.getToggleState();

        juce::Colour ink;
        if (on && hasOwnOnColour(button))
            ink = button.findColour(juce::TextButton::buttonOnColourId).brighter(0.35f);
        else if (style == theme::buttonStyle::primary || style == theme::buttonStyle::ghost)
            ink = highlighted ? theme::accentStep(accent, 300) : accent;
        else if (on)
            ink = theme::accentStep(accent, 200);
        else if (style == theme::buttonStyle::chip)
            ink = findColour(theme::textId).withAlpha(highlighted ? 1.0f : 0.62f);
        else
            ink = button.findColour(juce::TextButton::textColourOffId);

        if (! button.isEnabled())
            ink = ink.withMultipliedAlpha(0.45f);

        g.setFont(getTextButtonFont(button, button.getHeight()));
        g.setColour(ink);
        g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(juce::jmin(8, button.getWidth() / 6), 0),
                         juce::Justification::centred, 1, 0.8f);
    }

    void drawDrawableButton(juce::Graphics& g, juce::DrawableButton& button, bool highlighted, bool down) override
    {
        // Icon buttons are tools: no box until the mouse is over one, a tint
        // of the accent when pressed or on.
        if (! thickOutlines_)
        {
            const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
            const auto accent = findColour(theme::accentId);
            juce::Colour fill = juce::Colours::transparentBlack;
            if (down)
                fill = accent.withAlpha(0.22f);
            else if (button.getToggleState() && button.getClickingTogglesState())
                fill = accent.withAlpha(0.18f);
            else if (highlighted)
                fill = findColour(theme::textId).withAlpha(0.08f);
            g.setColour(fill);
            g.fillRoundedRectangle(bounds, 6.0f);
        }
        LookAndFeel_V4::drawDrawableButton(g, button, highlighted, down);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawToggleButton(g, button, highlighted, down);
            ring(g, button, button.getLocalBounds());
            return;
        }

        const float side = juce::jmin(14.0f, (float) button.getHeight() - 4.0f);
        const auto  box  = juce::Rectangle<float>(4.0f, ((float) button.getHeight() - side) * 0.5f, side, side);
        drawTickBox(g, button, box.getX(), box.getY(), box.getWidth(), box.getHeight(), button.getToggleState(),
                    button.isEnabled(), highlighted, down);

        g.setColour(button.findColour(juce::ToggleButton::textColourId).withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.45f));
        g.setFont(theme::font(juce::jmin(12.0f, (float) button.getHeight() * 0.5f)));
        g.drawFittedText(button.getButtonText(),
                         button.getLocalBounds().withTrimmedLeft(juce::roundToInt(box.getRight()) + 7).withTrimmedRight(2),
                         juce::Justification::centredLeft, 1);
        ring(g, button, button.getLocalBounds());
    }

    void drawTickBox(juce::Graphics& g, juce::Component& component, float x, float y, float w, float h, bool ticked,
                     bool enabled, bool highlighted, bool down) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawTickBox(g, component, x, y, w, h, ticked, enabled, highlighted, down);
            return;
        }

        const auto box    = juce::Rectangle<float>(x, y, w, h).reduced(0.75f);
        const auto accent = findColour(theme::accentId);
        const float alpha = enabled ? 1.0f : 0.45f;
        if (ticked)
        {
            g.setColour(accent.withMultipliedAlpha(alpha));
            g.fillRoundedRectangle(box, 4.0f);
            juce::Path tick;
            tick.startNewSubPath(box.getX() + box.getWidth() * 0.24f, box.getCentreY());
            tick.lineTo(box.getX() + box.getWidth() * 0.43f, box.getY() + box.getHeight() * 0.70f);
            tick.lineTo(box.getRight() - box.getWidth() * 0.22f, box.getY() + box.getHeight() * 0.30f);
            g.setColour(findColour(juce::ResizableWindow::backgroundColourId).withMultipliedAlpha(alpha));
            g.strokePath(tick, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else
        {
            g.setColour(findColour(theme::textId).withAlpha(down ? 0.14f : (highlighted ? 0.07f : 0.0f)));
            g.fillRoundedRectangle(box, 4.0f);
            g.setColour((highlighted ? accent : findColour(theme::dividerId).withAlpha(0.5f)).withMultipliedAlpha(alpha));
            g.drawRoundedRectangle(box, 4.0f, 1.5f);
        }
    }

    //==============================================================================
    // Inputs

    void drawComboBox(juce::Graphics& g, int width, int height, bool down, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox& box) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawComboBox(g, width, height, down, buttonX, buttonY, buttonW, buttonH, box);
            ring(g, box, { 0, 0, width, height });
            return;
        }

        const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height).reduced(0.5f);
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle(bounds, kRadius);
        g.setColour(box.hasKeyboardFocus(true) ? box.findColour(juce::ComboBox::focusedOutlineColourId)
                    : box.isMouseOver(true)    ? findColour(theme::textId).withAlpha(0.45f)
                                               : box.findColour(juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle(bounds, kRadius, 1.0f);

        const auto arrow = juce::Rectangle<float>((float) buttonX, (float) buttonY, (float) buttonW, (float) buttonH)
                               .withSizeKeepingCentre(10.0f, 10.0f);
        juce::Path caret;
        caret.startNewSubPath(arrow.getX(), arrow.getY() + 3.0f);
        caret.lineTo(arrow.getCentreX(), arrow.getBottom() - 2.5f);
        caret.lineTo(arrow.getRight(), arrow.getY() + 3.0f);
        g.setColour(box.findColour(juce::ComboBox::arrowColourId).withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.45f));
        g.strokePath(caret, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override
    {
        return theme::font(juce::jmin(12.0f, (float) box.getHeight() * 0.46f));
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(1, 1, box.getWidth() - 22, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
    }

    void fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        if (thickOutlines_ || dynamic_cast<juce::AlertWindow*>(editor.getParentComponent()) != nullptr)
        {
            LookAndFeel_V4::fillTextEditorBackground(g, width, height, editor);
            return;
        }
        g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle(juce::Rectangle<float>((float) width, (float) height).reduced(0.5f), kRadius);
    }

    void drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawTextEditorOutline(g, width, height, editor);
            return;
        }
        if (! editor.isEnabled() || editor.isReadOnly())
            return;
        const auto bounds = juce::Rectangle<float>((float) width, (float) height).reduced(0.5f);
        g.setColour(editor.hasKeyboardFocus(true) ? editor.findColour(juce::TextEditor::focusedOutlineColourId)
                    : editor.isMouseOver(true)    ? findColour(theme::textId).withAlpha(0.45f)
                                                  : editor.findColour(juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle(bounds, kRadius, 1.0f);
    }

    //==============================================================================
    // Sliders

    int getSliderThumbRadius(juce::Slider& slider) override
    {
        return slider.isRotary() ? LookAndFeel_V4::getSliderThumbRadius(slider) : 6;
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float position, float minimum,
                          float maximum, juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (! thickOutlines_ && style == juce::Slider::LinearBar)
        {
            // A value box, as the mockups show a figure that can be dragged:
            // the ground, an outline, and how far along it is as a tint.
            const auto box = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(0.5f);
            g.setColour(findColour(juce::ResizableWindow::backgroundColourId));
            g.fillRoundedRectangle(box, 5.0f);
            g.setColour(slider.findColour(juce::Slider::trackColourId).withAlpha(0.16f));
            g.fillRoundedRectangle(box.withRight(juce::jlimit(box.getX(), box.getRight(), position)), 5.0f);
            g.setColour(slider.isMouseOverOrDragging() ? findColour(theme::textId).withAlpha(0.45f)
                                                       : findColour(theme::dividerId));
            g.drawRoundedRectangle(box, 5.0f, 1.0f);
            return;
        }

        const bool plain = style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearVertical;
        if (thickOutlines_ || ! plain)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, position, minimum, maximum, style, slider);
            ring(g, slider, { x, y, width, height });
            return;
        }

        const bool  horizontal = style == juce::Slider::LinearHorizontal;
        const auto  area       = juce::Rectangle<int>(x, y, width, height).toFloat();
        const float alpha      = slider.isEnabled() ? 1.0f : 0.45f;

        // A thin track, filled in the accent up to a round thumb.
        const float thickness = horizontal ? 3.0f : 4.0f;
        const auto  track = horizontal ? juce::Rectangle<float>(area.getX(), area.getCentreY() - thickness * 0.5f,
                                                                area.getWidth(), thickness)
                                       : juce::Rectangle<float>(area.getCentreX() - thickness * 0.5f, area.getY(),
                                                                thickness, area.getHeight());
        g.setColour(slider.findColour(juce::Slider::backgroundColourId).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track, thickness * 0.5f);

        const auto filled = horizontal ? track.withRight(position) : track.withTop(position);
        g.setColour(slider.findColour(juce::Slider::trackColourId).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(filled, thickness * 0.5f);

        if (horizontal)
        {
            const auto thumb = juce::Rectangle<float>(12.0f, 12.0f).withCentre({ position, area.getCentreY() });
            g.setColour(findColour(juce::ResizableWindow::backgroundColourId));
            g.fillEllipse(thumb.expanded(2.0f));
            g.setColour(slider.findColour(juce::Slider::thumbColourId).withMultipliedAlpha(alpha));
            g.fillEllipse(thumb);
        }
        else
        {
            // A fader cap: wide and flat, with a line across its middle.
            const auto cap = juce::Rectangle<float>(juce::jmin(26.0f, area.getWidth()), 11.0f)
                                 .withCentre({ area.getCentreX(), position });
            g.setColour(juce::Colour(0xff595d6c).withMultipliedAlpha(alpha));
            g.fillRoundedRectangle(cap, 2.5f);
            g.setColour(juce::Colour(0xff9397ab).withMultipliedAlpha(alpha));
            g.drawRoundedRectangle(cap.reduced(0.5f), 2.5f, 1.0f);
            g.setColour(findColour(theme::textId).withMultipliedAlpha(alpha));
            g.fillRect(cap.getX() + 4.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 8.0f, 1.0f);
        }
        ring(g, slider, { x, y, width, height });
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position, float startAngle,
                          float endAngle, juce::Slider& slider) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawRotarySlider(g, x, y, width, height, position, startAngle, endAngle, slider);
            return;
        }

        // A plain ring with a pointer, as the mockups draw a track's pan.
        const auto  bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(3.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto  centre = bounds.getCentre();
        const float angle  = startAngle + position * (endAngle - startAngle);
        const float alpha  = slider.isEnabled() ? 1.0f : 0.45f;

        g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId).withMultipliedAlpha(alpha));
        g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre), 1.5f);

        juce::Path value;
        value.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, startAngle, angle, true);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).withAlpha(0.55f * alpha));
        g.strokePath(value, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).withMultipliedAlpha(alpha));
        g.drawLine(juce::Line<float>(centre, centre.getPointOnCircumference(radius * 0.85f, angle)), 1.75f);
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(theme::monoFont(11.5f));
        return label;
    }

    //==============================================================================
    // Menus, scrollbars, tooltips

    void drawMenuBarBackground(juce::Graphics& g, int width, int height, bool, juce::MenuBarComponent&) override
    {
        const auto ground = findColour(juce::ResizableWindow::backgroundColourId);
        if (thickOutlines_)
        {
            g.fillAll(ground);
            return;
        }
        g.setGradientFill(juce::ColourGradient::vertical(juce::Colour(0xff292b31), 0.0f, ground, (float) height));
        g.fillAll();
        g.setColour(findColour(theme::dividerId));
        g.fillRect(0, height - 1, width, 1);
    }

    void drawMenuBarItem(juce::Graphics& g, int width, int height, int itemIndex, const juce::String& text, bool over,
                         bool open, bool overBar, juce::MenuBarComponent& bar) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawMenuBarItem(g, width, height, itemIndex, text, over, open, overBar, bar);
            return;
        }
        const auto box = juce::Rectangle<float>((float) width, (float) height).reduced(1.0f, 4.0f);
        if (open || over)
        {
            g.setColour(findColour(theme::textId).withAlpha(0.09f));
            g.fillRoundedRectangle(box, 5.0f);
        }
        g.setColour(findColour(theme::textId).withAlpha(open || over ? 1.0f : 0.78f));
        g.setFont(getMenuBarFont(bar, itemIndex, text));
        g.drawFittedText(text, 0, 0, width, height, juce::Justification::centred, 1);
    }

    juce::Font getMenuBarFont(juce::MenuBarComponent&, int, const juce::String&) override
    {
        return theme::font(12.5f);
    }

    int getMenuBarItemWidth(juce::MenuBarComponent& bar, int itemIndex, const juce::String& text) override
    {
        return (int) std::ceil(juce::GlyphArrangement::getStringWidth(getMenuBarFont(bar, itemIndex, text), text)) + 18;
    }

    juce::Font getPopupMenuFont() override { return theme::font(12.5f); }

    void drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                            const juce::PopupMenu::Options& options) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawPopupMenuBackgroundWithOptions(g, width, height, options);
            return;
        }
        g.fillAll(findColour(juce::PopupMenu::backgroundColourId));
        g.setColour(juce::Colour(0xff595d6c));
        g.drawRect(0, 0, width, height, 1);
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool separator, bool active,
                           bool highlighted, bool ticked, bool subMenu, const juce::String& text,
                           const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawPopupMenuItem(g, area, separator, active, highlighted, ticked, subMenu, text, shortcut,
                                              icon, textColour);
            return;
        }

        if (separator)
        {
            g.setColour(findColour(theme::dividerId));
            g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
            return;
        }

        const auto row = area.reduced(4, 1).toFloat();
        if (highlighted && active)
        {
            g.setColour(findColour(juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRoundedRectangle(row, 5.0f);
        }

        auto ink = textColour != nullptr ? *textColour
                   : highlighted && active ? findColour(juce::PopupMenu::highlightedTextColourId)
                                           : findColour(theme::textId);
        if (! active)
            ink = ink.withMultipliedAlpha(0.4f);

        auto r = area.reduced(12, 0);
        auto check = r.removeFromLeft(16).toFloat();
        if (icon != nullptr)
            icon->drawWithin(g, check.reduced(1.0f), juce::RectanglePlacement::centred, ink.getFloatAlpha());
        else if (ticked)
        {
            juce::Path tick;
            const auto c = check.withSizeKeepingCentre(10.0f, 10.0f);
            tick.startNewSubPath(c.getX(), c.getCentreY());
            tick.lineTo(c.getX() + 3.5f, c.getBottom() - 1.5f);
            tick.lineTo(c.getRight(), c.getY() + 1.5f);
            g.setColour(theme::accentStep(findColour(theme::accentId), 300).withMultipliedAlpha(ink.getFloatAlpha()));
            g.strokePath(tick, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        r.removeFromLeft(6);

        if (subMenu)
        {
            const auto arrow = r.removeFromRight(10).toFloat().withSizeKeepingCentre(5.0f, 9.0f);
            juce::Path caret;
            caret.startNewSubPath(arrow.getX(), arrow.getY());
            caret.lineTo(arrow.getRight(), arrow.getCentreY());
            caret.lineTo(arrow.getX(), arrow.getBottom());
            g.setColour(ink.withMultipliedAlpha(0.7f));
            g.strokePath(caret, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setFont(getPopupMenuFont());
        if (shortcut.isNotEmpty())
        {
            g.setColour(ink.withMultipliedAlpha(0.5f));
            g.drawText(shortcut, r, juce::Justification::centredRight, true);
            r.removeFromRight(juce::roundToInt(juce::GlyphArrangement::getStringWidth(getPopupMenuFont(), shortcut)) + 12);
        }
        g.setColour(ink);
        g.drawFittedText(text, r, juce::Justification::centredLeft, 1);
    }

    void getIdealPopupMenuItemSize(const juce::String& text, bool separator, int standardHeight, int& width,
                                   int& height) override
    {
        LookAndFeel_V4::getIdealPopupMenuItemSize(text, separator, standardHeight, width, height);
        if (! separator && standardHeight <= 0)
            height = 26;
        width += 12;
    }

    int getDefaultScrollbarWidth() override { return 9; }

    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height, bool vertical,
                       int thumbStart, int thumbSize, bool over, bool down) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawScrollbar(g, bar, x, y, width, height, vertical, thumbStart, thumbSize, over, down);
            return;
        }
        const auto thumb = vertical ? juce::Rectangle<int>(x, thumbStart, width, thumbSize)
                                    : juce::Rectangle<int>(thumbStart, y, thumbSize, height);
        g.setColour(bar.findColour(juce::ScrollBar::thumbColourId).brighter(over || down ? 0.25f : 0.0f));
        g.fillRoundedRectangle(thumb.toFloat().reduced(1.5f), 4.0f);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawTooltip(g, text, width, height);
            return;
        }
        const auto bounds = juce::Rectangle<float>((float) width, (float) height);
        g.setColour(findColour(juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(findColour(juce::TooltipWindow::outlineColourId));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);

        juce::AttributedString attributed;
        attributed.setJustification(juce::Justification::centredLeft);
        attributed.append(text, theme::font(12.0f), findColour(juce::TooltipWindow::textColourId));
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(attributed, (float) width - 16.0f);
        layout.draw(g, bounds.reduced(8.0f, 5.0f));
    }

    juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> position,
                                          juce::Rectangle<int> parentArea) override
    {
        juce::AttributedString attributed;
        attributed.append(text, theme::font(12.0f));
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(attributed, 360.0f);
        const int w = (int) std::ceil(layout.getWidth()) + 16;
        const int h = (int) std::ceil(layout.getHeight()) + 10;
        return juce::Rectangle<int>(position.x > parentArea.getCentreX() ? position.x - (w + 12) : position.x + 12,
                                    position.y > parentArea.getCentreY() ? position.y - (h + 6) : position.y + 6, w, h)
            .constrainedWithin(parentArea);
    }

    void drawTableHeaderBackground(juce::Graphics& g, juce::TableHeaderComponent& header) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawTableHeaderBackground(g, header);
            return;
        }
        // No bar of its own: small capitals on the pane, ruled off below.
        g.setColour(findColour(theme::dividerId));
        g.fillRect(0, header.getHeight() - 1, header.getWidth(), 1);
    }

    void drawTableHeaderColumn(juce::Graphics& g, juce::TableHeaderComponent& header, const juce::String& name,
                               int columnId, int width, int height, bool over, bool down, int flags) override
    {
        if (thickOutlines_)
        {
            LookAndFeel_V4::drawTableHeaderColumn(g, header, name, columnId, width, height, over, down, flags);
            return;
        }
        if (over || down)
        {
            g.setColour(findColour(theme::textId).withAlpha(down ? 0.08f : 0.04f));
            g.fillRect(0, 0, width, height - 1);
        }
        auto area = juce::Rectangle<int>(width, height).reduced(8, 0);
        const bool sortedUp   = (flags & juce::TableHeaderComponent::sortedForwards) != 0;
        const bool sortedDown = (flags & juce::TableHeaderComponent::sortedBackwards) != 0;
        if (sortedUp || sortedDown)
        {
            const auto arrow = area.removeFromRight(10).toFloat().withSizeKeepingCentre(8.0f, 5.0f);
            juce::Path caret;
            caret.startNewSubPath(arrow.getX(), sortedUp ? arrow.getBottom() : arrow.getY());
            caret.lineTo(arrow.getCentreX(), sortedUp ? arrow.getY() : arrow.getBottom());
            caret.lineTo(arrow.getRight(), sortedUp ? arrow.getBottom() : arrow.getY());
            g.setColour(findColour(theme::textId).withAlpha(0.45f));
            g.strokePath(caret, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour(findColour(theme::textId).withAlpha(0.42f));
        g.setFont(theme::font(9.5f).withExtraKerningFactor(0.08f));
        g.drawText(name.toUpperCase(), area, juce::Justification::centredLeft, true);
    }

private:
    static constexpr float kRadius = 6.0f;

    /** Whether @p button lights in a colour of its own when on, rather than
        the accent every other toggle uses. */
    bool hasOwnOnColour(const juce::Button& button) const
    {
        return button.isColourSpecified(juce::TextButton::buttonOnColourId);
    }

    void ring(juce::Graphics& g, juce::Component& component, juce::Rectangle<int> area) const
    {
        if (! focusRings_ || ! component.hasKeyboardFocus(true))
            return;
        g.setColour(findColour(theme::focusRingId));
        g.drawRoundedRectangle(area.toFloat().reduced(1.0f), 6.0f, 2.0f);
    }

    bool focusRings_    = false;
    bool thickOutlines_ = false;
};

} // namespace soundsplice
