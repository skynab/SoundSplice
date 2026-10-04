#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"
#include "model/Song.h"

#include "AudioFileTypes.h"
#include "ClipPreview.h"
#include "Icons.h"
#include "ClipWindow.h"
#include "EnvelopeGeometry.h"
#include "SnapTargets.h"
#include "TimeFormat.h"
#include "model/Folders.h"
#include "model/Markers.h"
#include "model/RazorEdits.h"
#include "model/Takes.h"
#include "model/TempoChanges.h"
#include "model/Warp.h"
#include "model/TimeSelection.h"
#include "SpectrogramCache.h"
#include "TakeLanes.h"
#include "WaveformCache.h"
#include "TrackColours.h"
#include "TimelineGeometry.h"

namespace soundsplice
{
/**
    A bars/beats ruler, one lane per track with its clips drawn as blocks, and a
    playhead that sweeps during playback. Reads a snapshot of the Song; the owner
    refreshes it on document changes and feeds the playhead position from the
    transport on a timer.

    Sizes itself to the full content (song length x number of tracks, scaled by
    zoom) rather than the viewport — the owner wraps it in a juce::Viewport so
    long or heavily-zoomed timelines scroll instead of squeezing. Click empty
    ruler/lane space to seek the transport there; drag a clip to move where it
    starts (the engine now delays a track's pattern until its clip's start beat,
    so this is a real scheduling change, not just cosmetic — see Sequencer's
    clip-start gating).

    Accepts files from two different places, which need two different JUCE
    interfaces — and only having the first is why dragging a file in from
    Finder used to do nothing at all:

      - juce::DragAndDropTarget handles drags that start *inside* the app
        (the FileBrowserPanel's tree).
      - juce::FileDragAndDropTarget handles drags from the operating system.

    Both end at the same onFileDropped, so a file behaves identically however
    it arrived.

    Originally only a juce::DragAndDropTarget for files dragged out of a FileBrowserPanel
    (identified by sourceComponent being a juce::FileTreeComponent, not by the
    drag's description string — DockRegion uses that string for its own
    panel-regrouping drags, so type is the unambiguous signal). Dropping a
    file fires onFileDropped with the beat under the drop point and the track
    lane it landed on (-1 if it landed outside every lane); the owner adds it
    to that track if it's an audio track, or otherwise creates a new one.
*/
class ArrangementView final : public juce::Component,
                              public juce::DragAndDropTarget,
                              public juce::FileDragAndDropTarget,
                              public juce::TooltipClient
{
public:
    ArrangementView()
        : unmutedIcon_(icons::fromSvg(icons::kAudioOn)),
          mutedIcon_(icons::fromSvg(icons::kAudioDisabled)),
          gearIcon_(icons::fromSvg(icons::kGear))
    {
        // Scanning a file is asynchronous, so a clip is blank for the first
        // frames after it's added. Without this it would stay blank until
        // something else happened to invalidate the view.
        waveforms_.onUpdated = [this] { repaint(); };
        spectrograms_.onUpdated = [this] { repaint(); };

        // The gutter carries a colour stripe, a type tag, the name, and two
        // buttons. 110px fitted a name alone.
        geometry_.gutterWidth = 168.0f;
    }

    std::function<void(double)> onSeek; // beat position clicked

    /** Fired when a drag along the ruler starts moving, and when it's let go:
        the owner plays while the ruler is scrubbed, so what's under the mouse
        can be heard. A click without a drag fires neither. */
    std::function<void()> onScrubStarted;
    std::function<void()> onScrubEnded;
    std::function<void(int trackIndex, int clipIndex, double newStartBeats)> onClipMoved;
    std::function<void(int trackIndex, int clipIndex, double newLengthBeats)> onClipResized;

    /** Fired when an audio clip's left edge is dragged: the clip should start
        at @p newStartBeats with its audio left where it was (see
        trimClipStart, which the drag preview already clamps through). */
    std::function<void(int trackIndex, int clipIndex, double newStartBeats)> onClipStartTrimmed;

    /** Fired when an audio clip's contents are Ctrl-dragged (Cmd on a Mac)
        inside its edges: the clip should play its file from
        @p newOffsetSeconds, staying where it is (see slipClip, which the drag
        preview already clamps through). */
    std::function<void(int trackIndex, int clipIndex, double newOffsetSeconds)> onClipSlipped;

    /** Fired when a fade handle on an audio clip is dragged and released,
        with the clip's fades as they should now be. */
    std::function<void(int trackIndex, int clipIndex, const engine::ClipFades& fades)> onClipFadesChanged;

    /** Fired on a right-click on a clip, after onClipSelected. The owner
        shows the menu: it knows what the options do, and the view doesn't. */
    std::function<void(int trackIndex, int clipIndex)> onClipMenuRequested;

    /** Fired on a right-click on a marker in the ruler, and on a double-click
        on one. The owner shows the menu or asks for the new name. */
    std::function<void(int markerId)> onMarkerMenuRequested;
    std::function<void(int markerId)> onMarkerRenameRequested;

    /** Fired when a marker is dragged along the ruler and released somewhere
        new, with where it should now start. */
    std::function<void(int markerId, double newStartBeats)> onMarkerMoved;

    /** Tempo changes, along the bottom of the ruler: right-click to add one
        (or edit, ramp or remove the one there), drag one to move it. The
        view only says what was asked; the owner makes the edit. */
    std::function<void(double beat)>                  onTempoChangeRequested;
    std::function<void(double beat)>                  onTempoChangeRemoved;
    std::function<void(double beat)>                  onTempoRampToggled;
    std::function<void(double fromBeat, double toBeat)> onTempoChangeMoved;

    /** Fired when a clip's volume curve is edited (with curves shown): a
        point added, dragged or removed, with the curve as it should now be. */
    std::function<void(int trackIndex, int clipIndex, const engine::ClipEnvelope& envelope)> onClipEnvelopeChanged;

    /** Fired instead of onClipMoved when a move-drag ends on a *different*
        track than it started on (see typesAreCompatibleForClipMove — the
        drag preview never lands on an incompatible track in the first
        place, so this always names a real, allowed move). Fires even if the
        beat position didn't also change: changing track is itself an edit. */
    std::function<void(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats)> onClipMovedToTrack;
    std::function<void(int trackIndex, int clipIndex)> onClipSelected; // fired on press, before any drag

    /** Take lanes (setShowTakeLanes): a click on take @p take's row of a clip,
        to play that take over the clip, and a drag along one, to comp the
        stretch [@p fromBeats, @p toBeats) of the track to it. */
    std::function<void(int trackIndex, int clipIndex, int take)> onTakeChosen;
    std::function<void(int trackIndex, int take, double fromBeats, double toBeats)> onTakeSwiped;

    /** Fired when a track's mute button in the gutter is clicked. The view
        doesn't change the document itself — the owner does, and the change
        comes back through setSong. */
    std::function<void(int trackIndex)> onTrackMuteToggled;

    /** Fired when a track's gear button is clicked. The owner shows the menu:
        it knows what the options do, and the view doesn't. */
    std::function<void(int trackIndex)> onTrackSettingsRequested;

    /** Fired by a click on a folder track's triangle: collapse or expand it. */
    std::function<void(int trackIndex)> onFolderToggled;

    /** Fired when a track's header is alt-dragged: duplicate that track. */
    std::function<void(int trackIndex)> onTrackDuplicateRequested;
    std::function<void(const juce::File& file, double dropBeat, int trackIndex)> onFileDropped;

    /** Fired as a time selection is dragged out across the lanes (or Shift-
        dragged, which starts one over clips too), and when a click on a clip
        clears it. A click on an empty lane is a selection with no length: a
        cursor on that track. */
    std::function<void(const model::TimeSelection&)> onTimeSelectionChanged;

    /** Razor areas (model/RazorEdits.h): fired as Ctrl+Shift-drags (Cmd on
        a Mac) add them, and when a click elsewhere clears them. A drag
        starting inside one asks for them to be moved, by whole lanes and by
        beats, on release. */
    std::function<void(const model::RazorAreas&)> onRazorAreasChanged;
    std::function<void(double deltaBeats, int deltaTracks)> onRazorMoveRequested;

    void setSong(const model::Song& song)
    {
        song_  = song;
        clock_ = song_.bpm > 0.0 ? model::clockFor(song_) : model::BeatClock(120.0);
        rebuildRows();

        // Thumbnails are made here rather than in paint: creating one starts a
        // file scan and allocates, neither of which belongs on a path that
        // runs for every scroll and playhead tick.
        for (const auto& track : song_.tracks)
            for (const auto& clip : track.clips)
            {
                if (clip.type == model::ClipType::Audio && ! clip.audioFile.empty())
                    waveforms_.ensure(juce::File(clip.audioFile));

                // Every take's, for the take lanes.
                for (const auto& take : clip.takes)
                    if (! take.audioFile.empty())
                        waveforms_.ensure(juce::File(take.audioFile));
            }
        ensureSpectrograms();

        updateContentSize();
        repaint();
    }

    void setPlayheadBeats(double beats)
    {
        if (std::abs(beats - playheadBeats_) > 1.0e-6)
        {
            playheadBeats_ = beats;
            repaint();
        }
    }

    // The range the timeline will zoom over. Named, and readable, so the
    // owner can tell when a zoom button would do nothing rather than
    // duplicating the numbers and drifting from them.
    static constexpr float kMinZoom = 0.25f;
    static constexpr float kMaxZoom = 4.0f;

    void setZoom(float zoom)
    {
        geometry_.zoom = juce::jlimit(kMinZoom, kMaxZoom, zoom);
        updateContentSize();
        repaint();
    }

    float zoom() const noexcept { return geometry_.zoom; }

    /** The timeline's geometry at the current zoom, for fitting a span of it
        to the view (see app/TimelineZoom.h). */
    const TimelineGeometry& geometry() const noexcept { return geometry_; }

    /** Where the last clip ends, in beats: what Fit Project fits. */
    double arrangedEndBeats() const { return contentEndBeats(); }

    /** The range a lane's height can be set to: tall enough for the gutter's
        buttons, and not so tall that one track is a whole screen. */
    static constexpr float kMinLaneHeight = 28.0f;
    static constexpr float kMaxLaneHeight = 160.0f;

    /** How tall each track's lane is (Fit Vertically sets it). */
    void setLaneHeight(float height)
    {
        const float clamped = juce::jlimit(kMinLaneHeight, kMaxLaneHeight, height);
        if (std::abs(clamped - geometry_.laneHeight) < 0.01f)
            return;

        geometry_.laneHeight = clamped;
        updateContentSize();
        repaint();
    }

    float laneHeight() const noexcept { return geometry_.laneHeight; }

    /** Which beat sits under a given x. Exposed for the GUI tests, which is
        the only way to assert that zooming actually changed the mapping
        rather than merely storing a number. */
    double beatForXForTesting(float x) const { return geometry_.beatForX(x); }

    bool canZoomIn() const noexcept  { return geometry_.zoom < kMaxZoom; }
    bool canZoomOut() const noexcept { return geometry_.zoom > kMinZoom; }

    /** Highlights the clip currently open in the piano roll. */
    /** Whether dragging and resizing clips snaps to whole beats. Alt still
        inverts it for a single drag — see mouseDrag. */
    void setSnapToGrid(bool shouldSnap) { snapToGrid_ = shouldSnap; }
    bool snapsToGrid() const            { return snapToGrid_; }

    /** Whether dragged clip edges are pulled onto nearby markers and the
        playhead, and onto other clips' edges. These win over the grid when
        one is close enough; Alt turns all snapping off for a single drag. */
    void setSnapToMarkers(bool shouldSnap)   { snapToMarkers_ = shouldSnap; }
    bool snapsToMarkers() const              { return snapToMarkers_; }
    void setSnapToClipEdges(bool shouldSnap) { snapToClipEdges_ = shouldSnap; }
    bool snapsToClipEdges() const            { return snapToClipEdges_; }

    /** Whether audio clips show their volume curves, and clicks on them edit
        the curve rather than moving the clip: click to add a point, drag to
        move one, Alt-click to remove one. */
    /** Draws audio clips as spectrograms rather than waveforms (Audacity's
        spectrogram track view), each file analysed in the background. */
    void setShowSpectrograms(bool show)
    {
        if (show == showSpectrograms_)
            return;
        showSpectrograms_ = show;
        ensureSpectrograms();
        repaint();
    }

    bool showsSpectrograms() const noexcept { return showSpectrograms_; }

    /** The scale and colouring the audio editor's spectrogram uses, so the
        two views of a clip agree. */
    void setSpectrogramStyle(spectrogramimage::Scale scale, spectrogramimage::Display display)
    {
        spectrograms_.setStyle(scale, display);
        repaint();
    }

    const SpectrogramCache& spectrogramCache() const noexcept { return spectrograms_; }

    void setShowEnvelopes(bool show)
    {
        if (show == showEnvelopes_)
            return;

        showEnvelopes_   = show;
        envelopeEditing_ = false;
        repaint();
    }

    bool showsEnvelopes() const { return showEnvelopes_; }

    /** Whether a clip with takes draws each in a row of its own under a
        header band (app/TakeLanes.h): click a row to play that take, drag
        along one to comp to it, drag the header to move the clip. */
    void setShowTakeLanes(bool show)
    {
        if (show == showTakeLanes_)
            return;

        showTakeLanes_ = show;
        swiping_       = false;
        repaint();
    }

    bool showsTakeLanes() const noexcept { return showTakeLanes_; }

    /** How the ruler and grid count time (bars and beats, or a clock, sample
        or timecode grid), which is also what clips snap to. */
    void setTimeDisplay(const app::TimeDisplay& display)
    {
        if (display == timeDisplay_)
            return;

        timeDisplay_ = display;
        repaint();
    }

    const app::TimeDisplay& timeDisplay() const { return timeDisplay_; }

    void setSelectedClip(int trackIndex, int clipIndex)
    {
        if (selectedTrackForEdit_ != trackIndex || selectedClipForEdit_ != clipIndex)
        {
            selectedTrackForEdit_ = trackIndex;
            selectedClipForEdit_  = clipIndex;
            repaint();
        }
    }

    /** Shows @p selection, as set by the owner (after an edit moves it, say). */
    void setTimeSelection(const model::TimeSelection& selection)
    {
        if (selection == timeSelection_)
            return;

        timeSelection_ = selection;
        repaint();
    }

    const model::TimeSelection& timeSelection() const { return timeSelection_; }

    /** Shows @p areas, as set by the owner (after a move or a paste). */
    void setRazorAreas(const model::RazorAreas& areas)
    {
        if (areas == razorAreas_)
            return;

        razorAreas_ = areas;
        repaint();
    }

    const model::RazorAreas& razorAreas() const { return razorAreas_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(theme::surface(*this, theme::paneId));

        const float  ppb       = geometry_.pixelsPerBeat();
        const double qpb       = quartersPerBar();
        const auto   width     = (float) getWidth();
        const auto   height    = (float) getHeight();
        const float  timelineX = geometry_.gutterWidth;
        const int    numBars   = (int) std::ceil(totalBeats() / qpb);

        // Ruler + bar lines.
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(0.0f, 0.0f, width, geometry_.rulerHeight);
        g.setFont(juce::FontOptions(12.0f));
        if (timeDisplay_.format != app::TimeFormat::BarsBeats)
        {
            paintSecondsGrid(g, height);
        }
        else
        {
            // Beat lines inside each bar, so a bar reads as its beats rather
            // than as one undivided box — in 4/4 that is four subdivisions per
            // bar, and it follows the time signature rather than assuming
            // four. Dropped when they'd be closer together than this, since a
            // grid too fine to resolve is just a lighter background.
            const float beatSpacing = ppb;
            if (beatSpacing >= kMinGridSpacing)
            {
                g.setColour(juce::Colours::white.withAlpha(0.07f));
                const int totalBeatLines = (int) std::ceil(totalBeats());
                for (int beat = 0; beat <= totalBeatLines; ++beat)
                {
                    if (std::fmod((double) beat, qpb) < 1.0e-9)
                        continue; // the bar line itself is drawn heavier below

                    g.fillRect(geometry_.xForBeat((double) beat), geometry_.rulerHeight,
                               1.0f, height - geometry_.rulerHeight);
                }
            }

            for (int bar = 0; bar <= numBars; ++bar)
            {
                const float x = geometry_.xForBeat((double) bar * qpb);
                g.setColour(juce::Colours::white.withAlpha(0.16f));
                g.fillRect(x, 0.0f, 1.0f, height);
                g.setColour(juce::Colours::white.withAlpha(0.5f));
                g.drawText(juce::String(bar + 1), (int) x + 4, 2, 40, (int) geometry_.rulerHeight - 4,
                           juce::Justification::centredLeft);
            }
        }


        // Under the lanes, so a range's shading never covers a clip.
        paintMarkers(g, height);

        // Track lanes + clips.
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            if (rowOfTrack_[(size_t) i] < 0)
                continue; // in a collapsed folder

            const auto& track = song_.tracks[(size_t) i];
            const float y     = laneTop(i);

            if (rowOfTrack_[(size_t) i] % 2 == 0)
            {
                g.setColour(juce::Colours::white.withAlpha(0.03f));
                g.fillRect(0.0f, y, width, geometry_.laneHeight);
            }

            const auto colour = trackColour(track.colour);

            // A stripe down the left of the gutter, so the colour is legible
            // even on a track whose clips are all scrolled out of view.
            g.setColour(colour.withAlpha(track.muted ? 0.35f : 1.0f));
            g.fillRect(0.0f, y, 4.0f, geometry_.laneHeight);

            // The type tag. Track names double as the type indicator until
            // someone renames one — call an audio track "Verse" and nothing
            // would say it was audio any more. This is what makes renaming
            // free.
            const auto tagArea = juce::Rectangle<float>(tagLeft(i), y + geometry_.laneHeight * 0.5f - 8.0f,
                                                        32.0f, 16.0f);
            paintFolderMarks(g, i, y);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillRoundedRectangle(tagArea, 3.0f);
            g.setColour(juce::Colours::white.withAlpha(track.muted ? 0.35f : 0.7f));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(trackTypeTag(track.type), tagArea, juce::Justification::centred);

            // A muted track's name dims with it, so the state reads from the
            // whole row rather than only from the icon.
            const float nameX     = tagArea.getRight() + 6.0f;
            const float nameWidth = muteButtonBounds(i).getX() - nameX - 4.0f;

            g.setColour(juce::Colours::white.withAlpha(track.muted ? 0.35f : 0.85f));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(track.name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(track.name),
                       (int) nameX, (int) y, (int) juce::jmax(10.0f, nameWidth),
                       (int) geometry_.laneHeight, juce::Justification::centredLeft);

            if (auto* icon = track.muted ? mutedIcon_.get() : unmutedIcon_.get())
            {
                const auto bounds = muteButtonBounds(i);

                if (i == hoveredMuteTrack_)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.10f));
                    g.fillRoundedRectangle(bounds.expanded(2.0f), 3.0f);
                }

                // Drawn inside its hit area for the same reason the gear is:
                // the speaker fills its box on the width axis and was the
                // other heavy glyph in the gutter. Still the larger of the
                // two, since mute is the control you reach for and the gear
                // is settings.
                icon->drawWithin(g, bounds.withSizeKeepingCentre(kMuteGlyphSize, kMuteGlyphSize),
                                 juce::RectanglePlacement::centred, 1.0f);
            }

            // While an alt-drag is live, mark the track it would copy.
            if (i == duplicateDragTrack_ && duplicateDragMoved_)
            {
                g.setColour(theme::colour(*this, theme::accentId).withAlpha(0.18f));
                g.fillRect(0.0f, y, geometry_.gutterWidth, geometry_.laneHeight);
            }

            if (gearIcon_ != nullptr)
            {
                const auto bounds = gearButtonBounds(i);

                if (i == hoveredGearTrack_)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.10f));
                    g.fillRoundedRectangle(bounds.expanded(2.0f), 3.0f);
                }

                // Drawn smaller than the area it responds to. The gear's
                // artwork is square and fills its box, where the speaker
                // beside it is wider than it is tall and so fills only 22x18
                // — drawn at the same size the gear reads as the heaviest
                // thing in the gutter, which a settings affordance shouldn't
                // be. The hit target stays the full size: shrinking the
                // glyph shouldn't make the button harder to hit.
                gearIcon_->drawWithin(g, bounds.withSizeKeepingCentre(kGearGlyphSize, kGearGlyphSize),
                                      juce::RectanglePlacement::centred, 1.0f);
            }

            for (int c = 0; c < (int) track.clips.size(); ++c)
            {
                // Drawn separately, as a single ghost overlay after every
                // lane is painted (see below) — that's the one code path
                // that has to handle both "still on its own lane" and
                // "currently over a different one," so the in-place special
                // case that used to live here is gone.
                if (dragging_ && i == dragTrackIndex_ && c == dragClipIndex_)
                    continue;

                const auto&  clip          = track.clips[(size_t) c];
                const bool   isEditSelected = i == selectedTrackForEdit_ && c == selectedClipForEdit_;
                const double startBeats     = clip.startBeats;
                const double lengthBeats    = clip.lengthBeats;

                const float cx = geometry_.xForBeat(startBeats);
                const float cw = juce::jmax(2.0f, (float) lengthBeats * ppb);
                const juce::Rectangle<float> r(cx, y + 3.0f, cw, geometry_.laneHeight - 6.0f);
                // The track's own colour, which is most of the point of
                // having one: parts are told apart by the clips, not by the
                // gutter you have to look away to read.
                auto clipColour = trackColour(track.colour);
                if (track.muted)
                    clipColour = clipColour.withMultipliedSaturation(0.3f).withMultipliedBrightness(0.7f);

                g.setColour(clipColour);
                g.fillRoundedRectangle(r, 3.0f);

                if (const auto lanes = takeLanesFor(clip, r); lanes.shown())
                    paintTakeLanes(g, clip, r, lanes, i);
                else
                    paintClipContents(g, clip, r);

                if (showEnvelopes_ && clip.type == model::ClipType::Audio)
                    paintEnvelope(g, envelopeEditing_ && i == envelopeTrack_ && c == envelopeClip_
                                         ? envelopePreview_ : clip.envelope,
                                  clip, r);

                // A grip along the right edge, so the resize handle is
                // visible rather than only discoverable by hovering.
                if (r.getWidth() > 3.0f * kResizeEdgePixels)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.18f));
                    g.fillRect(r.getRight() - kResizeEdgePixels, r.getY() + 2.0f,
                               kResizeEdgePixels - 1.0f, r.getHeight() - 4.0f);

                    // Audio clips can be trimmed from the left as well — see
                    // isOnClipLeftEdge.
                    if (clip.type == model::ClipType::Audio)
                        g.fillRect(r.getX() + 1.0f, r.getY() + 2.0f,
                                   kResizeEdgePixels - 1.0f, r.getHeight() - 4.0f);
                }

                g.setColour(isEditSelected ? theme::colour(*this, theme::accentId).withAlpha(0.9f) : juce::Colours::black.withAlpha(0.3f));
                g.drawRoundedRectangle(r, 3.0f, isEditSelected ? 2.0f : 1.0f);
            }

            // An empty lane otherwise looks identical to a broken one —
            // EffectChainPanel and SessionView already say so when they're
            // empty; a track with nothing arranged deserves the same.
            if (track.clips.empty())
            {
                g.setColour(juce::Colours::white.withAlpha(track.muted ? 0.2f : 0.35f));
                g.setFont(juce::FontOptions(12.0f));
                g.drawText(track.type == model::TrackType::Bus
                               ? "Bus - mixes the tracks routed or sent to it (Out and Sends on their mixer strips)"
                               : "No clips - select this track and click + Clip to add one",
                           juce::Rectangle<float>(timelineX + 8.0f, y, width - timelineX - 16.0f,
                                                  geometry_.laneHeight),
                           juce::Justification::centredLeft);
            }
        }

        paintTimeSelection(g);
        paintRazorAreas(g);
        paintTempoChanges(g, height);

        // The dragged clip's ghost, drawn once here rather than inline in
        // the loop above: a resize always stays on dragTrackIndex_'s lane, a
        // move follows dragPreviewTrackIndex_ — which may be a different
        // lane than the clip's own, mid cross-track drag. Kept in the
        // source track's colour throughout, even while hovering a different
        // lane: it hasn't landed there yet, and recolouring it would read as
        // "this already belongs to that track."
        if (dragging_ && dragTrackIndex_ >= 0 && dragTrackIndex_ < (int) song_.tracks.size())
        {
            const int ghostRow = (resizing_ || trimmingStart_ || slipping_ || fadeDrag_ != 0)
                                     ? dragTrackIndex_
                                     : dragPreviewTrackIndex_;
            if (ghostRow >= 0 && ghostRow < (int) song_.tracks.size())
            {
                const float ghostY = laneTop(ghostRow);
                const float cx     = geometry_.xForBeat(dragPreviewStart_);
                const float cw     = juce::jmax(2.0f, (float) dragPreviewLength_ * ppb);
                const juce::Rectangle<float> r(cx, ghostY + 3.0f, cw, geometry_.laneHeight - 6.0f);

                auto ghostColour = trackColour(song_.tracks[(size_t) dragTrackIndex_].colour).brighter(0.3f);
                g.setColour(ghostColour);
                g.fillRoundedRectangle(r, 3.0f);

                if (dragClipIndex_ >= 0 && dragClipIndex_ < (int) song_.tracks[(size_t) dragTrackIndex_].clips.size())
                {
                    // Drawn as it will be once dropped: a left-edge trim moves
                    // where in the file the waveform starts, not just the box.
                    auto ghostClip = song_.tracks[(size_t) dragTrackIndex_].clips[(size_t) dragClipIndex_];
                    if (trimmingStart_)
                        ghostClip = trimClipStart(ghostClip, dragPreviewStart_, clock_, kMinClipBeats);
                    if (fadeDrag_ != 0)
                        ghostClip.fades = dragPreviewFades_;
                    if (slipping_)
                        ghostClip.sourceOffsetSeconds = dragPreviewOffset_;
                    paintClipContents(g, ghostClip, r);
                }

                if (r.getWidth() > 3.0f * kResizeEdgePixels)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.18f));
                    g.fillRect(r.getRight() - kResizeEdgePixels, r.getY() + 2.0f,
                               kResizeEdgePixels - 1.0f, r.getHeight() - 4.0f);
                }

                g.setColour(theme::colour(*this, theme::accentId).withAlpha(0.9f));
                g.drawRoundedRectangle(r, 3.0f, 2.0f);
            }
        }

        // Gutter separator.
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRect(timelineX - 1.0f, 0.0f, 1.0f, height);

        // Playhead.
        const float px = geometry_.xForBeat(playheadBeats_);
        if (px >= timelineX && px <= width)
        {
            g.setColour(theme::colour(*this, theme::signalInkId).withAlpha(0.9f));
            g.fillRect(px, 0.0f, 2.0f, height);
        }

        // Drop preview: a file is being dragged over the timeline.
        if (fileDragActive_)
        {
            const float dx = geometry_.xForBeat(dropPreviewBeat_);
            g.setColour(theme::colour(*this, theme::accentId).withAlpha(0.5f));
            g.fillRect(dx, 0.0f, 2.0f, height);
        }
    }

    /** Test access to the mute geometry. The GUI tests assert that the
        painting and the hit-testing agree, which is only checkable from
        outside if both are reachable. */
    juce::Rectangle<float> muteButtonBoundsForTesting(int trackIndex) const { return muteButtonBounds(trackIndex); }
    int   muteButtonAtForTesting(juce::Point<float> point) const { return muteButtonAt(point); }
    juce::Rectangle<float> gearButtonBoundsForTesting(int trackIndex) const { return gearButtonBounds(trackIndex); }
    static constexpr float gearGlyphSizeForTesting() { return kGearGlyphSize; }
    static constexpr float muteGlyphSizeForTesting() { return kMuteGlyphSize; }
    bool  isOnRulerForTesting(juce::Point<float> point) const { return isOnRuler(point); }
    int   trackAtYForTesting(float y) const { return trackAtY(y); }
    float laneHeightForTesting() const { return geometry_.laneHeight; }
    float rulerHeightForTesting() const { return geometry_.rulerHeight; }
    static constexpr float muteSizeForTesting() { return kMuteSize; }
    int   gearButtonAtForTesting(juce::Point<float> point) const { return gearButtonAt(point); }
    float gutterWidthForTesting() const { return geometry_.gutterWidth; }
    static bool typesAreCompatibleForClipMoveForTesting(model::TrackType a, model::TrackType b)
    {
        return typesAreCompatibleForClipMove(a, b);
    }

private:
    /** Draws an audio clip's waveform.

        Only the part that is heard: an audio clip plays its file once from
        the clip's start and goes silent when the file runs out, so a waveform
        stretched to fill the clip would show something it doesn't do. See
        audioClipDrawnFraction, which is where that rule lives and is tested.

        A thumbnail still scanning draws nothing rather than a partial
        waveform that would redraw a moment later looking different. */
    void paintAudioClipContents(juce::Graphics& g, const model::Clip& clip,
                                juce::Rectangle<float> bounds, float insetY = 3.0f)
    {
        if (clip.audioFile.empty() || bounds.getWidth() < 8.0f || bounds.getHeight() < 2.0f * insetY + 2.0f)
            return;

        auto* thumbnail = waveforms_.find(juce::File(clip.audioFile));
        if (thumbnail == nullptr || thumbnail->getTotalLength() <= 0.0)
            return;

        // What's left of the file from the clip's offset on: a trimmed or
        // split clip starts partway into its recording.
        // The clip's seconds per beat, on average over it: through the tempo
        // map, so a clip after a tempo change is drawn as long as it plays.
        // A warped clip's file is drawn stretched as it plays: its offset is
        // in that time, the file's own is that divided by the stretch.
        const double stretch        = model::warpedit::factorFor(song_, clip);
        const double fileSeconds    = thumbnail->getTotalLength() * stretch - clip.sourceOffsetSeconds;
        const double secondsPerBeat = clipSecondsFor(clip) / juce::jmax(1.0e-9, clip.lengthBeats);
        const double fraction       = audioClipDrawnFraction(fileSeconds, clip.lengthBeats, secondsPerBeat);
        const double seconds        = audioClipAudibleSeconds(fileSeconds, clip.lengthBeats, secondsPerBeat);
        if (fraction <= 0.0 || seconds <= 0.0)
            return;

        auto area = bounds.reduced(2.0f, insetY);
        area.setWidth((float) (area.getWidth() * fraction));
        if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
            return;

        // As a spectrogram once its picture is ready (the waveform until
        // then): the columns for the part of the file the clip plays,
        // stretched over it. A column sits at the middle of its window.
        if (showSpectrograms_)
            if (const auto* picture = spectrograms_.find(juce::File(clip.audioFile)); picture != nullptr && picture->isReady())
            {
                const double firstAt    = picture->windowSeconds * 0.5;
                const double columnFrom = (clip.sourceOffsetSeconds / stretch - firstAt) / picture->secondsPerColumn;
                const double columnTo   = ((clip.sourceOffsetSeconds + seconds) / stretch - firstAt) / picture->secondsPerColumn;
                const auto   source     = juce::Rectangle<float>((float) columnFrom, 0.0f, (float) (columnTo - columnFrom),
                                                                 (float) picture->image.getHeight());

                juce::Graphics::ScopedSaveState state(g);
                g.reduceClipRegion(area.toNearestInt());
                g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
                g.drawImageTransformed(picture->image,
                                       juce::AffineTransform::translation(-source.getX(), 0.0f)
                                           .scaled(area.getWidth() / source.getWidth(), area.getHeight() / source.getHeight())
                                           .translated(area.getX(), area.getY()));
                return;
            }

        // Scaled by the clip's own gain, so a normalised or turned-down clip
        // looks different on the timeline too. Drawing at a fixed 1.0 meant
        // the level was visible in the audio editor and invisible here, which
        // is worse than showing it nowhere: two views of the same clip
        // disagreeing reads as one of them being wrong.
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        thumbnail->drawChannels(g, area.toNearestInt(),
                                clip.sourceOffsetSeconds / stretch, (clip.sourceOffsetSeconds + seconds) / stretch,
                                juce::Decibels::decibelsToGain(clip.gainDb));
    }

    /** Pixels per second of audio at the current zoom and the tempo at
        @p beat. */
    float pixelsPerSecondAt(double beat) const
    {
        return geometry_.pixelsPerBeat() * (float) (juce::jmax(1.0, clock_.bpmAt(beat)) / 60.0);
    }

    /** The x of the moment @p seconds into @p clip, through the tempo map. */
    float xForClipSeconds(const model::Clip& clip, double seconds) const
    {
        return geometry_.xForBeat(clock_.beatAfter(clip.startBeats, seconds));
    }

    /** How long @p clip's window lasts, in seconds. */
    double clipSecondsFor(const model::Clip& clip) const
    {
        return clock_.secondsBetween(clip.startBeats, clip.startBeats + clip.lengthBeats);
    }

    /** How long an audio clip's whole file is, in seconds, or 0 while it is
        still being scanned. */
    double fileSecondsFor(const model::Clip& clip) const
    {
        if (auto* thumbnail = waveforms_.find(juce::File(clip.audioFile)); thumbnail != nullptr)
            return juce::jmax(0.0, thumbnail->getTotalLength()) * model::warpedit::factorFor(song_, clip);
        return 0.0;
    }

    /** How long an audio clip is heard for, in seconds: its window, cut short
        where its file runs out. Falls back to the window while the file is
        still being scanned. */
    double audibleSecondsFor(const model::Clip& clip) const
    {
        const double window = clipSecondsFor(clip);

        if (auto* thumbnail = waveforms_.find(juce::File(clip.audioFile));
            thumbnail != nullptr && thumbnail->getTotalLength() > 0.0)
            return juce::jlimit(0.0, window, thumbnail->getTotalLength() * model::warpedit::factorFor(song_, clip)
                                                 - clip.sourceOffsetSeconds);

        return window;
    }

    /** Where an audio clip's two fade handles sit: the x of the end of the
        fade-in and of the start of the fade-out. A fade of zero puts its
        handle in the corner, which is where you reach to start one. */
    std::pair<float, float> fadeHandleXs(const model::Clip& clip) const
    {
        const double audible = audibleSecondsFor(clip);
        const auto   fitted  = engine::fittedFades(clip.fades, audible);

        return { xForClipSeconds(clip, fitted.inSeconds), xForClipSeconds(clip, audible - fitted.outSeconds) };
    }

    /** Which fade handle of the clip on @p trackIndex's lane @p point is on:
        +1 for the fade-in, -1 for the fade-out, 0 for neither. Handles live
        along the clip's top edge, so the lower part of each edge is still the
        trim and resize grip. */
    int fadeHandleAt(const model::Clip& clip, int trackIndex, juce::Point<float> point) const
    {
        if (clip.type != model::ClipType::Audio)
            return 0;

        const float top = laneTop(trackIndex) + 3.0f;
        if (point.y < top || point.y > top + kFadeHandleSize + 2.0f)
            return 0;

        const auto [inX, outX] = fadeHandleXs(clip);

        // The fade-in wins a tie: on a clip too narrow for both handles,
        // one of them has to be reachable.
        if (point.x >= inX - kFadeHandleSize && point.x <= inX + kFadeHandleSize)
            return +1;
        if (point.x >= outX - kFadeHandleSize && point.x <= outX + kFadeHandleSize)
            return -1;
        return 0;
    }

    /** Shades what an audio clip's fades take away, draws each curve, and
        marks the handles. Drawn with engine::fittedFades and
        engine::fadeCurve, the same functions playback uses, so the picture
        is the sound. */
    void paintClipFades(juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> bounds)
    {
        if (bounds.getWidth() < 3.0f * kFadeHandleSize || bounds.getHeight() < 2.0f * kFadeHandleSize)
            return;

        const double audible = audibleSecondsFor(clip);
        const auto   fitted  = engine::fittedFades(clip.fades, audible);
        const float  left    = bounds.getX();
        const float  top     = bounds.getY();
        const float  bottom  = bounds.getBottom();

        auto paintFade = [&](double seconds, engine::FadeShape shape, bool isFadeIn)
        {
            if (seconds <= 0.0)
                return;

            const float startX = isFadeIn ? left : xForClipSeconds(clip, audible - seconds);
            const float width  = (isFadeIn ? xForClipSeconds(clip, seconds) : xForClipSeconds(clip, audible)) - startX;

            // The shaded region runs along the top edge and back under the
            // curve: everything above the curve is level the fade removes.
            juce::Path shaded, curve;
            shaded.startNewSubPath(startX, top);

            constexpr int kSteps = 24;
            for (int i = 0; i <= kSteps; ++i)
            {
                const double t    = (double) i / kSteps;
                const float  gain = engine::fadeCurve(shape, isFadeIn ? t : 1.0 - t);
                const float  x    = startX + (float) t * width;
                const float  y    = bottom - gain * (bottom - top);

                shaded.lineTo(x, y);
                if (i == 0)
                    curve.startNewSubPath(x, y);
                else
                    curve.lineTo(x, y);
            }

            shaded.lineTo(startX + width, top);
            shaded.closeSubPath();

            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillPath(shaded);
            g.setColour(juce::Colours::white.withAlpha(0.7f));
            g.strokePath(curve, juce::PathStrokeType(1.0f));
        };

        paintFade(fitted.inSeconds, fitted.inShape, true);
        paintFade(fitted.outSeconds, fitted.outShape, false);

        // Always drawn, even with no fade: a handle you can't see is a
        // feature nobody finds.
        const auto [inX, outX] = fadeHandleXs(clip);
        const float maxX       = bounds.getRight() - kFadeHandleSize;

        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.fillRect(juce::jlimit(left, maxX, inX), top, kFadeHandleSize, kFadeHandleSize);
        g.fillRect(juce::jlimit(left, maxX, outX - kFadeHandleSize), top, kFadeHandleSize, kFadeHandleSize);
    }

    /** Where a track's mute button sits, in this component's coordinates.

        One definition used by both the painting and the click handling. Worked
        out separately they drift, and the result is a control drawn in one
        place that responds in another — which looks exactly like a button
        that doesn't work. */
    juce::Rectangle<float> muteButtonBounds(int trackIndex) const
    {
        const float top = laneTop(trackIndex);
        return juce::Rectangle<float>(geometry_.gutterWidth - 2.0f * kMuteSize - 10.0f,
                                      top + (geometry_.laneHeight - kMuteSize) * 0.5f,
                                      kMuteSize, kMuteSize);
    }

    /** The track whose lane a y-coordinate falls in, or -1 above or below
        them all. */
    int trackAtY(float y) const
    {
        if (y < geometry_.rulerHeight)
            return -1;

        const int row = (int) ((y - geometry_.rulerHeight) / geometry_.laneHeight);
        return row >= 0 && row < (int) trackOfRow_.size() ? trackOfRow_[(size_t) row] : -1;
    }

    // Folder tracks (model/Folders.h): which track each lane shows, and
    // which lane each track is on (-1 inside a collapsed folder).
    std::vector<int> trackOfRow_;
    std::vector<int> rowOfTrack_;

    void rebuildRows()
    {
        trackOfRow_ = model::folderedit::visibleTrackIndices(song_);
        rowOfTrack_.assign(song_.tracks.size(), -1);
        for (int row = 0; row < (int) trackOfRow_.size(); ++row)
            rowOfTrack_[(size_t) trackOfRow_[(size_t) row]] = row;
    }

    /** The top of track @p trackIndex's lane, or far off the top for one
        inside a collapsed folder, so nothing there is ever hit. */
    float laneTop(int trackIndex) const
    {
        if (trackIndex < 0 || trackIndex >= (int) rowOfTrack_.size() || rowOfTrack_[(size_t) trackIndex] < 0)
            return -1.0e6f;
        return geometry_.rulerHeight + (float) rowOfTrack_[(size_t) trackIndex] * geometry_.laneHeight;
    }

    static constexpr float kFolderIndent = 12.0f;

    /** Where a track's type tag starts: indented in a folder, and past the
        triangle on a folder. */
    float tagLeft(int trackIndex) const
    {
        if (model::folderedit::parentIndexOf(song_, trackIndex) >= 0)
            return 10.0f + kFolderIndent;
        return model::folderedit::isFolder(song_, trackIndex) ? 20.0f : 10.0f;
    }

    /** A folder's collapse triangle, or nothing for a track that isn't one. */
    juce::Rectangle<float> folderToggleBounds(int trackIndex) const
    {
        if (! model::folderedit::isFolder(song_, trackIndex))
            return {};
        return { 5.0f, laneTop(trackIndex) + geometry_.laneHeight * 0.5f - 7.0f, 14.0f, 14.0f };
    }

    int folderToggleAt(juce::Point<float> point) const
    {
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
            if (folderToggleBounds(i).contains(point))
                return i;
        return -1;
    }

    /** A folder's triangle, pointing down when open; a line down the gutter
        beside the tracks in one. */
    void paintFolderMarks(juce::Graphics& g, int trackIndex, float y)
    {
        if (const auto toggle = folderToggleBounds(trackIndex); ! toggle.isEmpty())
        {
            const bool collapsed = song_.tracks[(size_t) trackIndex].folderCollapsed;
            const auto c         = toggle.getCentre();
            juce::Path triangle;
            if (collapsed)
                triangle.addTriangle(c.x - 3.0f, c.y - 5.0f, c.x - 3.0f, c.y + 5.0f, c.x + 4.0f, c.y);
            else
                triangle.addTriangle(c.x - 5.0f, c.y - 3.0f, c.x + 5.0f, c.y - 3.0f, c.x, c.y + 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.75f));
            g.fillPath(triangle);
        }
        else if (model::folderedit::parentIndexOf(song_, trackIndex) >= 0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.18f));
            g.fillRect(12.0f, y, 2.0f, geometry_.laneHeight);
        }
    }

    /** True for a point on the ruler strip, right of the gutter. The gutter's
        share of that strip is not a scrub target: it sits above the track
        names, not above any part of the timeline. */
    bool isOnRuler(juce::Point<float> point) const
    {
        return point.y < geometry_.rulerHeight && point.x >= geometry_.gutterWidth;
    }

    void scrubTo(float x)
    {
        if (onSeek)
            onSeek(geometry_.beatForX(x));
    }

    /** The track whose mute button contains @p point, or -1. */
    int muteButtonAt(juce::Point<float> point) const
    {
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
            if (muteButtonBounds(i).contains(point))
                return i;
        return -1;
    }

    /** The gear sits just right of the mute, sharing its vertical placement. */
    juce::Rectangle<float> gearButtonBounds(int trackIndex) const
    {
        return muteButtonBounds(trackIndex).translated(kMuteSize + 4.0f, 0.0f);
    }

    int gearButtonAt(juce::Point<float> point) const
    {
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
            if (gearButtonBounds(i).contains(point))
                return i;
        return -1;
    }

    /** Draws what's inside a clip as small blocks, so two clips holding
        different music don't look identical. Audio clips are left plain:
        there is no waveform cached here, and reading the file at paint time
        is not something a paint routine should do. */
    /** Which take a clip that has several plays, in its bottom-left corner:
        a comped stretch reads as the take each piece came from. */
    void paintClipTake(juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> bounds)
    {
        if (clip.takes.size() < 2 || bounds.getWidth() < 40.0f || bounds.getHeight() < 24.0f)
            return;

        const auto& take  = clip.takes[(size_t) juce::jlimit(0, (int) clip.takes.size() - 1, clip.activeTake)];
        const auto  label = (take.name.empty() ? "Take " + juce::String(clip.activeTake + 1) : juce::String(take.name))
                          + " / " + juce::String((int) clip.takes.size());

        g.setFont(juce::FontOptions(10.0f));
        const auto area = bounds.reduced(4.0f, 3.0f).removeFromBottom(12.0f);
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(area.withWidth(juce::jmin(area.getWidth(), 8.0f + 6.0f * (float) label.length())));
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawText(label, area.reduced(3.0f, 0.0f), juce::Justification::centredLeft, true);
    }

    /** A clip's take lanes, or none: none unless they're shown, the clip is
        audio with two takes or more, and its box is tall enough for them. */
    app::TakeLaneLayout takeLanesFor(const model::Clip& clip, juce::Rectangle<float> bounds) const
    {
        if (! showTakeLanes_ || clip.type != model::ClipType::Audio)
            return {};
        return app::takeLaneLayout(bounds.getY(), bounds.getHeight(), (int) clip.takes.size());
    }

    /** A take's name as the lanes and the header show it. */
    static juce::String takeLabel(const model::Clip& clip, int take)
    {
        const auto& name = clip.takes[(size_t) take].name;
        return name.empty() ? "Take " + juce::String(take + 1) : juce::String(name);
    }

    /** Draws a clip as its take lanes: the header, naming the take that
        plays, and a row per take with that take's audio where the clip is,
        the one playing lit and the rest dimmed. A swipe being dragged along a
        row of this track shows over it. */
    void paintTakeLanes(juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> bounds,
                        const app::TakeLaneLayout& lanes, int trackIndex)
    {
        for (int t = 0; t < lanes.rowCount; ++t)
        {
            const auto row     = juce::Rectangle<float>(bounds.getX(), lanes.rowTop(t), bounds.getWidth(), lanes.rowHeight);
            const bool playing = t == clip.activeTake;

            if (playing)
            {
                g.setColour(juce::Colours::white.withAlpha(0.14f));
                g.fillRect(row);
            }

            auto take                = clip;
            take.audioFile           = clip.takes[(size_t) t].audioFile;
            take.sourceOffsetSeconds = model::takeedit::takeOffsetSeconds(clip, clip.takes[(size_t) t]);
            paintAudioClipContents(g, take, row, 1.0f);
            if (! playing)
            {
                g.setColour(juce::Colours::black.withAlpha(0.3f));
                g.fillRect(row);
            }

            if (lanes.rowHeight >= 12.0f && row.getWidth() > 40.0f)
            {
                g.setFont(juce::FontOptions(9.5f));
                g.setColour(juce::Colours::white.withAlpha(playing ? 0.85f : 0.5f));
                g.drawText(takeLabel(clip, t), row.reduced(4.0f, 0.0f), juce::Justification::centredLeft, true);
            }

            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillRect(row.getX(), row.getY(), row.getWidth(), 1.0f);
        }

        // The header: which take plays here, as a plain clip says it.
        const auto header = juce::Rectangle<float>(bounds.getX(), lanes.headerTop, bounds.getWidth(),
                                                   app::kTakeLaneHeaderHeight);
        if (header.getWidth() > 40.0f)
        {
            const int playing = juce::jlimit(0, (int) clip.takes.size() - 1, clip.activeTake);
            g.setFont(juce::FontOptions(9.5f));
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawText(takeLabel(clip, playing) + " / " + juce::String((int) clip.takes.size()),
                       header.reduced(kFadeHandleSize + 4.0f, 0.0f), juce::Justification::centredLeft, true);
        }

        paintClipFades(g, clip, bounds);

        if (swiping_ && swipeMoved_ && trackIndex == swipeTrack_ && swipeTake_ < lanes.rowCount)
        {
            const double from = std::min(swipeAnchorBeat_, swipeCurrentBeat_);
            const double to   = std::max(swipeAnchorBeat_, swipeCurrentBeat_);
            const float  x0   = std::max(bounds.getX(), geometry_.xForBeat(from));
            const float  x1   = std::min(bounds.getRight(), geometry_.xForBeat(to));
            if (x1 > x0)
            {
                const auto band = juce::Rectangle<float>(x0, lanes.rowTop(swipeTake_), x1 - x0, lanes.rowHeight);
                g.setColour(theme::colour(*this, theme::accentId).withAlpha(0.3f));
                g.fillRect(band);
                g.setColour(theme::colour(*this, theme::accentId).withAlpha(0.9f));
                g.drawRect(band, 1.0f);
            }
        }
    }

    void paintClipContents(juce::Graphics& g, const model::Clip& clip,
                           juce::Rectangle<float> bounds)
    {
        if (clip.type == model::ClipType::Audio)
        {
            paintAudioClipContents(g, clip, bounds);
            paintClipFades(g, clip, bounds);
            paintClipTake(g, clip, bounds);
            return;
        }

        if (clip.type != model::ClipType::Instrument)
            return;

        // Below this the blocks are smaller than the corner rounding and read
        // as noise rather than as content.
        if (bounds.getWidth() < 16.0f || bounds.getHeight() < 10.0f)
            return;

        const auto area = bounds.reduced(2.0f, 3.0f);
        if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
            return;

        const auto blocks = clipPreviewBlocks(clip.pattern.notes, clip.pattern.lengthBeats,
                                              clip.lengthBeats);
        if (blocks.empty())
            return;

        g.setColour(juce::Colours::white.withAlpha(0.55f));

        for (const auto& block : blocks)
        {
            // At least a pixel each way: a sixteenth in a long clip rounds to
            // nothing otherwise, and a clip that looks empty is worse than
            // one that looks approximate.
            const float w = juce::jmax(1.0f, (float) block.width * area.getWidth());
            const float h = juce::jmax(1.0f, (float) block.height * area.getHeight());

            g.fillRect(area.getX() + (float) block.x * area.getWidth(),
                       area.getY() + (float) block.y * area.getHeight(),
                       w, h);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Checked before the clip hit-test: the button sits in the gutter,
        // where no clip can be, but a click there would otherwise fall
        // through to the seek at the bottom of this function and move the
        // playhead every time someone muted a track.
        if (const int folder = folderToggleAt(e.position); folder >= 0)
        {
            if (onFolderToggled)
                onFolderToggled(folder);
            return;
        }

        if (const int muteTrack = muteButtonAt(e.position); muteTrack >= 0)
        {
            if (onTrackMuteToggled)
                onTrackMuteToggled(muteTrack);
            return;
        }

        if (const int gearTrack = gearButtonAt(e.position); gearTrack >= 0)
        {
            if (onTrackSettingsRequested)
                onTrackSettingsRequested(gearTrack);
            return;
        }

        // The ruler is a scrub bar: press anywhere along it and the playhead
        // follows the mouse until release. Checked before the clip hit-test
        // because the ruler sits above every lane, so nothing there can be a
        // clip anyway — and checking first keeps the two from ever competing.
        if (isOnRuler(e.position))
        {
            if (e.mods.isPopupMenu())
            {
                if (const int marker = markerAt(e.position); marker >= 0)
                {
                    if (onMarkerMenuRequested)
                        onMarkerMenuRequested(marker);
                }
                else
                    showTempoMenu(e.position.x);
                return;
            }

            // A tempo change's flag is picked up rather than scrubbed through.
            if (const int change = tempoChangeAt(e.position); change >= 0)
            {
                draggingTempo_     = change;
                tempoDragFromBeat_ = song_.tempoChanges[(size_t) change].beat;
                tempoDragToBeat_   = tempoDragFromBeat_;
                return;
            }

            // A marker is picked up rather than scrubbed through: dragging
            // moves it, and a click goes to it (see mouseUp).
            if (const int markerId = markerAt(e.position); markerId >= 0)
            {
                if (const auto* marker = model::findMarker(song_, markerId))
                {
                    markerDragId_        = markerId;
                    markerDragMoved_     = false;
                    markerPressX_        = e.position.x;
                    markerGrabBeat_      = geometry_.beatForX(e.position.x);
                    markerOriginalStart_ = marker->startBeats;
                    markerPreviewStart_  = marker->startBeats;
                    return;
                }
            }

            scrubbing_      = true;
            scrubAudible_   = false;
            scrubPressX_    = e.position.x;
            scrubTo(e.position.x);
            return;
        }

        // Razor areas. Ctrl+Shift-drag (Cmd on a Mac) adds one on each lane
        // it crosses, keeping those already made; a drag starting inside one
        // moves them all; any other click on the lanes lets them go.
        if (! e.mods.isPopupMenu() && e.position.x >= geometry_.gutterWidth && trackAtY(e.position.y) >= 0)
        {
            const int lane = trackAtY(e.position.y);
            if (e.mods.isShiftDown() && e.mods.isCommandDown())
            {
                razorDragging_    = true;
                razorBase_        = razorAreas_;
                razorAnchorBeat_  = snappedBeatAt(e.position.x, e.mods.isAltDown());
                razorAnchorTrack_ = lane;
                return;
            }

            if (! razorAreas_.empty() && ! e.mods.isAnyModifierKeyDown()
                && model::razoredit::areaAt(razorAreas_, song_.tracks[(size_t) lane].id,
                                            geometry_.beatForX(e.position.x)) != nullptr)
            {
                razorMoving_      = true;
                razorMoveMoved_   = false;
                razorPressX_      = e.position.x;
                razorGrabBeat_    = snappedBeatAt(e.position.x, false);
                razorGrabTrack_   = lane;
                razorDeltaBeats_  = 0.0;
                razorDeltaTracks_ = 0;
                return;
            }

            changeRazorAreas({});
        }

        // Shift-drag selects time anywhere on the lanes, over clips as well as
        // between them: without it a selection could only start in a gap.
        if (e.mods.isShiftDown() && ! e.mods.isPopupMenu() && e.position.x >= geometry_.gutterWidth
            && trackAtY(e.position.y) >= 0)
        {
            beginTimeSelection(e);
            return;
        }

        int trackIndex = -1, clipIndex = -1;
        if (findClipAt(e.position, trackIndex, clipIndex))
        {
            if (e.mods.isPopupMenu())
            {
                if (onClipSelected)
                    onClipSelected(trackIndex, clipIndex);
                if (onClipMenuRequested)
                    onClipMenuRequested(trackIndex, clipIndex);
                return;
            }

            // Picking up a clip is working on that clip, so the edit commands
            // go back to it rather than to a time selection made earlier.
            changeTimeSelection({});

            const auto& clip = song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];

            // With curves shown, a click on an audio clip edits its curve.
            // Started before the clip is reported selected: selecting can
            // hand this view a new song, and the edit reads the clip.
            if (showEnvelopes_ && clip.type == model::ClipType::Audio)
            {
                beginEnvelopeEdit(trackIndex, clipIndex, e);
                if (onClipSelected)
                    onClipSelected(trackIndex, clipIndex);
                return;
            }

            // A take's row, with take lanes shown: a click plays that take, a
            // drag along it comps (see mouseUp). The edge grips still trim,
            // Ctrl still slips, and the header moves the clip.
            if (const auto lanes = takeLanesFor(clip, clipBounds(trackIndex, clip));
                lanes.shown() && ! e.mods.isCommandDown() && ! isOnClipRightEdge(clip, e.position.x)
                && ! isOnClipLeftEdge(clip, e.position.x))
            {
                if (const int row = lanes.rowAt(e.position.y); row >= 0)
                {
                    swiping_          = true;
                    swipeMoved_       = false;
                    swipeTrack_       = trackIndex;
                    swipeClip_        = clipIndex;
                    swipeTake_        = row;
                    swipePressX_      = e.position.x;
                    swipeAnchorBeat_  = snappedBeatAt(e.position.x, e.mods.isAltDown());
                    swipeCurrentBeat_ = swipeAnchorBeat_;
                    if (onClipSelected)
                        onClipSelected(trackIndex, clipIndex);
                    return;
                }
            }

            // A fade handle sits in a clip's top corner, on top of the edge
            // grips, so it is checked first.
            dragging_           = true;
            fadeDrag_           = fadeHandleAt(clip, trackIndex, e.position);
            resizing_           = fadeDrag_ == 0 && isOnClipRightEdge(clip, e.position.x);
            trimmingStart_      = fadeDrag_ == 0 && ! resizing_ && isOnClipLeftEdge(clip, e.position.x);
            // Ctrl-drag (Cmd on a Mac) on a clip's body slips its audio
            // inside its edges. Not Alt, which already inverts snapping, nor
            // Shift, which selects time.
            slipping_           = fadeDrag_ == 0 && ! resizing_ && ! trimmingStart_
                        && clip.type == model::ClipType::Audio && e.mods.isCommandDown();
            dragOriginalOffset_ = clip.sourceOffsetSeconds;
            dragPreviewOffset_  = clip.sourceOffsetSeconds;
            dragOriginalFades_  = clip.fades;
            dragPreviewFades_   = clip.fades;
            dragTrackIndex_     = trackIndex;
            dragClipIndex_      = clipIndex;
            dragGrabBeat_       = geometry_.beatForX(e.position.x);
            dragOriginalStart_  = clip.startBeats;
            dragOriginalLength_ = clip.lengthBeats;
            dragPreviewStart_   = dragOriginalStart_;
            dragPreviewLength_  = dragOriginalLength_;
            dragPreviewTrackIndex_ = trackIndex;

            if (onClipSelected)
                onClipSelected(trackIndex, clipIndex);
            return;
        }

        if (e.position.x < geometry_.gutterWidth)
        {
            // Alt-dragging a track's header duplicates it. Armed here and
            // fired on release after a real drag, so a stray alt-click on a
            // header can't silently add a track.
            //
            // The gesture lives on the header rather than on a clip because
            // alt already means "don't snap" while dragging a clip, and one
            // modifier meaning two things on the same surface is how a user
            // ends up duplicating a track they meant to nudge.
            if (e.mods.isAltDown())
            {
                duplicateDragTrack_ = trackAtY(e.position.y);
                duplicateDragMoved_ = false;
            }
            return; // otherwise the gutter is not the timeline
        }

        // An empty lane: the playhead goes there, and a drag from here selects.
        if (trackAtY(e.position.y) >= 0)
            beginTimeSelection(e);

        if (onSeek)
            onSeek(geometry_.beatForX(e.position.x));
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (envelopeEditing_)
        {
            if (envelopeTrack_ >= 0 && envelopeTrack_ < (int) song_.tracks.size()
                && envelopeClip_ >= 0 && envelopeClip_ < (int) song_.tracks[(size_t) envelopeTrack_].clips.size())
            {
                const auto& clip           = song_.tracks[(size_t) envelopeTrack_].clips[(size_t) envelopeClip_];
                const auto [seconds, gain] = envelopePositionAt(envelopeTrack_, clip, e.position);
                envelopePoint_             = envelopePreview_.movePoint(envelopePoint_, seconds, gain);
                repaint();
            }
            return;
        }

        if (razorDragging_)
        {
            const int last  = (int) song_.tracks.size() - 1;
            const int track = juce::jlimit(0, juce::jmax(0, last), trackAtYUnclamped(e.position.y));
            auto      areas = razorBase_;
            for (const auto& area : model::razoredit::areasFromDrag(song_, razorAnchorBeat_,
                                                                     snappedBeatAt(e.position.x, e.mods.isAltDown()),
                                                                     razorAnchorTrack_, track))
                areas = model::razoredit::addArea(song_, std::move(areas), area);
            changeRazorAreas(areas);
            return;
        }

        if (razorMoving_)
        {
            if (! razorMoveMoved_ && std::abs(e.position.x - razorPressX_) < kSwipeDragPixels
                && trackAtYUnclamped(e.position.y) == razorGrabTrack_)
                return;

            razorMoveMoved_   = true;
            razorDeltaBeats_  = snappedBeatAt(e.position.x, e.mods.isAltDown()) - razorGrabBeat_;
            razorDeltaTracks_ = trackAtYUnclamped(e.position.y) - razorGrabTrack_;
            repaint();
            return;
        }

        if (swiping_)
        {
            if (! swipeMoved_ && std::abs(e.position.x - swipePressX_) < kSwipeDragPixels)
                return;

            swipeMoved_       = true;
            swipeCurrentBeat_ = snappedBeatAt(e.position.x, e.mods.isAltDown());
            repaint();
            return;
        }

        if (draggingTempo_ >= 0)
        {
            // Snapped to the bar: a tempo change a fraction of a beat off the
            // bar line reads as a mistake even when it's meant.
            const double qpb = quartersPerBar();
            tempoDragToBeat_ = std::max(qpb, std::round(geometry_.beatForX(e.position.x) / qpb) * qpb);
            repaint();
            return;
        }

        if (markerDragId_ >= 0)
        {
            // A few pixels before it counts as a move, so a click that
            // wobbles is still a click.
            if (! markerDragMoved_ && std::abs(e.position.x - markerPressX_) < kMarkerDragPixels)
                return;

            markerDragMoved_ = true;

            const auto* marker = model::findMarker(song_, markerDragId_);
            if (marker == nullptr)
                return;

            // Snapped as a span, so a range lands with either edge on
            // something; never onto where it's being dragged from.
            const bool invert  = e.mods.isAltDown();
            auto       magnets = invert ? std::vector<double> {} : magnetsExcluding(-1, -1);
            magnets.erase(std::remove_if(magnets.begin(), magnets.end(), [marker](double m)
                          {
                              return std::abs(m - marker->startBeats) < 1.0e-9
                                  || std::abs(m - (marker->startBeats + marker->lengthBeats)) < 1.0e-9;
                          }),
                          magnets.end());

            const double tolerance = kSnapMagnetPixels / std::max(1.0e-3, (double) geometry_.pixelsPerBeat());
            const double wanted    = markerOriginalStart_ + (geometry_.beatForX(e.position.x) - markerGrabBeat_);
            markerPreviewStart_    = std::max(0.0, app::snapSpanStart(wanted, marker->lengthBeats, magnets, tolerance,
                                                                      snapToGrid_ != invert, snapUnitBeats()));
            repaint();
            return;
        }

        if (selectingTime_)
        {
            const int last  = (int) song_.tracks.size() - 1;
            const int track = e.position.y < geometry_.rulerHeight ? 0
                                : juce::jlimit(0, juce::jmax(0, last), trackAtYUnclamped(e.position.y));
            changeTimeSelection(model::selectionFromDrag(song_, timeAnchorBeat_,
                                                         snappedBeatAt(e.position.x, e.mods.isAltDown()),
                                                         timeAnchorTrack_, track));
            return;
        }

        if (duplicateDragTrack_ >= 0)
        {
            if (! duplicateDragMoved_ && e.getDistanceFromDragStart() >= kDuplicateDragPixels)
            {
                duplicateDragMoved_ = true;
                repaint();
            }
            return;
        }

        if (scrubbing_)
        {
            // Deliberately not restricted to the ruler: once the press has
            // started, dragging down into the lanes or off the edge should
            // keep scrubbing rather than stopping the moment the mouse
            // strays, which is how every transport scrub bar behaves.
            if (! scrubAudible_ && std::abs(e.position.x - scrubPressX_) >= kScrubDragPixels)
            {
                scrubAudible_ = true;
                if (onScrubStarted)
                    onScrubStarted();
            }
            scrubTo(e.position.x);
            return;
        }

        if (! dragging_)
            return;

        const double currentBeat = geometry_.beatForX(e.position.x);

        // Alt *inverts* the snap setting rather than only switching it off.
        // With snapping on it's the usual "let me place this freely just this
        // once"; with snapping off it's the way back to the grid without
        // going to the menu. One modifier that always means "the other one"
        // is easier to remember than one that only works in one direction.
        const bool   invert    = e.mods.isAltDown();
        const bool   gridOn    = snapToGrid_ != invert;
        const auto   magnets   = invert ? std::vector<double> {} : magnetsExcluding(dragTrackIndex_, dragClipIndex_);
        const double tolerance = kSnapMagnetPixels / std::max(1.0e-3, (double) geometry_.pixelsPerBeat());
        const double gridUnit  = snapUnitBeats();

        if (fadeDrag_ != 0)
        {
            // Measured in seconds from the clip's edge, and kept from running
            // into the other fade. Not snapped: a fade's length is about how
            // it sounds, not about the beat grid.
            if (dragTrackIndex_ < (int) song_.tracks.size()
                && dragClipIndex_ < (int) song_.tracks[(size_t) dragTrackIndex_].clips.size())
            {
                const auto&  clip    = song_.tracks[(size_t) dragTrackIndex_].clips[(size_t) dragClipIndex_];
                const double audible = audibleSecondsFor(clip);
                const double atX     = clock_.secondsBetween(clip.startBeats, geometry_.beatForX(e.position.x));

                if (fadeDrag_ > 0)
                    dragPreviewFades_.inSeconds =
                        juce::jlimit(0.0, juce::jmax(0.0, audible - dragPreviewFades_.outSeconds), atX);
                else
                    dragPreviewFades_.outSeconds =
                        juce::jlimit(0.0, juce::jmax(0.0, audible - dragPreviewFades_.inSeconds), audible - atX);
            }
        }
        else if (slipping_)
        {
            // Not snapped: slipping lines audio up by ear and eye, and the
            // clip's edges, which are what the grid is for, don't move.
            if (dragTrackIndex_ < (int) song_.tracks.size()
                && dragClipIndex_ < (int) song_.tracks[(size_t) dragTrackIndex_].clips.size())
            {
                const auto&  clip  = song_.tracks[(size_t) dragTrackIndex_].clips[(size_t) dragClipIndex_];
                const double delta = clock_.secondsBetween(dragGrabBeat_, currentBeat);
                dragPreviewOffset_ = slipClip(clip, delta, fileSecondsFor(clip), clock_).sourceOffsetSeconds;
            }
        }
        else if (trimmingStart_)
        {
            // Through the same function the edit itself uses, so the ghost
            // stops exactly where the drop will: at the file's first sample,
            // or a minimum length short of the clip's end.
            if (dragTrackIndex_ < (int) song_.tracks.size()
                && dragClipIndex_ < (int) song_.tracks[(size_t) dragTrackIndex_].clips.size())
            {
                const auto& clip    = song_.tracks[(size_t) dragTrackIndex_].clips[(size_t) dragClipIndex_];
                const auto  start   = std::max(0.0, app::snapPosition(currentBeat, magnets, tolerance, gridOn, gridUnit));
                const auto  trimmed = trimClipStart(clip, start, clock_, kMinClipBeats);
                dragPreviewStart_  = trimmed.startBeats;
                dragPreviewLength_ = trimmed.lengthBeats;
            }
        }
        else if (resizing_)
        {
            // The end is what's being placed, so the end is what snaps.
            const double end   = app::snapPosition(currentBeat, magnets, tolerance, gridOn, gridUnit);
            dragPreviewLength_ = std::max(kMinClipBeats, end - dragPreviewStart_);
        }
        else
        {
            dragPreviewStart_ = std::max(0.0, app::snapSpanStart(dragOriginalStart_ + (currentBeat - dragGrabBeat_),
                                                                 dragOriginalLength_, magnets, tolerance,
                                                                 gridOn, gridUnit));

            // Follows the mouse into a different lane only if that track can
            // actually take this clip (see typesAreCompatibleForClipMove) —
            // otherwise the ghost just stays put rather than following into
            // a lane it can't be dropped on.
            const int hovered = trackAtY(e.position.y);
            if (hovered >= 0 && hovered < (int) song_.tracks.size()
                && typesAreCompatibleForClipMove(song_.tracks[(size_t) dragTrackIndex_].type,
                                                 song_.tracks[(size_t) hovered].type))
                dragPreviewTrackIndex_ = hovered;
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (duplicateDragTrack_ >= 0)
        {
            const int track = duplicateDragTrack_;
            const bool dragged = duplicateDragMoved_;

            duplicateDragTrack_ = -1;
            duplicateDragMoved_ = false;
            repaint();

            if (dragged && onTrackDuplicateRequested)
                onTrackDuplicateRequested(track);
            return;
        }

        if (razorDragging_)
        {
            razorDragging_ = false;
            return;
        }

        if (razorMoving_)
        {
            razorMoving_ = false;
            repaint();
            if (razorMoveMoved_ && (std::abs(razorDeltaBeats_) > 1.0e-9 || razorDeltaTracks_ != 0)
                && onRazorMoveRequested)
                onRazorMoveRequested(razorDeltaBeats_, razorDeltaTracks_);
            return;
        }

        if (swiping_)
        {
            swiping_ = false;
            repaint();

            const double from = std::min(swipeAnchorBeat_, swipeCurrentBeat_);
            const double to   = std::max(swipeAnchorBeat_, swipeCurrentBeat_);
            if (! swipeMoved_)
            {
                if (onTakeChosen)
                    onTakeChosen(swipeTrack_, swipeClip_, swipeTake_);
            }
            else if (to - from > 1.0e-9 && onTakeSwiped)
                onTakeSwiped(swipeTrack_, swipeTake_, from, to);
            return;
        }

        if (envelopeEditing_)
        {
            envelopeEditing_ = false;
            envelopePoint_   = -1;
            repaint();

            // A click on an existing point that didn't move it changes nothing.
            if (onClipEnvelopeChanged && envelopePreview_ != envelopeOriginal_)
                onClipEnvelopeChanged(envelopeTrack_, envelopeClip_, envelopePreview_);
            return;
        }

        if (draggingTempo_ >= 0)
        {
            const double from = tempoDragFromBeat_, to = tempoDragToBeat_;
            draggingTempo_ = -1;
            repaint();
            if (std::abs(to - from) > 1.0e-9 && onTempoChangeMoved)
                onTempoChangeMoved(from, to);
            return;
        }

        if (markerDragId_ >= 0)
        {
            const int  id    = markerDragId_;
            const bool moved = markerDragMoved_;
            markerDragId_    = -1;
            markerDragMoved_ = false;
            repaint();

            if (moved)
            {
                if (onMarkerMoved && std::abs(markerPreviewStart_ - markerOriginalStart_) > 1.0e-9)
                    onMarkerMoved(id, markerPreviewStart_);
                return;
            }

            // A click: go there, and a range selects what it spans on every
            // track, ready to cut, copy or export.
            if (const auto* marker = model::findMarker(song_, id))
            {
                const double start  = marker->startBeats;
                const double length = marker->lengthBeats;

                if (onSeek)
                    onSeek(start);

                if (length > 0.0)
                {
                    model::TimeSelection all { start, start + length, {} };
                    for (const auto& track : song_.tracks)
                        all.trackIds.push_back(track.id);
                    changeTimeSelection(all);
                }
            }
            return;
        }

        if (scrubbing_)
        {
            scrubbing_ = false;
            if (scrubAudible_)
            {
                scrubAudible_ = false;
                if (onScrubEnded)
                    onScrubEnded();
            }
            return;
        }

        if (selectingTime_)
        {
            selectingTime_ = false;
            return;
        }

        if (! dragging_)
            return;

        const bool wasResizing      = resizing_;
        const bool wasTrimmingStart = trimmingStart_;
        const bool wasSlipping      = slipping_;
        const int  wasFadeDrag      = fadeDrag_;
        dragging_      = false;
        resizing_      = false;
        trimmingStart_ = false;
        slipping_      = false;
        fadeDrag_      = 0;

        // Only fire for an actual change — a plain click-to-select (no drag)
        // would otherwise create a harmless but noisy no-op undo step.
        if (wasFadeDrag != 0)
        {
            if (onClipFadesChanged && dragPreviewFades_ != dragOriginalFades_)
                onClipFadesChanged(dragTrackIndex_, dragClipIndex_, dragPreviewFades_);
        }
        else if (wasSlipping)
        {
            if (onClipSlipped && std::abs(dragPreviewOffset_ - dragOriginalOffset_) > 1.0e-9)
                onClipSlipped(dragTrackIndex_, dragClipIndex_, dragPreviewOffset_);
        }
        else if (wasTrimmingStart)
        {
            if (onClipStartTrimmed && std::abs(dragPreviewStart_ - dragOriginalStart_) > 1.0e-9)
                onClipStartTrimmed(dragTrackIndex_, dragClipIndex_, dragPreviewStart_);
        }
        else if (wasResizing)
        {
            if (onClipResized && std::abs(dragPreviewLength_ - dragOriginalLength_) > 1.0e-9)
                onClipResized(dragTrackIndex_, dragClipIndex_, dragPreviewLength_);
        }
        else if (dragPreviewTrackIndex_ != dragTrackIndex_)
        {
            // A track change is a real edit on its own, whether or not the
            // beat position also moved.
            if (onClipMovedToTrack)
                onClipMovedToTrack(dragTrackIndex_, dragClipIndex_, dragPreviewTrackIndex_, dragPreviewStart_);
        }
        else if (onClipMoved && std::abs(dragPreviewStart_ - dragOriginalStart_) > 1.0e-9)
        {
            onClipMoved(dragTrackIndex_, dragClipIndex_, dragPreviewStart_);
        }
        dragPreviewTrackIndex_ = -1;
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (const int marker = markerAt(e.position); marker >= 0)
        {
            if (onMarkerRenameRequested)
                onMarkerRenameRequested(marker);
            return;
        }

        // On a lane between markers: select from the marker before to the one
        // after, on that track, as double-clicking between labels does in
        // Audacity.
        const int lane = trackAtY(e.position.y);
        if (lane < 0 || e.position.x < geometry_.gutterWidth || song_.markers.empty())
            return;

        const auto [from, to] = model::spanBetweenMarkers(song_, geometry_.beatForX(e.position.x), contentEndBeats());
        if (to > from)
            changeTimeSelection({ from, to, { song_.tracks[(size_t) lane].id } });
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        updateMuteHover(muteButtonAt(e.position));
        updateGearHover(gearButtonAt(e.position));

        // Nothing else marks the ruler as draggable, so the cursor does.
        if (isOnRuler(e.position))
        {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        // The resize cursor is the only hint the clip's edge is grabbable.
        int trackIndex = -1, clipIndex = -1;
        bool onEdge = false;
        if (findClipAt(e.position, trackIndex, clipIndex))
        {
            const auto& clip = song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
            if (showEnvelopes_ && clip.type == model::ClipType::Audio)
            {
                setMouseCursor(juce::MouseCursor::CrosshairCursor);
                return;
            }
            if (fadeHandleAt(clip, trackIndex, e.position) != 0)
            {
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
                return;
            }
            onEdge = isOnClipRightEdge(clip, e.position.x) || isOnClipLeftEdge(clip, e.position.x);
        }
        setMouseCursor(onEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        updateMuteHover(-1);
        updateGearHover(-1);
    }

    /** Neither the mute toggle nor the gear icon are real child Components
        (both are painted regions, hit-tested by hand like the rest of a
        track's row) so neither can carry its own setTooltip() the way a real
        Button would — this is what TooltipWindow actually asks instead. */
    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative().toFloat();
        if (gearButtonAt(pos) >= 0)
            return "Track settings - rename, recolor, duplicate, delete";
        if (muteButtonAt(pos) >= 0)
            return "Mute this track";

        int trackIndex = -1, clipIndex = -1;
        if (findClipAt(pos, trackIndex, clipIndex)
            && fadeHandleAt(song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex], trackIndex, pos) != 0)
            return "Drag to fade - right-click the clip for fade shapes";

        if (findClipAt(pos, trackIndex, clipIndex)
            && song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].type == model::ClipType::Audio
            && ! showEnvelopes_)
            return juce::String("Drag to move - ") + (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX ? "Cmd" : "Ctrl")
                   + "-drag to slip the audio inside the clip";

        if (const int markerId = markerAt(pos); markerId >= 0)
            if (const auto* marker = model::findMarker(song_, markerId))
                return juce::String::fromUTF8(marker->name.c_str())
                       + " - drag to move, double-click to rename, right-click for more";
        return {};
    }

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        // Either kind of source in the Files pane: the directory tree, which
        // hands its selection back through the component, and the file grid,
        // which names the file in the description. Recognising only the first
        // is why dragging from the grid — where the files actually are — did
        // nothing at all.
        return dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr
            || audiofiles::fileFromDragDescription(details.description) != juce::File{};
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        fileDragActive_  = true;
        dropPreviewBeat_ = geometry_.beatForX((float) details.localPosition.x);
        repaint();
    }

    void itemDragMove(const SourceDetails& details) override
    {
        dropPreviewBeat_ = geometry_.beatForX((float) details.localPosition.x);
        repaint();
    }

    void itemDragExit(const SourceDetails&) override
    {
        fileDragActive_ = false;
        repaint();
    }

    void itemDropped(const SourceDetails& details) override
    {
        fileDragActive_ = false;
        repaint();

        auto file = audiofiles::fileFromDragDescription(details.description);

        if (file == juce::File{})
        {
            auto* fileTree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get());
            if (fileTree == nullptr || fileTree->getNumSelectedFiles() == 0)
                return;
            file = fileTree->getSelectedFile(0);
        }

        if (file != juce::File{} && onFileDropped)
            onFileDropped(file, geometry_.beatForX((float) details.localPosition.x),
                         trackIndexForY((float) details.localPosition.y));
    }

    // juce::FileDragAndDropTarget — drags from the OS, not from inside the app.
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return audiofiles::containsImportableAudio(files);
    }

    void fileDragEnter(const juce::StringArray&, int x, int /*y*/) override
    {
        fileDragActive_  = true;
        dropPreviewBeat_ = geometry_.beatForX((float) x);
        repaint();
    }

    void fileDragMove(const juce::StringArray&, int x, int /*y*/) override
    {
        dropPreviewBeat_ = geometry_.beatForX((float) x);
        repaint();
    }

    void fileDragExit(const juce::StringArray&) override
    {
        fileDragActive_ = false;
        repaint();
    }

    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        fileDragActive_ = false;
        repaint();

        if (! onFileDropped)
            return;

        const int    trackIndex = trackIndexForY((float) y);
        const double dropBeat   = geometry_.beatForX((float) x);

        // Dropping several files at once lays them end to end rather than
        // stacking them all on one beat, where they'd overlap and only the
        // first would be audible. Each lands on its own new track when the
        // drop wasn't onto an existing lane (trackIndex -1), so a multi-file
        // drop reads as "import these", not "make a mess".
        //
        // Their real lengths aren't known until each is probed by the owner,
        // so they're spaced by a fixed gap — corrected the moment the owner
        // sizes each clip to its file.
        double beat = dropBeat;
        for (const auto& file : audiofiles::importableFilesIn(files))
        {
            onFileDropped(file, beat, trackIndex);
            beat += kMultiDropSpacingBeats;
        }
    }

private:
    /** How far apart consecutive files from one multi-file drop are placed,
        when they share a track. Four beats is one bar at 4/4 — enough that
        two short clips don't overlap, and an obvious grid position to nudge
        from afterwards. */
    static constexpr double kMultiDropSpacingBeats = 4.0;

    /** The track lane @p y falls in, or -1 if it's above the first lane
        (the ruler) or below the last one. */
    int trackIndexForY(float y) const
    {
        if (y < geometry_.rulerHeight)
            return -1;
        return trackAtY(y);
    }

    /** Finds the clip under @p pos, if any (searching by lane, then by clip rect). */
    bool findClipAt(juce::Point<float> pos, int& trackIndexOut, int& clipIndexOut) const
    {
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            const float y = laneTop(i);
            if (pos.y < y || pos.y >= y + geometry_.laneHeight)
                continue;

            const auto& track = song_.tracks[(size_t) i];
            for (int c = 0; c < (int) track.clips.size(); ++c)
            {
                const auto& clip = track.clips[(size_t) c];
                const float cx   = geometry_.xForBeat(clip.startBeats);
                const float cw   = juce::jmax(2.0f, (float) clip.lengthBeats * geometry_.pixelsPerBeat());

                if (pos.x >= cx && pos.x < cx + cw)
                {
                    trackIndexOut = i;
                    clipIndexOut  = c;
                    return true;
                }
            }
        }
        return false;
    }

    /** Markers: a flag on the ruler at each one's start with its name beside
        it and a line down through the lanes; a range also gets a band across
        the ruler, a closing line, and a light shade over what it spans. */
    void paintMarkers(juce::Graphics& g, float height)
    {
        if (song_.markers.empty())
            return;

        const auto colour = juce::Colour(0xffffc233);
        g.setFont(juce::FontOptions(11.0f));

        for (const auto& marker : song_.markers)
        {
            // A marker being dragged is drawn where it would land.
            const double start = marker.id == markerDragId_ && markerDragMoved_ ? markerPreviewStart_
                                                                                : marker.startBeats;
            const float  x     = geometry_.xForBeat(start);

            if (marker.lengthBeats > 0.0)
            {
                const float right = geometry_.xForBeat(start + marker.lengthBeats);
                g.setColour(colour.withAlpha(0.18f));
                g.fillRect(x, 0.0f, right - x, geometry_.rulerHeight);
                g.setColour(colour.withAlpha(0.05f));
                g.fillRect(x, geometry_.rulerHeight, right - x, height - geometry_.rulerHeight);
                g.setColour(colour.withAlpha(0.6f));
                g.fillRect(right - 1.0f, 0.0f, 1.0f, height);
            }

            g.setColour(colour.withAlpha(0.8f));
            g.fillRect(x, 0.0f, 1.0f, height);

            juce::Path flag;
            flag.addTriangle(x, 0.0f, x + kMarkerFlagSize, 0.0f, x, kMarkerFlagSize);
            g.setColour(colour);
            g.fillPath(flag);

            if (! marker.name.empty())
                g.drawText(juce::String::fromUTF8(marker.name.c_str()), (int) x + 4,
                           (int) (geometry_.rulerHeight * 0.45f), kMarkerLabelWidth,
                           (int) (geometry_.rulerHeight * 0.55f), juce::Justification::centredLeft, true);
        }
    }

    /** The id of the marker under @p point on the ruler, or -1: a point
        marker's flag, or anywhere across a range's band. The last one drawn
        wins, since it's the one on top. */
    int markerAt(juce::Point<float> point) const
    {
        if (! isOnRuler(point))
            return -1;

        for (auto it = song_.markers.rbegin(); it != song_.markers.rend(); ++it)
        {
            const float left  = geometry_.xForBeat(it->startBeats);
            const float right = std::max(geometry_.xForBeat(it->startBeats + it->lengthBeats),
                                         left + kMarkerFlagSize);
            if (point.x >= left - kMarkerHitSlop && point.x <= right + kMarkerHitSlop)
                return it->id;
        }

        return -1;
    }

    /** The time grid's labelled (major) and unlabelled (minor) steps in
        seconds, from the format's own steps (clock, samples or frames),
        chosen from the zoom and tempo so labels never collide and lines
        never smear together. The minor step divides the major one, and is
        what clips snap to. */
    std::pair<double, double> secondsGridSteps() const
    {
        const double secondsPerBeat  = 60.0 / juce::jmax(1.0, song_.bpm);
        const double pixelsPerSecond = (double) geometry_.pixelsPerBeat() / secondsPerBeat;
        const auto   steps           = app::gridStepsFor(timeDisplay_);
        const double major = app::gridStep(steps, pixelsPerSecond, app::minLabelSpacing(timeDisplay_.format));
        return { major, app::minorStep(steps, major, pixelsPerSecond, kMinSnapSpacing) };
    }

    /** The ruler and grid when counting time rather than bars: a labelled
        line every major step and a light one every minor step. */
    void paintSecondsGrid(juce::Graphics& g, float height)
    {
        // Through the tempo map: after a tempo change a second is a different
        // number of beats.
        const auto [major, minor]   = secondsGridSteps();
        const double totalSeconds   = clock_.secondsAt(totalBeats());

        g.setColour(juce::Colours::white.withAlpha(0.07f));
        const int minorLines = (int) std::ceil(totalSeconds / minor);
        for (int i = 0; i <= minorLines; ++i)
            g.fillRect(geometry_.xForBeat(clock_.beatAt((double) i * minor)), geometry_.rulerHeight,
                       1.0f, height - geometry_.rulerHeight);

        const int majorLines = (int) std::ceil(totalSeconds / major);
        for (int i = 0; i <= majorLines; ++i)
        {
            const double seconds = (double) i * major;
            const float  x       = geometry_.xForBeat(clock_.beatAt(seconds));

            g.setColour(juce::Colours::white.withAlpha(0.16f));
            g.fillRect(x, 0.0f, 1.0f, height);
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawText(juce::String(app::gridLabel(timeDisplay_, seconds, major)), (int) x + 4, 2,
                       (int) app::minLabelSpacing(timeDisplay_.format) - 6, (int) geometry_.rulerHeight - 4,
                       juce::Justification::centredLeft);
        }
    }

    /** What a dragged clip edge or position snaps to, in beats: a whole beat
        when counting bars and beats, the minor grid step when counting
        minutes and seconds. */
    double snapUnitBeats() const
    {
        if (timeDisplay_.format == app::TimeFormat::BarsBeats)
            return 1.0;

        const double secondsPerBeat = 60.0 / juce::jmax(1.0, song_.bpm);
        return secondsGridSteps().second / secondsPerBeat;
    }

    double quartersPerBar() const
    {
        return juce::jmax(1, song_.timeSigNumerator) * 4.0 / (double) juce::jmax(1, song_.timeSigDenominator);
    }

    /** The song's musical span: at least a minimum number of bars, or further if
        any clip extends past that (plus a little trailing room to work in). */
    double totalBeats() const
    {
        const double qpb    = quartersPerBar();
        double       endBeat = kMinimumBars * qpb;

        for (const auto& track : song_.tracks)
            for (const auto& clip : track.clips)
                endBeat = std::max(endBeat, clip.startBeats + clip.lengthBeats);

        return endBeat + 4.0 * qpb;
    }

    void updateContentSize()
    {
        const int numTracks = (int) trackOfRow_.size();
        setSize((int) std::ceil(geometry_.contentWidth(totalBeats())),
                (int) std::ceil(geometry_.contentHeight(numTracks)));
    }

    static constexpr int kMinimumBars = 16;

    void updateMuteHover(int trackIndex)
    {
        if (hoveredMuteTrack_ == trackIndex)
            return;

        hoveredMuteTrack_ = trackIndex;
        repaint();
    }

    void updateGearHover(int trackIndex)
    {
        if (hoveredGearTrack_ == trackIndex)
            return;

        hoveredGearTrack_ = trackIndex;
        repaint();
    }

    WaveformCache waveforms_;
    SpectrogramCache spectrograms_;
    bool             showSpectrograms_ = false;

    /** Asks for a picture of every audio file in the song while the lanes
        show spectrograms, and forgets those no longer in it. */
    void ensureSpectrograms()
    {
        if (! showSpectrograms_)
            return;

        std::set<juce::String> files;
        for (const auto& track : song_.tracks)
            for (const auto& clip : track.clips)
                if (clip.type == model::ClipType::Audio && ! clip.audioFile.empty())
                {
                    spectrograms_.ensure(juce::File(clip.audioFile));
                    files.insert(juce::File(clip.audioFile).getFullPathName());
                }
        spectrograms_.keepOnly(files);
    }

    std::unique_ptr<juce::Drawable> unmutedIcon_, mutedIcon_, gearIcon_;
    int  hoveredMuteTrack_    = -1;
    int  hoveredGearTrack_    = -1;
    bool scrubbing_           = false;

    // Which tempo marker is being dragged, and where it has reached. The song
    // is only edited on release, so a drag in progress is drawn from here
    // rather than by rewriting the document on every mouse move.
    int  duplicateDragTrack_  = -1;    // the header being alt-dragged, or -1
    bool duplicateDragMoved_  = false; // ...and whether it has moved far enough to count

    // Far enough that a twitch during an alt-click isn't a duplicate.
    static constexpr int kDuplicateDragPixels = 8;

    TimelineGeometry geometry_;
    model::Song      song_;
    model::BeatClock clock_ { 120.0 }; // song_'s, kept with it in setSong
    double           playheadBeats_ = 0.0;

    static constexpr double kMinClipBeats     = 1.0;  // a clip shorter than a beat isn't useful
    static constexpr float  kResizeEdgePixels = 6.0f;
    static constexpr float  kFadeHandleSize   = 8.0f;

    // A marker's flag on the ruler, the room its name gets, and how far
    // outside the flag a click still lands on it.
    static constexpr float  kMarkerFlagSize   = 8.0f;
    static constexpr int    kMarkerLabelWidth = 120;
    static constexpr float  kMarkerHitSlop    = 3.0f;
    static constexpr float  kMuteSize         = 22.0f;

    // How large each glyph is *drawn*; both clickable areas stay kMuteSize.
    // Mute is the bigger of the two: it is the control, the gear is settings.
    static constexpr float  kMuteGlyphSize    = 17.0f;
    static constexpr float  kGearGlyphSize    = 12.0f;

    // Below this, beat lines are closer together than they can be told apart
    // and the grid stops being information.
    static constexpr float  kMinGridSpacing   = 6.0f;

    // The time grids: unlabelled lines, and so snap points, need this much
    // room. Labels need app::minLabelSpacing, which depends on the format.
    static constexpr float  kMinSnapSpacing       = 12.0f;

    /** How close, on screen, a dragged edge has to come to a marker, the
        playhead or another clip's edge to be pulled onto it. */
    static constexpr float kSnapMagnetPixels = 8.0f;

    /** The positions a dragged edge can snap to (see app::snapPosition), in
        beats: marker starts and ends and the playhead, and the edges of every
        clip except the one being dragged, as the snap settings allow. */
    std::vector<double> magnetsExcluding(int trackIndex, int clipIndex) const
    {
        std::vector<double> magnets;

        if (snapToMarkers_)
        {
            for (const auto& marker : song_.markers)
            {
                magnets.push_back(marker.startBeats);
                if (marker.lengthBeats > 0.0)
                    magnets.push_back(marker.startBeats + marker.lengthBeats);
            }
            magnets.push_back(playheadBeats_);
        }

        if (snapToClipEdges_)
        {
            for (int t = 0; t < (int) song_.tracks.size(); ++t)
            {
                const auto& clips = song_.tracks[(size_t) t].clips;
                for (int c = 0; c < (int) clips.size(); ++c)
                {
                    if (t == trackIndex && c == clipIndex)
                        continue;

                    magnets.push_back(clips[(size_t) c].startBeats);
                    magnets.push_back(clips[(size_t) c].startBeats + clips[(size_t) c].lengthBeats);
                }
            }
        }

        return magnets;
    }

    bool isOnClipRightEdge(const model::Clip& clip, float x) const
    {
        const float right = geometry_.xForBeat(clip.startBeats + clip.lengthBeats);
        return x >= right - kResizeEdgePixels && x <= right;
    }

    /** Only audio clips have a grabbable left edge: trimming one moves where
        its file starts playing (see trimClipStart), which a MIDI pattern has
        no equivalent of. */
    bool isOnClipLeftEdge(const model::Clip& clip, float x) const
    {
        if (clip.type != model::ClipType::Audio)
            return false;

        const float left = geometry_.xForBeat(clip.startBeats);
        return x >= left && x <= left + kResizeEdgePixels;
    }

    /** Whether a clip can be dragged from a track of type @p from onto a
        track of type @p to. Same type only, and never Audio: audio clips
        are file-backed, not a Pattern, so they don't belong here at all. */
    static bool typesAreCompatibleForClipMove(model::TrackType from, model::TrackType to) noexcept
    {
        return from == to && from != model::TrackType::Audio;
    }

    bool   scrubAudible_      = false; // a ruler drag that has moved far enough to play
    float  scrubPressX_       = 0.0f;
    static constexpr float kScrubDragPixels = 3.0f;

    bool   dragging_          = false;
    bool   resizing_          = false;
    bool   trimmingStart_     = false; // dragging an audio clip's left edge
    bool   slipping_          = false; // Ctrl-dragging an audio clip's contents inside its edges
    double dragOriginalOffset_ = 0.0;  // the clip's sourceOffsetSeconds at grab
    double dragPreviewOffset_  = 0.0;  // live preview while slipping
    int    fadeDrag_          = 0;     // +1 dragging a fade-in handle, -1 a fade-out, 0 neither
    engine::ClipFades dragOriginalFades_; // the clip's fades at grab
    engine::ClipFades dragPreviewFades_;  // live preview while dragging a fade handle
    int    dragTrackIndex_    = -1;
    int    dragClipIndex_     = -1;
    double dragGrabBeat_       = 0.0; // beat under the mouse at grab
    double dragOriginalStart_  = 0.0; // the clip's startBeats at grab
    double dragPreviewStart_   = 0.0; // live preview while dragging
    double dragOriginalLength_ = 0.0; // the clip's lengthBeats at grab
    double dragPreviewLength_  = 0.0; // live preview while resizing
    int    dragPreviewTrackIndex_ = -1; // which lane a move-drag is currently over (resize never changes this)

    int selectedTrackForEdit_ = -1;
    int selectedClipForEdit_  = -1;

    // The time selection, and the drag making one: where it started, in
    // beats (snapped) and by lane.
    model::TimeSelection timeSelection_;
    bool   selectingTime_   = false;
    double timeAnchorBeat_  = 0.0;
    int    timeAnchorTrack_ = 0;

    /** The beat under @p x, snapped the way a clip edge would be: to markers,
        the playhead and clip edges, then the grid, with @p invert (Alt)
        flipping the grid setting and dropping the rest for one drag. */
    double snappedBeatAt(float x, bool invert) const
    {
        const bool   gridOn    = snapToGrid_ != invert;
        const auto   magnets   = invert ? std::vector<double> {} : magnetsExcluding(-1, -1);
        const double tolerance = kSnapMagnetPixels / std::max(1.0e-3, (double) geometry_.pixelsPerBeat());
        return std::max(0.0, app::snapPosition(geometry_.beatForX(x), magnets, tolerance, gridOn, snapUnitBeats()));
    }

    // Razor areas, and the drags making or moving them: the areas before a
    // drag that adds more began, where it began, and how far a move has gone.
    model::RazorAreas razorAreas_;
    model::RazorAreas razorBase_;
    bool   razorDragging_    = false;
    double razorAnchorBeat_  = 0.0;
    int    razorAnchorTrack_ = 0;
    bool   razorMoving_      = false;
    bool   razorMoveMoved_   = false;
    float  razorPressX_      = 0.0f;
    double razorGrabBeat_    = 0.0;
    int    razorGrabTrack_   = 0;
    double razorDeltaBeats_  = 0.0;
    int    razorDeltaTracks_ = 0;

    // Take lanes, and a swipe being dragged along one: which track, clip and
    // take it began on, and the stretch it covers so far (snapped beats).
    bool   showTakeLanes_    = false;
    bool   swiping_          = false;
    bool   swipeMoved_       = false;
    int    swipeTrack_       = -1;
    int    swipeClip_        = -1;
    int    swipeTake_        = -1;
    float  swipePressX_      = 0.0f;
    double swipeAnchorBeat_  = 0.0;
    double swipeCurrentBeat_ = 0.0;
    static constexpr float kSwipeDragPixels = 4.0f;

    // Clip volume curves (engine/ClipEnvelope.h): whether they're shown and
    // edited, and the edit in progress — which clip, which point, and the
    // curve before and during it. The song only changes on release.
    bool                 showEnvelopes_   = false;
    bool                 envelopeEditing_ = false;
    int                  envelopeTrack_   = -1;
    int                  envelopeClip_    = -1;
    int                  envelopePoint_   = -1;
    engine::ClipEnvelope envelopeOriginal_;
    engine::ClipEnvelope envelopePreview_;
    static constexpr float kEnvelopeHitPixels = 6.0f;

    /** A clip's box on its lane, as the clip loop in paint draws it. */
    juce::Rectangle<float> clipBounds(int trackIndex, const model::Clip& clip) const
    {
        const float y = laneTop(trackIndex);
        return { geometry_.xForBeat(clip.startBeats), y + 3.0f,
                 juce::jmax(2.0f, (float) clip.lengthBeats * geometry_.pixelsPerBeat()), geometry_.laneHeight - 6.0f };
    }

    /** Where a mouse position falls on @p clip's curve: seconds into its
        file, kept to what it plays, and the gain its height means. */
    std::pair<double, float> envelopePositionAt(int trackIndex, const model::Clip& clip, juce::Point<float> point) const
    {
        const auto   box     = clipBounds(trackIndex, clip);
        const double seconds = app::clampToClipSource(
            clip, app::sourceSecondsAtBeat(clip, geometry_.beatForX(point.x), clock_), clock_);
        return { seconds, app::gainForY(point.y, box.getY(), box.getHeight()) };
    }

    /** Starts editing @p clip's curve at a press: Alt on a point removes it
        straight away; on a point picks it up; anywhere else adds one there
        and picks that up. */
    void beginEnvelopeEdit(int trackIndex, int clipIndex, const juce::MouseEvent& e)
    {
        const auto& clip           = song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
        const auto [seconds, gain] = envelopePositionAt(trackIndex, clip, e.position);

        envelopeTrack_    = trackIndex;
        envelopeClip_     = clipIndex;
        envelopeOriginal_ = clip.envelope;
        envelopePreview_  = clip.envelope;

        const double pixelsPerSecond = (double) pixelsPerSecondAt(clip.startBeats);
        const int    near = envelopePreview_.indexNear(seconds, kEnvelopeHitPixels / juce::jmax(1.0e-6, pixelsPerSecond));

        if (near >= 0 && e.mods.isAltDown())
        {
            envelopePreview_.removePointAt(near);
            repaint();
            if (onClipEnvelopeChanged)
                onClipEnvelopeChanged(trackIndex, clipIndex, envelopePreview_);
            return;
        }

        envelopePoint_   = near >= 0 ? near : envelopePreview_.addPoint(seconds, gain);
        envelopeEditing_ = true;
        repaint();
    }

    /** A clip's volume curve over it: faint unity line, the curve as
        playback follows it (from the same gainAt), and a handle at each
        point. */
    void paintEnvelope(juce::Graphics& g, const engine::ClipEnvelope& envelope, const model::Clip& clip,
                       juce::Rectangle<float> box)
    {
        if (box.getWidth() < 4.0f || box.getHeight() < 8.0f)
            return;

        juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(box.toNearestInt());

        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.drawHorizontalLine((int) app::yForGain(1.0f, box.getY(), box.getHeight()), box.getX(), box.getRight());

        juce::Path curve;
        bool       started = false;
        for (float x = box.getX(); x <= box.getRight(); x += 2.0f)
        {
            const double seconds = app::sourceSecondsAtBeat(clip, geometry_.beatForX(x), clock_);
            const float  y       = app::yForGain(envelope.gainAt(seconds), box.getY(), box.getHeight());
            if (! started)
            {
                curve.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                curve.lineTo(x, y);
            }
        }

        g.setColour(juce::Colours::yellow.withAlpha(0.9f));
        g.strokePath(curve, juce::PathStrokeType(1.5f));

        for (const auto& point : envelope.points())
        {
            const float x = geometry_.xForBeat(app::beatAtSourceSeconds(clip, point.seconds, clock_));
            const float y = app::yForGain(point.gain, box.getY(), box.getHeight());
            g.fillRect(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }

    // A marker being dragged along the ruler: which, whether it has moved far
    // enough to be a drag, and where it started and is now.
    int    markerDragId_        = -1;
    bool   markerDragMoved_     = false;
    float  markerPressX_        = 0.0f;
    double markerGrabBeat_      = 0.0;
    double markerOriginalStart_ = 0.0;
    double markerPreviewStart_  = 0.0;
    static constexpr float kMarkerDragPixels = 3.0f;

    /** Where the last clip ends, or the whole timeline if there are none. */
    double contentEndBeats() const
    {
        double end = 0.0;
        for (const auto& track : song_.tracks)
            for (const auto& clip : track.clips)
                end = std::max(end, clip.startBeats + clip.lengthBeats);
        return end > 0.0 ? end : totalBeats();
    }

    /** The track for @p y, running past the last lane rather than
        stopping (and before the first, below 0), so a drag below the tracks
        keeps the bottom one selected. */
    int trackAtYUnclamped(float y) const
    {
        const int row  = (int) std::floor((y - geometry_.rulerHeight) / geometry_.laneHeight);
        const int rows = (int) trackOfRow_.size();
        if (row < 0 || rows == 0)
            return row;
        if (row < rows)
            return trackOfRow_[(size_t) row];
        return trackOfRow_.back() + (row - rows + 1);
    }

    void beginTimeSelection(const juce::MouseEvent& e)
    {
        selectingTime_   = true;
        timeAnchorBeat_  = snappedBeatAt(e.position.x, e.mods.isAltDown());
        timeAnchorTrack_ = trackAtY(e.position.y);
        changeTimeSelection(model::selectionFromDrag(song_, timeAnchorBeat_, timeAnchorBeat_,
                                                     timeAnchorTrack_, timeAnchorTrack_));
    }

    void changeTimeSelection(const model::TimeSelection& selection)
    {
        if (selection == timeSelection_)
            return;

        timeSelection_ = selection;
        repaint();

        if (onTimeSelectionChanged)
            onTimeSelectionChanged(timeSelection_);
    }

    /** Each tempo change after the start: a flag along the bottom of the
        ruler with its tempo, a faint line down through the lanes, and for a
        ramp, a slope back to the change before, the span it slides across. */
    void paintTempoChanges(juce::Graphics& g, float height)
    {
        const float top = geometry_.rulerHeight * 0.55f;
        for (int i = 0; i < (int) song_.tempoChanges.size(); ++i)
        {
            const auto&  change = song_.tempoChanges[(size_t) i];
            const double beat   = i == draggingTempo_ ? tempoDragToBeat_ : change.beat;
            const float  x      = geometry_.xForBeat(beat);
            if (x < geometry_.gutterWidth - 2.0f || x > (float) getWidth())
                continue;

            g.setColour(juce::Colours::orange.withAlpha(0.22f));
            g.fillRect(x, geometry_.rulerHeight, 1.0f, height - geometry_.rulerHeight);
            g.setColour(juce::Colours::orange.withAlpha(0.9f));
            g.fillRect(x, top, 2.0f, geometry_.rulerHeight - top);

            if (change.ramp)
            {
                const double previous = i > 0 ? song_.tempoChanges[(size_t) i - 1].beat : 0.0;
                g.setColour(juce::Colours::orange.withAlpha(0.55f));
                g.drawLine(geometry_.xForBeat(previous), geometry_.rulerHeight - 1.0f, x, top, 1.5f);
            }

            g.setColour(juce::Colours::orange.withAlpha(0.95f));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(juce::String(change.bpm, change.bpm == std::round(change.bpm) ? 0 : 1),
                       (int) x + 4, (int) top - 1, 48, (int) (geometry_.rulerHeight - top) + 1,
                       juce::Justification::centredLeft);
        }
    }

    /** The tempo change whose flag is under @p point on the ruler, or -1.
        Generous, since the flag is two pixels wide. */
    int tempoChangeAt(juce::Point<float> point) const
    {
        if (point.y < geometry_.rulerHeight * 0.5f || point.y >= geometry_.rulerHeight)
            return -1;
        for (int i = 0; i < (int) song_.tempoChanges.size(); ++i)
            if (std::abs(geometry_.xForBeat(song_.tempoChanges[(size_t) i].beat) - point.x) <= 5.0f)
                return i;
        return -1;
    }

    /** Add a tempo change at the bar clicked, or edit, ramp or remove the
        one there. */
    void showTempoMenu(float x)
    {
        int existing = -1;
        for (int i = 0; i < (int) song_.tempoChanges.size(); ++i)
            if (std::abs(geometry_.xForBeat(song_.tempoChanges[(size_t) i].beat) - x) <= 5.0f)
                existing = i;

        const double qpb     = quartersPerBar();
        const double barBeat = std::max(qpb, std::round(geometry_.beatForX(x) / qpb) * qpb);

        juce::PopupMenu menu;
        if (existing >= 0)
        {
            menu.addItem(1, "Edit Tempo Change...");
            menu.addItem(3, "Slide to This Tempo (Ramp)", true, song_.tempoChanges[(size_t) existing].ramp);
            menu.addSeparator();
            menu.addItem(2, "Remove Tempo Change");
        }
        else
            menu.addItem(1, "Add Tempo Change at Bar " + juce::String((int) std::round(barBeat / qpb) + 1) + "...");

        const double beat = existing >= 0 ? song_.tempoChanges[(size_t) existing].beat : barBeat;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [self = juce::Component::SafePointer<ArrangementView>(this), beat](int result)
        {
            if (self == nullptr)
                return;
            if (result == 1 && self->onTempoChangeRequested)
                self->onTempoChangeRequested(beat);
            else if (result == 2 && self->onTempoChangeRemoved)
                self->onTempoChangeRemoved(beat);
            else if (result == 3 && self->onTempoRampToggled)
                self->onTempoRampToggled(beat);
        });
    }

    int    draggingTempo_     = -1;
    double tempoDragFromBeat_ = 0.0;
    double tempoDragToBeat_   = 0.0;

    void changeRazorAreas(const model::RazorAreas& areas)
    {
        if (areas == razorAreas_)
            return;

        razorAreas_ = areas;
        repaint();

        if (onRazorAreasChanged)
            onRazorAreasChanged(razorAreas_);
    }

    /** Each razor area shaded over its lane, and, while they're being
        dragged, an outline of each where it would land. */
    void paintRazorAreas(juce::Graphics& g)
    {
        const auto paintArea = [&](const model::RazorArea& area, int lane, bool ghost)
        {
            if (lane < 0 || lane >= (int) song_.tracks.size())
                return;

            const float left  = geometry_.xForBeat(area.startBeats);
            const float right = geometry_.xForBeat(area.endBeats);
            const auto  box   = juce::Rectangle<float>(left, laneTop(lane), juce::jmax(1.0f, right - left),
                                                       geometry_.laneHeight);
            if (! ghost)
            {
                g.setColour(juce::Colour(0xffff8c42).withAlpha(0.28f));
                g.fillRect(box);
            }
            g.setColour(juce::Colour(0xffff8c42).withAlpha(ghost ? 0.95f : 0.8f));
            g.drawRect(box, ghost ? 2.0f : 1.0f);
        };

        for (const auto& area : razorAreas_)
        {
            const int lane = model::razoredit::trackIndexOf(song_, area.trackId);
            paintArea(area, lane, false);

            if (razorMoving_ && razorMoveMoved_)
                paintArea({ area.trackId, area.startBeats + razorDeltaBeats_, area.endBeats + razorDeltaBeats_ },
                          lane + razorDeltaTracks_, true);
        }
    }

    /** Shaded across each selected lane, or a line on each while it's only a
        cursor. Over the clips, since what's selected is what's in them. */
    void paintTimeSelection(juce::Graphics& g)
    {
        if (! timeSelection_.hasTracks())
            return;

        const float left  = geometry_.xForBeat(timeSelection_.startBeats);
        const float right = geometry_.xForBeat(timeSelection_.endBeats);

        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            if (! timeSelection_.includes(song_.tracks[(size_t) i].id) || rowOfTrack_[(size_t) i] < 0)
                continue;

            const float y = laneTop(i);

            if (right - left >= 1.0f)
            {
                g.setColour(juce::Colours::white.withAlpha(0.18f));
                g.fillRect(left, y, right - left, geometry_.laneHeight);
                g.setColour(juce::Colours::white.withAlpha(0.55f));
                g.fillRect(left, y, 1.0f, geometry_.laneHeight);
                g.fillRect(right - 1.0f, y, 1.0f, geometry_.laneHeight);
            }
            else
            {
                g.setColour(juce::Colours::white.withAlpha(0.8f));
                g.fillRect(left, y, 1.0f, geometry_.laneHeight);
            }
        }
    }

    bool   snapToGrid_      = true;
    bool   snapToMarkers_   = true;
    bool   snapToClipEdges_ = true;
    app::TimeDisplay timeDisplay_;
    bool   fileDragActive_  = false;
    double dropPreviewBeat_ = 0.0;
};

} // namespace soundsplice
