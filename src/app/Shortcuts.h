#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace looper::keys
{
/**
    The keyboard shortcuts, defined once.

    Each is the single source for both the key the app listens for and the
    text its menu item advertises, so the two can't drift; a menu promising a
    shortcut that does nothing is worse than no shortcut.

    **Built from key codes, never from KeyPress::createFromDescription.**
    That parser is a trap here, twice over. It splits on '+', so "command + -"
    is ambiguous to it. And it resolves key names with
    containsWholeWordIgnoreCase, which fails on multi-word names like
    "cursor left" — whereupon it falls through to "the last character of the
    string" and returns something plausible-looking and wrong.

    That is not hypothetical; it is what these were, and it had shipped.
    "command + backspace" resolved to command+E (the last letter of
    "backspace"), and "command + cursor left" and "command + cursor right"
    both resolved to command+T, since "left" and "right" both end in T. Delete
    Clip and both bar-step shortcuts had never worked, and the two bar steps
    were the same key as each other. Explicit key codes have no such failure
    mode.

    In a header rather than inside MainComponent.cpp so the table can be
    tested at all. The startup assertion that used to be the only guard
    checks validity only — every one of the wrong keys above was perfectly
    valid — and compiles out of the release build people actually run.
*/
namespace detail
{
    constexpr int cmd    = juce::ModifierKeys::commandModifier;
    constexpr int shift  = juce::ModifierKeys::shiftModifier;
    constexpr int alt    = juce::ModifierKeys::altModifier;
    constexpr int noMods = 0;

    inline juce::KeyPress with(int keyCode, int modifiers)
    {
        return juce::KeyPress(keyCode, juce::ModifierKeys(modifiers), 0);
    }
}

inline const juce::KeyPress newProject = detail::with('N', detail::cmd);
inline const juce::KeyPress open       = detail::with('O', detail::cmd);
inline const juce::KeyPress save       = detail::with('S', detail::cmd);
inline const juce::KeyPress saveAs     = detail::with('S', detail::cmd | detail::shift);
inline const juce::KeyPress exportAudio = detail::with('B', detail::cmd | detail::shift);

inline const juce::KeyPress undo       = detail::with('Z', detail::cmd);
inline const juce::KeyPress redo       = detail::with('Z', detail::cmd | detail::shift);
inline const juce::KeyPress redoAlt    = detail::with('Y', detail::cmd);

// Notes and clips get separate shortcuts for the same reason they get
// separate menu commands: one pair whose meaning depends on which pane has
// focus is a coin toss at the moment you press it.
//
// The audio editor is the deliberate exception. Cut/copy/paste on a waveform
// are the most standard shortcuts there are, and a user working in that pane
// reaches for them without thinking — so cmd+C/X/V mean audio while the Audio
// pane is the active one, and notes everywhere else. The discriminator is
// which pane is in front, not which control has keyboard focus, so it's
// visible on screen at the moment the key is pressed rather than being an
// invisible piece of state.
inline const juce::KeyPress cutAudio   = detail::with('X', detail::cmd);
inline const juce::KeyPress copyNotes  = detail::with('C', detail::cmd);
inline const juce::KeyPress pasteNotes = detail::with('V', detail::cmd);
inline const juce::KeyPress copyClip   = detail::with('C', detail::cmd | detail::shift);
inline const juce::KeyPress pasteClip  = detail::with('V', detail::cmd | detail::shift);
inline const juce::KeyPress duplicate  = detail::with('D', detail::cmd);
inline const juce::KeyPress quantize   = detail::with('U', detail::cmd);
inline const juce::KeyPress deleteClip = detail::with(juce::KeyPress::backspaceKey, detail::cmd);

// Track-level copy/paste sits on the alt variants: cmd+C/V are notes and
// cmd+shift+C/V are clips, so tracks take the remaining pair rather than
// overloading either with a third meaning.
inline const juce::KeyPress copyTrack      = detail::with('C', detail::cmd | detail::alt);
inline const juce::KeyPress pasteTrack     = detail::with('V', detail::cmd | detail::alt);
inline const juce::KeyPress duplicateTrack = detail::with('D', detail::cmd | detail::shift);

// Both spellings of "delete": on macOS the keycap labelled Delete is
// backspace, while forward-delete is a separate key that PC keyboards label
// Delete. Accepting both means the same keycap works everywhere.
inline const juce::KeyPress deleteTrack    = detail::with(juce::KeyPress::backspaceKey, detail::noMods);
inline const juce::KeyPress deleteTrackAlt = detail::with(juce::KeyPress::deleteKey, detail::noMods);

// Transport. Space is unmodified because it's the control reached for most,
// and every DAW spells it this way; a focused text field consumes its own
// keys first, so it can't interrupt typing.
inline const juce::KeyPress playPause  = detail::with(juce::KeyPress::spaceKey, detail::noMods);
inline const juce::KeyPress toStart    = detail::with(juce::KeyPress::homeKey, detail::noMods);
inline const juce::KeyPress toEnd      = detail::with(juce::KeyPress::endKey, detail::noMods);
inline const juce::KeyPress backOneBar = detail::with(juce::KeyPress::leftKey, detail::cmd);
inline const juce::KeyPress onOneBar   = detail::with(juce::KeyPress::rightKey, detail::cmd);
inline const juce::KeyPress record     = detail::with('R', detail::cmd);
inline const juce::KeyPress loop       = detail::with('L', detail::cmd);

inline const juce::KeyPress zoomIn  = detail::with('=', detail::cmd);
inline const juce::KeyPress zoomOut = detail::with('-', detail::cmd);

/** A shortcut with the name of what it does, so a test that finds a bad one
    can say which. */
struct NamedShortcut
{
    const char*    name;
    juce::KeyPress key;
};

/** Every shortcut the app binds. Adding one here is the point: an entry left
    out is an entry nothing checks. */
inline std::vector<NamedShortcut> all()
{
    return {
        { "New Project",     newProject },
        { "Open",            open },
        { "Save",            save },
        { "Save As",         saveAs },
        { "Export Audio",    exportAudio },

        { "Undo",            undo },
        { "Redo",            redo },
        { "Redo (alt)",      redoAlt },

        { "Copy Notes",      copyNotes },
        { "Paste Notes",     pasteNotes },
        { "Copy Clip",       copyClip },
        { "Paste Clip",      pasteClip },
        { "Duplicate Clip",  duplicate },
        { "Quantize",        quantize },
        { "Delete Clip",     deleteClip },

        { "Copy Track",      copyTrack },
        { "Paste Track",     pasteTrack },
        { "Duplicate Track", duplicateTrack },
        { "Delete Track",    deleteTrack },
        { "Delete Track (forward delete)", deleteTrackAlt },

        { "Play/Pause",      playPause },
        { "To Start",        toStart },
        { "To End",          toEnd },
        { "Back One Bar",    backOneBar },
        { "On One Bar",      onOneBar },
        { "Record",          record },
        { "Loop",            loop },

        { "Zoom In",         zoomIn },
        { "Zoom Out",        zoomOut },
    };
}

} // namespace looper::keys
