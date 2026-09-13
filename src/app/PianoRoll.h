#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/MidiNote.h"
#include "engine/Pattern.h"
#include "model/DrumKit.h"

#include "PianoRollGeometry.h"
#include "TrackColours.h"
#include "model/Track.h"

namespace looper
{
/**
    A step grid of rows x time steps, with a left-hand gutter naming each row —
    a pitch (e.g. "C4") in the usual melodic mode, or a pad name ("Kick",
    "Snare", ...) in drum mode (see setDrumPads) — the same way ArrangementView
    names each of its lanes.

    Clicking an empty cell adds a note and clicking an existing one removes it:
    the original one-click-per-step behaviour, kept because it's the fastest way
    to block out a part. Everything else is additive on top of it:

      - drag a note's right edge to set its length (which then becomes the
        length new notes are added with, so a part in eighths is entered by
        resizing once);
      - drag in the velocity lane along the bottom to set a note's velocity,
        which the grid also shows as note brightness;
      - scroll the wheel to move the visible pitch range, ⌘/ctrl-scroll to zoom
        it — the grid is no longer stuck on a fixed two octaves.

    Edits fire onChange with the whole pattern, which the owner snapshots into
    the engine. Drags report only on mouse-up so that dragging a note's length
    is one undo step rather than one per pixel.
*/
class PianoRoll final : public juce::Component
{
public:
    PianoRoll()
    {
        // Needed for keyPressed to ever run: without focus the Delete key
        // goes straight past this pane to the app's, which deletes a track.
        setWantsKeyboardFocus(true);
        seedDemo();
    }

    std::function<void(const engine::Pattern&)> onChange;
    std::function<void(int noteNumber)>         onNotePreview; // fired when a note is *added* by clicking

    const engine::Pattern& pattern() const noexcept { return pattern_; }

    /** Replace the displayed pattern without firing onChange (used for undo/redo).
        The column count follows the pattern's own length, so a longer clip is
        actually editable rather than showing only its first bar — same rule
        DrumStepGrid uses, capped so a very long pattern can't produce
        hairline columns. */
    /** Where the transport is inside this pattern, in beats from its start,
        and whether to show it at all.

        The position is pattern-local because a clip loops: the engine wraps
        playback within the pattern length, so a playhead drawn from the
        song's absolute position would leave the grid on the first repeat and
        never come back. The owner does that wrapping — it is the only thing
        that knows which clip is open and where it sits. */
    void setPlayheadBeats(double patternLocalBeats, bool visible)
    {
        if (std::abs(patternLocalBeats - playheadBeats_) < 1.0e-6 && visible == playheadVisible_)
            return;

        playheadBeats_   = patternLocalBeats;
        playheadVisible_ = visible;
        repaint();
    }

    // Test access: the playhead's x and the grid's bar spacing are the two
    // things that can be silently wrong here — drawn off the grid, or not
    // following the time signature.
    /** Where the playhead line is, in this component's coordinates. The owner
        needs it to keep the viewport following playback. */
    float playheadX() const { return playheadX((float) getWidth()); }

    float playheadXForTesting(float totalWidth) const { return playheadX(totalWidth); }
    int   numStepsForTesting() const { return geometry_.numSteps; }
    float gutterWidthForTesting() const { return geometry_.gutterWidth; }
    double stepBeatsForTesting() const { return geometry_.stepBeats; }
    static constexpr int maxStepsForTesting() { return kMaxSteps; }
    float rowHeightForTesting(float totalHeight) const { return geometry_.rowHeight(totalHeight); }
    double beatsPerBarForTesting() const { return beatsPerBar_; }
    float gridHeightForTesting() const { return gridHeight(); }

    /** Horizontal zoom: how much wider than its viewport the grid draws.

        x1 fits the whole pattern across the pane, which is what it has always
        done. Above that the grid is drawn wider and the viewport scrolls, so
        a long pattern can be worked on at a usable step size — at 256 steps
        in an 800-pixel pane a sixteenth is three pixels across, which is not
        something a note can be placed on.

        The roll draws to whatever width it is given, so this is only ever the
        owner's sum: it decides the bounds, and this says by how much. Kept
        here so the two zooms read the same way and share a control. */
    static constexpr float kMinTimeZoom = 1.0f;  // never narrower than the pane
    static constexpr float kMaxTimeZoom = 16.0f;

    void setTimeZoom(float zoom)
    {
        const float clamped = juce::jlimit(kMinTimeZoom, kMaxTimeZoom, zoom);
        if (std::abs(clamped - timeZoom_) < 1.0e-6f)
            return;

        timeZoom_ = clamped;
        if (onTimeZoomChanged)
            onTimeZoomChanged();
    }

    float timeZoom() const noexcept { return timeZoom_; }

    /** The width this roll wants, given the width available to view it in.
        Below x1 there is nothing to scroll, so it simply fills the pane. */
    int preferredWidth(int viewportWidth) const
    {
        const float gutter = geometry_.gutterWidth;
        const float grid   = juce::jmax(0.0f, (float) viewportWidth - gutter);
        return juce::roundToInt(gutter + grid * timeZoom_);
    }

    /** Fired when the zoom changes from inside, so the owner can resize. */
    std::function<void()> onTimeZoomChanged;

    // The pitch window a fresh roll opens with, and what x1 zoom means.
    static constexpr int kDefaultLowPitch = 48; // C3
    static constexpr int kDefaultNumRows  = 24; // two octaves

    /** Pitch zoom as a multiplier, so it reads the way the timeline's does:
        x1 is the default two-octave window, x2 shows half as many rows twice
        as tall, x0.5 twice as many.

        Expressed as a multiplier rather than as a row count because a row
        count is an implementation detail — "24 rows" says nothing, while
        "x1" is the same thing the tracks view means by it. The row count is
        what's actually stored, so a zoom lands on the nearest achievable
        window and pitchZoom() reports where it landed rather than what was
        asked for. */
    static constexpr float kMinPitchZoom = (float) kDefaultNumRows / (float) PianoRollGeometry::kMaxRows;
    static constexpr float kMaxPitchZoom = (float) kDefaultNumRows / (float) PianoRollGeometry::kMinRows;

    void setPitchZoom(float zoom)
    {
        const float clamped = juce::jlimit(kMinPitchZoom, kMaxPitchZoom, zoom);
        const int   rows    = juce::roundToInt((float) kDefaultNumRows / clamped);

        if (rows == geometry_.numRows)
            return;

        geometry_.setPitchRange(geometry_.lowPitch, rows);
        hoverRow_ = -1;
        repaint();
    }

    float pitchZoom() const noexcept
    {
        return (float) kDefaultNumRows / (float) juce::jmax(1, geometry_.numRows);
    }

    bool canPitchZoomIn() const noexcept  { return geometry_.numRows > PianoRollGeometry::kMinRows; }
    bool canPitchZoomOut() const noexcept { return geometry_.numRows < PianoRollGeometry::kMaxRows; }

    /** Fired when the pitch window changes from inside — the wheel gesture —
        so an owner showing the zoom elsewhere can follow it. */
    std::function<void()> onPitchZoomChanged;

    /** How many beats make a bar, for the grid's heavy lines. */
    void setBeatsPerBar(double beats)
    {
        const double clamped = beats > 0.0 ? beats : 4.0;
        if (std::abs(clamped - beatsPerBar_) < 1.0e-9)
            return;

        beatsPerBar_ = clamped;
        repaint();
    }

    void setPattern(const engine::Pattern& p)
    {
        pattern_ = p;
        const int steps = (int) std::llround(pattern_.lengthBeats / geometry_.stepBeats);
        geometry_.numSteps = juce::jlimit(1, kMaxSteps, steps);
        selection_.clear(); // indices refer to the old pattern; they mean nothing now
        repaint();
    }

    /** Shows a placeholder instead of the grid when there's no clip open to
        edit — the caller falls back to a shared empty Pattern in that case
        (see MainComponent::currentPattern), which this can't tell apart from
        a real clip that's genuinely empty without being told directly. An
        empty grid with nothing to explain it reads as broken, the same
        reasoning as SynthEditor's and FretboardPane's placeholders. */
    void setNoClipSelected(bool none)
    {
        if (noClipSelected_ == none)
            return;
        noClipSelected_ = none;
        repaint();
    }

    /** Which track this clip belongs to, so the header says so — this tab's
        title never changes per track, so without this there was no on-screen
        way to tell which track's notes were actually open after switching
        tracks while parked here. */
    void setTrackInfo(const juce::String& name, juce::uint32 colour)
    {
        trackName_   = name;
        trackColour_ = colour;
        repaint();
    }

    /** Indices into pattern().notes of the current selection, empty if none.
        Callers treat "no selection" as "the whole pattern" (see
        NoteOps::quantizeNotes), so a user who hasn't discovered the selection
        gesture can still quantize. */
    const std::vector<int>& selectedNoteIndices() const noexcept { return selection_; }

    // Test access to the selection, which is otherwise only reachable through
    // shift-clicks and rubber bands.
    void selectAllForTesting()
    {
        selection_.clear();
        for (int i = 0; i < (int) pattern_.notes.size(); ++i)
            selection_.push_back(i);
    }

    void selectForTesting(std::vector<int> indices) { selection_ = std::move(indices); }

    /** Removes the selected notes and reports the new pattern. Returns how
        many went, so a caller can say so. */
    int deleteSelectedNotes()
    {
        if (selection_.empty())
            return 0;

        // Highest index first: erasing from the front would shift every
        // index after it and the rest of the selection would point at the
        // wrong notes — or past the end.
        auto indices = selection_;
        std::sort(indices.begin(), indices.end(), std::greater<int>());

        int removed = 0;
        for (int index : indices)
        {
            if (index >= 0 && index < (int) pattern_.notes.size())
            {
                pattern_.notes.erase(pattern_.notes.begin() + index);
                ++removed;
            }
        }

        selection_.clear();
        repaint();

        if (removed > 0 && onChange)
            onChange(pattern_);

        return removed;
    }

    void clearSelection()
    {
        if (selection_.empty())
            return;
        selection_.clear();
        repaint();
    }

    /** Restores a selection after the pattern has been reloaded by an edit
        that left every note in place — quantize and swing move notes but
        never add, remove or reorder them, so the indices still mean the same
        notes. Out-of-range indices are dropped rather than trusted. */
    void setSelectedNoteIndices(const std::vector<int>& indices)
    {
        selection_.clear();
        for (int index : indices)
            if (index >= 0 && index < (int) pattern_.notes.size())
                selection_.push_back(index);
        repaint();
    }

    /** Switches into drum mode: one row per pad, labelled and pitched by
        @p pads instead of the usual contiguous pitch range — no
        black/white shading or octave lines (neither means anything for
        pads), and no pitch scrolling since the rows *are* the kit. */
    void setDrumPads(const std::vector<model::DrumPad>& pads)
    {
        drumPads_             = pads;
        drumMode_             = true;
        geometry_.numRows     = juce::jmax(1, (int) pads.size());
        hoverRow_             = -1;
        repaint();
    }

    /** Switches back to the usual contiguous-pitch melodic mode. */
    void setMelodicMode()
    {
        if (! drumMode_)
            return;
        drumMode_ = false;
        geometry_.setPitchRange(kDefaultLowPitch, kDefaultNumRows);
        hoverRow_ = -1;
        repaint();
    }

    void clear()
    {
        pattern_.notes.clear();
        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void mouseDown(const juce::MouseEvent& rawEvent) override
    {
        // Rebased so the grid geometry below can keep treating (0,0) as its
        // own top-left, the way it always has — the header lives above that,
        // painted with its own untranslated transform (see paint()).
        const auto e = rawEvent.withNewPosition(rawEvent.position.translated(0.0f, -(float) kTrackHeaderHeight));

        dragMode_      = DragMode::None;
        dragNoteIndex_ = -1;

        if (isInVelocityLane(e.position.y))
        {
            beginVelocityDrag(e);
            return;
        }

        grabKeyboardFocus(); // so Delete goes to the notes, not to the track

        int row = 0, step = 0;
        const bool onGrid = geometry_.cellAt(e.position.x, e.position.y,
                                             (float) getWidth(), gridHeight(), row, step);

        // Shift is the selection modifier throughout: on a note it toggles
        // that note, on empty grid it starts a rubber band. Selecting had to
        // go on a modifier because a plain click already means add-or-remove,
        // which is the fastest way to block a part out and worth keeping.
        if (e.mods.isShiftDown())
        {
            const int existing = onGrid ? noteIndexAtCell(row, step) : -1;
            if (existing >= 0)
            {
                toggleSelected(existing);
                repaint();
            }
            else
            {
                rubberBanding_  = true;
                rubberStart_    = e.position;
                rubberCurrent_  = e.position;
            }
            return;
        }

        clearSelection();

        if (! onGrid)
            return;

        const int noteNumber = pitchForRow(row);
        if (noteNumber < 0)
            return; // a drum-mode row past the end of the pad list (shouldn't happen; defensive)

        const int existing = noteIndexAtCell(row, step);
        if (existing >= 0)
        {
            // Grabbing the right-hand edge resizes rather than deletes — the
            // one place a click on a note doesn't remove it.
            if (isOnResizeEdge(existing, e.position.x))
            {
                dragMode_      = DragMode::ResizeNote;
                dragNoteIndex_ = existing;
                return;
            }

            pattern_.notes.erase(pattern_.notes.begin() + existing);
            repaint();
            if (onChange)
                onChange(pattern_);
            return;
        }

        pattern_.notes.push_back({ step * geometry_.stepBeats, defaultLengthBeats_, noteNumber, defaultVelocity_ });
        if (onNotePreview)
            onNotePreview(noteNumber); // only on add, not on removing an existing note

        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void mouseDrag(const juce::MouseEvent& rawEvent) override
    {
        const auto e = rawEvent.withNewPosition(rawEvent.position.translated(0.0f, -(float) kTrackHeaderHeight));

        if (rubberBanding_)
        {
            rubberCurrent_ = e.position;
            repaint();
            return;
        }

        if (dragNoteIndex_ < 0 || dragNoteIndex_ >= (int) pattern_.notes.size())
            return;

        auto& note = pattern_.notes[(size_t) dragNoteIndex_];

        if (dragMode_ == DragMode::ResizeNote)
        {
            const float colWidth = geometry_.colWidth((float) getWidth());
            if (colWidth <= 0.0f)
                return;

            // Length snaps to whole steps, never goes below one (so a note
            // can't be dragged out of existence), and can't run past the end
            // of the pattern it lives in.
            const int   start    = stepOf(note);
            const int   maxSteps = juce::jmax(1, geometry_.numSteps - start);
            const float startX   = geometry_.xForStep(start, (float) getWidth());
            const int   steps    = juce::jlimit(1, maxSteps,
                                                (int) std::lround((e.position.x - startX) / colWidth));
            note.lengthBeats     = steps * geometry_.stepBeats;
            defaultLengthBeats_ = note.lengthBeats; // new notes inherit the length you just chose
            repaint();
        }
        else if (dragMode_ == DragMode::Velocity)
        {
            note.velocity    = velocityForY(e.position.y);
            defaultVelocity_ = note.velocity;
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (rubberBanding_)
        {
            selectNotesIn(juce::Rectangle<float>(rubberStart_, rubberCurrent_));
            rubberBanding_ = false;
            repaint();
            return; // selecting changes nothing about the pattern itself
        }

        // Drags report once, on release: resizing a note across ten pixels
        // should be one undo step, not ten.
        if (dragMode_ != DragMode::None && onChange)
            onChange(pattern_);

        dragMode_      = DragMode::None;
        dragNoteIndex_ = -1;
    }

    void mouseMove(const juce::MouseEvent& rawEvent) override
    {
        const auto e = rawEvent.withNewPosition(rawEvent.position.translated(0.0f, -(float) kTrackHeaderHeight));

        int row = 0, step = 0;
        const int newHoverRow = geometry_.cellAt(e.position.x, e.position.y,
                                                  (float) getWidth(), gridHeight(), row, step)
                                     ? row : -1;
        if (newHoverRow != hoverRow_)
        {
            hoverRow_ = newHoverRow;
            repaint();
        }

        // A resize cursor is the only hint that the edge is grabbable.
        const int overNote = newHoverRow >= 0 ? noteIndexAtCell(row, step) : -1;
        setMouseCursor(overNote >= 0 && isOnResizeEdge(overNote, e.position.x)
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::NormalCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverRow_ != -1)
        {
            hoverRow_ = -1;
            repaint();
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    /** Delete removes the selected notes.

        Consumed whenever this pane has focus, even with nothing selected. It
        would otherwise fall through to the app's Delete, which removes the
        whole selected *track* — so editing notes and reaching for Delete
        would destroy the part being edited. A key that means "delete a note"
        in a note editor must not sometimes mean "delete everything". */
    bool keyPressed(const juce::KeyPress& key) override
    {
        // M toggles palm muting on the selection - see togglePalmMuteOnSelection.
        if (key.getModifiers().getRawFlags() == 0 && key.getTextCharacter() == 'm')
            return togglePalmMuteOnSelection();

        if (! key.isKeyCode(juce::KeyPress::deleteKey)
            && ! key.isKeyCode(juce::KeyPress::backspaceKey))
            return false;

        if (key.getModifiers().getRawFlags() != 0)
            return false; // cmd+backspace is Delete Clip; leave it to the owner

        const int removed = deleteSelectedNotes();
        if (onNotesDeleted)
            onNotesDeleted(removed);

        return true;
    }

    /**
        Flips the selected notes between open and palm-muted.

        All-or-nothing rather than per note: if any selected note is open they
        all become muted, otherwise they all open up. Toggling each
        independently would make a mixed selection scramble rather than change,
        and "make these chug" is what the keystroke means.

        Reported through onChange like every other edit, so it lands in undo as
        one step. Returns false with nothing selected, which leaves the key
        unconsumed rather than silently doing nothing to everything.
    */
    bool togglePalmMuteOnSelection()
    {
        if (selection_.empty())
            return false;

        bool anyOpen = false;
        for (int index : selection_)
            if (juce::isPositiveAndBelow(index, (int) pattern_.notes.size())
                && pattern_.notes[(size_t) index].articulation
                                          == engine::Articulation::Normal)
                anyOpen = true;

        const auto wanted = anyOpen ? engine::Articulation::PalmMute
                                    : engine::Articulation::Normal;

        bool changed = false;
        for (int index : selection_)
        {
            if (! juce::isPositiveAndBelow(index, (int) pattern_.notes.size()))
                continue;

            auto& note = pattern_.notes[(size_t) index];
            if (note.articulation != wanted)
            {
                note.articulation = wanted;
                changed = true;
            }
        }

        if (changed)
        {
            repaint();
            if (onChange)
                onChange(pattern_);
        }

        return true;
    }

    /** Fired after a Delete keystroke, with how many notes went — zero
        included, so the owner can say why nothing happened. */
    std::function<void(int numNotesDeleted)> onNotesDeleted;

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (std::abs(wheel.deltaY) < 1.0e-4f)
            return;

        const bool up = wheel.deltaY > 0.0f;

        // Shift-scroll zooms time, matching cmd-scroll for pitch. Checked
        // before the drum-mode guard below, because time zoom means the same
        // thing for a kit pattern as for a melodic one — it's only the
        // *pitch* axis that a kit doesn't have.
        if (e.mods.isShiftDown())
        {
            setTimeZoom(timeZoom_ * (up ? 1.25f : 0.8f));
            return;
        }

        // Drum mode's rows are the kit's pads, not a pitch range, so there is
        // nothing to scroll or zoom vertically.
        if (drumMode_)
            return;

        const bool zooming = e.mods.isCommandDown() || e.mods.isCtrlDown();
        if (zooming)
            geometry_.zoomBy(up ? -2 : 2); // fewer rows = taller rows = zoomed in
        else
            geometry_.scrollPitchBy(up ? 1 : -1);

        hoverRow_ = -1;
        repaint();

        // The wheel and the zoom control set the same thing, so whichever is
        // used the other has to follow — otherwise the readout drifts from
        // what's on screen.
        if (zooming && onPitchZoomChanged)
            onPitchZoomChanged();
    }

    void paint(juce::Graphics& g) override
    {
        if (noClipSelected_)
        {
            g.fillAll(juce::Colour(0xff1e1e22));
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText("Select a track and clip to edit its notes",
                       getLocalBounds(), juce::Justification::centred);
            return;
        }

        paintTrackHeader(g, getLocalBounds().removeFromTop(kTrackHeaderHeight),
                         trackName_, trackColour_, drumMode_ ? model::TrackType::Drum : model::TrackType::Instrument);

        // Everything below is drawn as though the grid started at (0,0), the
        // way it always has — a transform, not a rewrite of every y in this
        // function (and in the mouse handlers, which subtract the same
        // offset from incoming positions instead — see mouseDown et al.).
        juce::Graphics::ScopedSaveState pushHeaderOffset(g);
        g.addTransform(juce::AffineTransform::translation(0.0f, (float) kTrackHeaderHeight));

        const float w  = (float) getWidth();
        const float h  = gridHeight();
        const float cw = geometry_.colWidth(w);
        const float ch = geometry_.rowHeight(h);
        const float gx = geometry_.gutterWidth;

        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.fillRect(getLocalBounds());

        g.setFont(juce::FontOptions(11.0f));
        for (int r = 0; r < geometry_.numRows; ++r)
        {
            const bool  black    = ! drumMode_ && engine::isBlackKey(pitchForRow(r));
            const float y        = geometry_.yForRow(r, h);
            const bool  hovered  = (r == hoverRow_);

            // Gutter cell: the row's pitch (melodic) or pad (drum) name.
            g.setColour(black ? juce::Colour(0xff222226) : juce::Colour(0xff35353a));
            g.fillRect(juce::Rectangle<float>(0.0f, y, gx, ch));
            g.setColour(juce::Colours::white.withAlpha(black ? 0.55f : 0.85f));
            g.drawText(labelForRow(r), 4, (int) y, (int) gx - 6, (int) ch,
                       juce::Justification::centredLeft);

            // Grid lane for this row.
            g.setColour(hovered ? juce::Colours::white.withAlpha(0.10f)
                                : (black ? juce::Colours::black.withAlpha(0.28f)
                                         : juce::Colours::white.withAlpha(0.05f)));
            g.fillRect(juce::Rectangle<float>(gx, y, w - gx, ch));
        }

        // Three weights, not two: the bar line has to be findable at a glance
        // or a long pattern is an undifferentiated field of beats. Which
        // steps are bars follows the time signature rather than assuming
        // four.
        const int stepsPerBeat = juce::jmax(1, (int) std::llround(1.0 / geometry_.stepBeats));
        const int stepsPerBar  = juce::jmax(1, (int) std::llround(beatsPerBar_ / geometry_.stepBeats));

        for (int s = 0; s <= geometry_.numSteps; ++s)
        {
            const bool bar  = (s % stepsPerBar) == 0;
            const bool beat = (s % stepsPerBeat) == 0;

            g.setColour(juce::Colours::white.withAlpha(bar ? 0.38f : (beat ? 0.20f : 0.07f)));
            g.fillRect(geometry_.xForStep(s, w), 0.0f, bar ? 2.0f : 1.0f, h);
        }

        // Row separators — melodic mode gets a heavier line at each octave
        // boundary (every 12 rows); drum mode has no such concept, just
        // plain separators between the handful of pads.
        for (int r = 0; r <= geometry_.numRows; ++r)
        {
            const bool heavy = ! drumMode_ && (pitchForRow(std::min(r, geometry_.numRows - 1)) % 12) == 0;
            g.setColour(juce::Colours::white.withAlpha(heavy ? 0.18f : 0.06f));
            g.fillRect(0.0f, geometry_.yForRow(r, h), w, heavy ? 2.0f : 1.0f);
        }

        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(gx - 1.0f, 0.0f, 1.0f, h);

        paintPlayhead(g, w, h);

        for (size_t i = 0; i < pattern_.notes.size(); ++i)
        {
            const auto& n    = pattern_.notes[i];
            const int   step = stepOf(n);
            const int   row  = rowForPitch(n.noteNumber);
            if (row < 0 || row >= geometry_.numRows || step < 0 || step >= geometry_.numSteps)
                continue; // outside the visible pitch window or past the pattern's end

            // Brightness carries velocity, so the grid alone tells you which
            // hits are accented without reading the lane below.
            const juce::Rectangle<float> block(geometry_.xForStep(step, w) + 1.0f,
                                               geometry_.yForRow(row, h) + 1.0f,
                                               (float) spanOf(n) * cw - 2.0f, ch - 2.0f);
            const bool palmMuted = n.articulation == engine::Articulation::PalmMute;

            // A muted note is a different *colour*, not a dimmer green: green's
            // brightness already carries velocity, so shading it would make a
            // quiet open note and a loud chug look the same. Orange reads as a
            // different kind of note at a glance, which is the whole reason
            // this is a visible flag rather than a velocity trick.
            g.setColour((palmMuted ? juce::Colours::orange : juce::Colours::limegreen)
                            .withAlpha(0.4f + 0.6f * juce::jlimit(0.0f, 1.0f, n.velocity)));
            g.fillRect(block);

            if (palmMuted)
            {
                // A bar across the note, so the two are still distinguishable
                // without relying on colour alone.
                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.fillRect(block.withSizeKeepingCentre(block.getWidth() * 0.7f, 2.0f));
            }

            if (isSelected((int) i))
            {
                g.setColour(juce::Colours::cyan.withAlpha(0.95f));
                g.drawRect(block, 2.0f);
            }
        }

        if (rubberBanding_)
        {
            const juce::Rectangle<float> band(rubberStart_, rubberCurrent_);
            g.setColour(juce::Colours::cyan.withAlpha(0.12f));
            g.fillRect(band);
            g.setColour(juce::Colours::cyan.withAlpha(0.6f));
            g.drawRect(band, 1.0f);
        }

        paintVelocityLane(g, w, cw, gx);
    }

private:
    /** The vertical line showing where playback is inside the pattern. Drawn
        last so it sits over the notes: it is a readout, and a readout behind
        the data it refers to is worse than none. */
    /** The pattern's length in beats, falling back to the grid's extent for a
        pattern that never had one set. */
    double patternLengthBeats() const
    {
        return pattern_.lengthBeats > 0.0 ? pattern_.lengthBeats
                                          : geometry_.numSteps * geometry_.stepBeats;
    }

    /** Where the playhead line sits for a given component width. Clamped to
        the grid: a position past the pattern's end would otherwise draw
        outside it, and a line floating beyond the last step reads as a
        rendering fault rather than as a position. */
    float playheadX(float totalWidth) const
    {
        // The pattern's own length, not the grid's. They agree unless the
        // grid has been capped, and in that case the pattern is the truth —
        // scaling against a truncated grid would put the line at the wrong
        // position for the whole clip rather than only past the cap.
        const double patternBeats = patternLengthBeats();
        if (patternBeats <= 0.0)
            return geometry_.gutterWidth;

        const double position = juce::jlimit(0.0, patternBeats, playheadBeats_);
        return geometry_.gutterWidth
             + (float) (position / patternBeats) * geometry_.gridWidth(totalWidth);
    }

    void paintPlayhead(juce::Graphics& g, float w, float h) const
    {
        if (! playheadVisible_ || patternLengthBeats() <= 0.0)
            return;

        g.setColour(juce::Colours::orange.withAlpha(0.9f));
        g.fillRect(playheadX(w), 0.0f, 2.0f, h);
    }

    enum class DragMode { None, ResizeNote, Velocity };

    // A ceiling on the grid, not a statement about bars. It used to be 64,
    // commented "4 bars of 16ths", which is only 4 bars in 4/4: four bars of
    // 5/4 is 80 sixteenths and of 12/8 is 96, so a pattern that long was
    // silently truncated — the tail invisible in the editor and unreachable,
    // while still playing. 256 is sixteen bars of 4/4 and covers every meter
    // and length the app offers, with room to spare; it exists only so an
    // absurd pattern length can't ask for a million columns.
    static constexpr int   kMaxSteps           = 256;
    static constexpr float kVelocityLaneHeight = 46.0f;
    static constexpr float kResizeEdgePixels   = 6.0f;

    float gridHeight() const
    {
        return juce::jmax(1.0f, (float) getHeight() - (float) kTrackHeaderHeight - kVelocityLaneHeight);
    }
    float velocityLaneTop() const { return gridHeight(); }
    bool  isInVelocityLane(float y) const { return y >= velocityLaneTop(); }

    bool isSelected(int index) const
    {
        return std::find(selection_.begin(), selection_.end(), index) != selection_.end();
    }

    void toggleSelected(int index)
    {
        const auto it = std::find(selection_.begin(), selection_.end(), index);
        if (it != selection_.end())
            selection_.erase(it);
        else
            selection_.push_back(index);
    }

    /** Replaces the selection with every note whose block overlaps @p area
        (in this component's coordinates). */
    void selectNotesIn(juce::Rectangle<float> area)
    {
        selection_.clear();

        const float w  = (float) getWidth();
        const float h  = gridHeight();
        const float cw = geometry_.colWidth(w);
        const float ch = geometry_.rowHeight(h);

        for (size_t i = 0; i < pattern_.notes.size(); ++i)
        {
            const auto& n    = pattern_.notes[i];
            const int   step = stepOf(n);
            const int   row  = rowForPitch(n.noteNumber);
            if (row < 0 || step < 0 || step >= geometry_.numSteps)
                continue;

            const juce::Rectangle<float> block(geometry_.xForStep(step, w), geometry_.yForRow(row, h),
                                               (float) spanOf(n) * cw, ch);
            if (area.intersects(block))
                selection_.push_back((int) i);
        }
    }

    int stepOf(const engine::Note& n) const { return (int) std::llround(n.startBeats / geometry_.stepBeats); }
    int spanOf(const engine::Note& n) const
    {
        return juce::jmax(1, (int) std::llround(n.lengthBeats / geometry_.stepBeats));
    }

    /** The note occupying (row, step), or -1 — a note covers every step from
        its start for as long as it lasts, so clicking anywhere along a long
        note finds it. */
    int noteIndexAtCell(int row, int step) const
    {
        const int pitch = pitchForRow(row);
        if (pitch < 0)
            return -1;

        for (size_t i = 0; i < pattern_.notes.size(); ++i)
        {
            const auto& n = pattern_.notes[i];
            if (n.noteNumber != pitch)
                continue;
            const int start = stepOf(n);
            if (step >= start && step < start + spanOf(n))
                return (int) i;
        }
        return -1;
    }

    bool isOnResizeEdge(int noteIndex, float x) const
    {
        if (noteIndex < 0 || noteIndex >= (int) pattern_.notes.size())
            return false;
        const auto& n     = pattern_.notes[(size_t) noteIndex];
        const float right = geometry_.xForStep(stepOf(n) + spanOf(n), (float) getWidth());
        return x >= right - kResizeEdgePixels && x <= right;
    }

    /** The note whose velocity bar sits under @p x in the lane: the one
        *starting* at that step (the bar is drawn at a note's start), highest
        pitch first so a chord's topmost bar is the one you grab. */
    int noteIndexForVelocityAt(float x) const
    {
        const float colWidth = geometry_.colWidth((float) getWidth());
        if (colWidth <= 0.0f || x < geometry_.gutterWidth)
            return -1;

        const int step  = (int) ((x - geometry_.gutterWidth) / colWidth);
        int       best  = -1;
        int       bestPitch = -1;

        for (size_t i = 0; i < pattern_.notes.size(); ++i)
        {
            const auto& n = pattern_.notes[i];
            if (stepOf(n) != step || rowForPitch(n.noteNumber) < 0)
                continue;
            if (n.noteNumber > bestPitch)
            {
                bestPitch = n.noteNumber;
                best      = (int) i;
            }
        }
        return best;
    }

    void beginVelocityDrag(const juce::MouseEvent& e)
    {
        const int index = noteIndexForVelocityAt(e.position.x);
        if (index < 0)
            return;

        dragMode_      = DragMode::Velocity;
        dragNoteIndex_ = index;
        pattern_.notes[(size_t) index].velocity = velocityForY(e.position.y);
        defaultVelocity_ = pattern_.notes[(size_t) index].velocity;
        repaint();
    }

    /** Lane position -> velocity, floored just above zero: a note at velocity
        0 is silent but still drawn, which looks like a bug. */
    float velocityForY(float y) const
    {
        const float top  = velocityLaneTop();
        const float span = juce::jmax(1.0f, (float) getHeight() - top);
        return juce::jlimit(0.05f, 1.0f, 1.0f - (y - top) / span);
    }

    void paintVelocityLane(juce::Graphics& g, float w, float colWidth, float gutter)
    {
        const float top    = velocityLaneTop();
        const float height = (float) getHeight() - top;
        if (height <= 0.0f)
            return;

        g.setColour(juce::Colour(0xff1a1a1e));
        g.fillRect(juce::Rectangle<float>(0.0f, top, w, height));
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(0.0f, top, w, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("Vel", 4, (int) top, (int) gutter - 6, (int) height, juce::Justification::centredLeft);

        for (const auto& n : pattern_.notes)
        {
            const int step = stepOf(n);
            if (step < 0 || step >= geometry_.numSteps || rowForPitch(n.noteNumber) < 0)
                continue; // hidden by the current pitch window, so no bar either

            const float velocity = juce::jlimit(0.0f, 1.0f, n.velocity);
            const float barH     = juce::jmax(1.0f, velocity * (height - 4.0f));
            const float x        = geometry_.xForStep(step, w);

            g.setColour(juce::Colours::limegreen.withAlpha(0.35f + 0.65f * velocity));
            g.fillRect(juce::Rectangle<float>(x + 2.0f, top + height - 2.0f - barH,
                                              juce::jmax(2.0f, colWidth - 4.0f), barH));
        }
    }

    int pitchForRow(int row) const
    {
        if (! drumMode_)
            return geometry_.pitchForRow(row);
        return (row >= 0 && row < (int) drumPads_.size()) ? drumPads_[(size_t) row].noteNumber : -1;
    }

    int rowForPitch(int pitch) const
    {
        if (! drumMode_)
        {
            const int row = geometry_.rowForPitch(pitch);
            return (row >= 0 && row < geometry_.numRows) ? row : -1; // outside the visible window
        }
        for (size_t i = 0; i < drumPads_.size(); ++i)
            if (drumPads_[i].noteNumber == pitch)
                return (int) i;
        return -1;
    }

    juce::String labelForRow(int row) const
    {
        if (! drumMode_)
            return engine::midiNoteName(pitchForRow(row));
        return (row >= 0 && row < (int) drumPads_.size()) ? juce::String(drumPads_[(size_t) row].label)
                                                          : juce::String();
    }

    void seedDemo()
    {
        pattern_.lengthBeats = geometry_.numSteps * geometry_.stepBeats; // 4 beats = 1 bar
        const int root   = 60;                          // C4
        const int arp[]  = { 0, 4, 7, 12 };             // C E G C
        for (int i = 0; i < 4; ++i)
            pattern_.notes.push_back({ (double) i, 0.5, root + arp[i], 0.8f });
    }

    PianoRollGeometry           geometry_;
    bool                        noClipSelected_ = false;
    juce::String                trackName_;
    juce::uint32                trackColour_ = 0;

    // Transport readout: where playback is inside this pattern, and whether
    // anything is playing. beatsPerBar_ drives the grid's bar lines.
    double playheadBeats_   = 0.0;
    bool   playheadVisible_ = false;
    double beatsPerBar_     = 4.0;
    float  timeZoom_        = 1.0f;
    engine::Pattern             pattern_;
    int                         hoverRow_ = -1;
    bool                        drumMode_ = false;
    std::vector<model::DrumPad> drumPads_;

    std::vector<int>   selection_;     // indices into pattern_.notes
    bool               rubberBanding_ = false;
    juce::Point<float> rubberStart_, rubberCurrent_;

    DragMode dragMode_      = DragMode::None;
    int      dragNoteIndex_ = -1;
    double   defaultLengthBeats_ = 0.25; // one step, until a resize changes it
    float    defaultVelocity_    = 0.8f;
};

} // namespace looper
