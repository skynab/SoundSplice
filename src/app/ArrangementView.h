#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Song.h"

#include "AudioFileTypes.h"
#include "ClipPreview.h"
#include "Icons.h"
#include "WaveformCache.h"
#include "TrackColours.h"
#include "TimelineGeometry.h"

namespace looper
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

        // The gutter carries a colour stripe, a type tag, the name, and two
        // buttons. 110px fitted a name alone.
        geometry_.gutterWidth = 168.0f;
    }

    std::function<void(double)> onSeek; // beat position clicked
    std::function<void(int trackIndex, int clipIndex, double newStartBeats)> onClipMoved;
    std::function<void(int trackIndex, int clipIndex, double newLengthBeats)> onClipResized;

    /** Fired instead of onClipMoved when a move-drag ends on a *different*
        track than it started on (see typesAreCompatibleForClipMove — the
        drag preview never lands on an incompatible track in the first
        place, so this always names a real, allowed move). Fires even if the
        beat position didn't also change: changing track is itself an edit. */
    std::function<void(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats)> onClipMovedToTrack;
    std::function<void(int trackIndex, int clipIndex)> onClipSelected; // fired on press, before any drag

    /** Fired when a track's mute button in the gutter is clicked. The view
        doesn't change the document itself — the owner does, and the change
        comes back through setSong. */
    std::function<void(int trackIndex)> onTrackMuteToggled;

    /** Fired when a track's gear button is clicked. The owner shows the menu:
        it knows what the options do, and the view doesn't. */
    std::function<void(int trackIndex)> onTrackSettingsRequested;

    /** Fired when a track's header is alt-dragged: duplicate that track. */
    std::function<void(int trackIndex)> onTrackDuplicateRequested;
    std::function<void(const juce::File& file, double dropBeat, int trackIndex)> onFileDropped;

    /** Tempo editing on the ruler. The view never edits the song itself — it
        says what was asked for and the owner does it through history_, which
        is what puts a tempo change in undo alongside every other edit. */
    std::function<void(double beat)> onTempoChangeRequested; // add or edit at this beat
    std::function<void(double beat)> onTempoChangeRemoved;
    std::function<void(double fromBeat, double toBeat)> onTempoChangeMoved;
    std::function<void(double beat)> onTempoRampToggled;

    void setSong(const model::Song& song)
    {
        song_ = song;

        // Thumbnails are made here rather than in paint: creating one starts a
        // file scan and allocates, neither of which belongs on a path that
        // runs for every scroll and playhead tick.
        for (const auto& track : song_.tracks)
            for (const auto& clip : track.clips)
                if (clip.type == model::ClipType::Audio && ! clip.audioFile.empty())
                    waveforms_.ensure(juce::File(clip.audioFile));

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

    void setSelectedClip(int trackIndex, int clipIndex)
    {
        if (selectedTrackForEdit_ != trackIndex || selectedClipForEdit_ != clipIndex)
        {
            selectedTrackForEdit_ = trackIndex;
            selectedClipForEdit_  = clipIndex;
            repaint();
        }
    }

    /** Tempo changes, drawn along the bottom of the ruler.

        A marker is drawn for every change *after* the start, not for the
        starting tempo: beat 0's tempo is what the transport's slider shows, and
        a marker there would suggest it could be dragged away from the start,
        which it cannot. */
    void paintTempoMarkers(juce::Graphics& g, float height)
    {
        if (song_.tempoChanges.empty())
            return;

        const float markerTop = geometry_.rulerHeight * 0.5f;

        for (int i = 0; i < (int) song_.tempoChanges.size(); ++i)
        {
            const auto&  change = song_.tempoChanges[(size_t) i];
            const double beat   = i == draggingTempo_ ? tempoDragToBeat_ : change.beat;
            const float  x      = geometry_.xForBeat(beat);
            if (x < geometry_.gutterWidth - 2.0f || x > (float) getWidth())
                continue;

            // A full-height line as well as the flag: a tempo change is a
            // property of the timeline, not of the ruler, and the clips it
            // affects are below.
            g.setColour(juce::Colours::orange.withAlpha(0.25f));
            g.fillRect(x, geometry_.rulerHeight, 1.0f, height - geometry_.rulerHeight);

            g.setColour(juce::Colours::orange.withAlpha(0.9f));
            g.fillRect(x, markerTop, 2.0f, geometry_.rulerHeight - markerTop);

            // A ramp is drawn as a slope running back to the previous change,
            // because that is the span it actually covers — a marker alone
            // would say the tempo arrives here without saying it has been
            // moving the whole way.
            if (change.ramp)
            {
                const double previousBeat = i > 0 ? song_.tempoChanges[(size_t) i - 1].beat : 0.0;
                const float  fromX        = geometry_.xForBeat(previousBeat);

                g.setColour(juce::Colours::orange.withAlpha(0.55f));
                g.drawLine(fromX, geometry_.rulerHeight - 1.0f, x, markerTop, 1.5f);
            }

            g.setFont(juce::FontOptions(10.0f));
            g.drawText(juce::String(change.bpm, 0),
                       (int) x + 4, (int) markerTop, 44,
                       (int) (geometry_.rulerHeight - markerTop),
                       juce::Justification::centredLeft);
        }
    }

    /** Add, edit or remove a tempo change at the clicked position.

        Snapped to the bar it was clicked in: a tempo change on an off-beat is
        almost never what someone means, and a marker a fraction of a beat away
        from the bar line reads as a mistake even when it was deliberate. */
    void showTempoMenu(float x)
    {
        const int    existing = tempoChangeAt(x);
        const double qpb      = quartersPerBar();
        const double rawBeat  = geometry_.beatForX(x);
        const double barBeat  = std::max(0.0, std::round(rawBeat / qpb) * qpb);

        juce::PopupMenu menu;

        if (existing >= 0)
        {
            const bool ramped = song_.tempoChanges[(size_t) existing].ramp;

            menu.addItem(1, "Edit tempo here...");
            menu.addItem(3, ramped ? "Jump to this tempo" : "Slide to this tempo (ramp)");
            menu.addSeparator();
            menu.addItem(2, "Remove tempo change");
        }
        else
        {
            menu.addItem(1, "Add tempo change at bar "
                             + juce::String((int) std::round(barBeat / qpb) + 1) + "...");
        }

        const double beat = existing >= 0 ? song_.tempoChanges[(size_t) existing].beat : barBeat;

        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this),
                           [self = juce::Component::SafePointer<ArrangementView>(this), beat](int result)
        {
            if (self == nullptr || result == 0)
                return;

            if (result == 1 && self->onTempoChangeRequested)
                self->onTempoChangeRequested(beat);
            else if (result == 2 && self->onTempoChangeRemoved)
                self->onTempoChangeRemoved(beat);
            else if (result == 3 && self->onTempoRampToggled)
                self->onTempoRampToggled(beat);
        });
    }

    /** The tempo change under @p x on the ruler, or -1. Hit width is generous
        because the marker is two pixels wide and nobody can click that. */
    int tempoChangeAt(float x) const
    {
        for (int i = 0; i < (int) song_.tempoChanges.size(); ++i)
            if (std::abs(geometry_.xForBeat(song_.tempoChanges[(size_t) i].beat) - x) <= 5.0f)
                return i;

        return -1;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));

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
        // Beat lines inside each bar, so a bar reads as its beats rather than
        // as one undivided box — in 4/4 that is four subdivisions per bar,
        // and it follows the time signature rather than assuming four.
        // Dropped when they'd be closer together than this, since a grid too
        // fine to resolve is just a lighter background.
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

        paintTempoMarkers(g, height);

        // Track lanes + clips.
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            const auto& track = song_.tracks[(size_t) i];
            const float y     = geometry_.rulerHeight + (float) i * geometry_.laneHeight;

            if (i % 2 == 0)
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
            // someone renames one — call a guitar track "Verse" and nothing
            // would say it was a guitar any more. This is what makes renaming
            // free.
            const auto tagArea = juce::Rectangle<float>(10.0f, y + geometry_.laneHeight * 0.5f - 8.0f,
                                                        32.0f, 16.0f);
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
                g.setColour(juce::Colours::cyan.withAlpha(0.18f));
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

                paintClipContents(g, clip, r);

                // A grip along the right edge, so the resize handle is
                // visible rather than only discoverable by hovering.
                if (r.getWidth() > 3.0f * kResizeEdgePixels)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.18f));
                    g.fillRect(r.getRight() - kResizeEdgePixels, r.getY() + 2.0f,
                               kResizeEdgePixels - 1.0f, r.getHeight() - 4.0f);
                }

                g.setColour(isEditSelected ? juce::Colours::cyan.withAlpha(0.9f) : juce::Colours::black.withAlpha(0.3f));
                g.drawRoundedRectangle(r, 3.0f, isEditSelected ? 2.0f : 1.0f);
            }

            // An empty lane otherwise looks identical to a broken one —
            // EffectChainPanel and SessionView already say so when they're
            // empty; a track with nothing arranged deserves the same.
            if (track.clips.empty())
            {
                g.setColour(juce::Colours::white.withAlpha(track.muted ? 0.2f : 0.35f));
                g.setFont(juce::FontOptions(12.0f));
                g.drawText("No clips - select this track and click + Clip to add one",
                           juce::Rectangle<float>(timelineX + 8.0f, y, width - timelineX - 16.0f,
                                                  geometry_.laneHeight),
                           juce::Justification::centredLeft);
            }
        }

        // The dragged clip's ghost, drawn once here rather than inline in
        // the loop above: a resize always stays on dragTrackIndex_'s lane, a
        // move follows dragPreviewTrackIndex_ — which may be a different
        // lane than the clip's own, mid cross-track drag. Kept in the
        // source track's colour throughout, even while hovering a different
        // lane: it hasn't landed there yet, and recolouring it would read as
        // "this already belongs to that track."
        if (dragging_ && dragTrackIndex_ >= 0 && dragTrackIndex_ < (int) song_.tracks.size())
        {
            const int ghostRow = resizing_ ? dragTrackIndex_ : dragPreviewTrackIndex_;
            if (ghostRow >= 0 && ghostRow < (int) song_.tracks.size())
            {
                const float ghostY = geometry_.rulerHeight + (float) ghostRow * geometry_.laneHeight;
                const float cx     = geometry_.xForBeat(dragPreviewStart_);
                const float cw     = juce::jmax(2.0f, (float) dragPreviewLength_ * ppb);
                const juce::Rectangle<float> r(cx, ghostY + 3.0f, cw, geometry_.laneHeight - 6.0f);

                auto ghostColour = trackColour(song_.tracks[(size_t) dragTrackIndex_].colour).brighter(0.3f);
                g.setColour(ghostColour);
                g.fillRoundedRectangle(r, 3.0f);

                if (dragClipIndex_ >= 0 && dragClipIndex_ < (int) song_.tracks[(size_t) dragTrackIndex_].clips.size())
                    paintClipContents(g, song_.tracks[(size_t) dragTrackIndex_].clips[(size_t) dragClipIndex_], r);

                if (r.getWidth() > 3.0f * kResizeEdgePixels)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.18f));
                    g.fillRect(r.getRight() - kResizeEdgePixels, r.getY() + 2.0f,
                               kResizeEdgePixels - 1.0f, r.getHeight() - 4.0f);
                }

                g.setColour(juce::Colours::cyan.withAlpha(0.9f));
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
            g.setColour(juce::Colours::orange.withAlpha(0.9f));
            g.fillRect(px, 0.0f, 2.0f, height);
        }

        // Drop preview: a file is being dragged over the timeline.
        if (fileDragActive_)
        {
            const float dx = geometry_.xForBeat(dropPreviewBeat_);
            g.setColour(juce::Colours::cyan.withAlpha(0.5f));
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
                                juce::Rectangle<float> bounds)
    {
        if (clip.audioFile.empty() || bounds.getWidth() < 8.0f || bounds.getHeight() < 8.0f)
            return;

        auto* thumbnail = waveforms_.find(juce::File(clip.audioFile));
        if (thumbnail == nullptr || thumbnail->getTotalLength() <= 0.0)
            return;

        const double secondsPerBeat = 60.0 / juce::jmax(1.0, song_.bpm);
        const double fraction       = audioClipDrawnFraction(thumbnail->getTotalLength(),
                                                             clip.lengthBeats, secondsPerBeat);
        const double seconds        = audioClipAudibleSeconds(thumbnail->getTotalLength(),
                                                              clip.lengthBeats, secondsPerBeat);
        if (fraction <= 0.0 || seconds <= 0.0)
            return;

        auto area = bounds.reduced(2.0f, 3.0f);
        area.setWidth((float) (area.getWidth() * fraction));
        if (area.getWidth() < 1.0f || area.getHeight() < 1.0f)
            return;

        // Scaled by the clip's own gain, so a normalised or turned-down clip
        // looks different on the timeline too. Drawing at a fixed 1.0 meant
        // the level was visible in the audio editor and invisible here, which
        // is worse than showing it nowhere: two views of the same clip
        // disagreeing reads as one of them being wrong.
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        thumbnail->drawChannels(g, area.toNearestInt(), 0.0, seconds,
                                juce::Decibels::decibelsToGain(clip.gainDb));
    }

    /** Where a track's mute button sits, in this component's coordinates.

        One definition used by both the painting and the click handling. Worked
        out separately they drift, and the result is a control drawn in one
        place that responds in another — which looks exactly like a button
        that doesn't work. */
    juce::Rectangle<float> muteButtonBounds(int trackIndex) const
    {
        const float top = geometry_.rulerHeight + geometry_.laneHeight * (float) trackIndex;
        return juce::Rectangle<float>(geometry_.gutterWidth - 2.0f * kMuteSize - 10.0f,
                                      top + (geometry_.laneHeight - kMuteSize) * 0.5f,
                                      kMuteSize, kMuteSize);
    }

    /** The lane a y-coordinate falls in, or -1 above or below them all. */
    int trackAtY(float y) const
    {
        if (y < geometry_.rulerHeight)
            return -1;

        const int index = (int) ((y - geometry_.rulerHeight) / geometry_.laneHeight);
        return index >= 0 && index < (int) song_.tracks.size() ? index : -1;
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
    void paintClipContents(juce::Graphics& g, const model::Clip& clip,
                           juce::Rectangle<float> bounds)
    {
        if (clip.type == model::ClipType::Audio)
        {
            paintAudioClipContents(g, clip, bounds);
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
            // Right-click edits the tempo map; left-click still scrubs, so the
            // ruler's primary gesture is unchanged.
            if (e.mods.isPopupMenu())
            {
                showTempoMenu(e.position.x);
                return;
            }

            // Pressing a marker drags it; pressing anywhere else on the ruler
            // scrubs. Checked first because the marker sits on top of the
            // scrub area, so the two would otherwise compete for the same
            // press and scrubbing would always win.
            const int marker = tempoChangeAt(e.position.x);
            if (marker >= 0)
            {
                draggingTempo_     = marker;
                tempoDragFromBeat_ = song_.tempoChanges[(size_t) marker].beat;
                tempoDragToBeat_   = tempoDragFromBeat_;
                return;
            }

            scrubbing_ = true;
            scrubTo(e.position.x);
            return;
        }

        int trackIndex = -1, clipIndex = -1;
        if (findClipAt(e.position, trackIndex, clipIndex))
        {
            const auto& clip = song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];

            dragging_           = true;
            resizing_           = isOnClipRightEdge(clip, e.position.x);
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

        if (onSeek)
            onSeek(geometry_.beatForX(e.position.x));
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (duplicateDragTrack_ >= 0)
        {
            if (! duplicateDragMoved_ && e.getDistanceFromDragStart() >= kDuplicateDragPixels)
            {
                duplicateDragMoved_ = true;
                repaint();
            }
            return;
        }

        if (draggingTempo_ >= 0)
        {
            // Snapped to the bar, like adding one: a tempo change a fraction of
            // a beat off the bar line reads as a mistake even when deliberate.
            const double qpb = quartersPerBar();
            tempoDragToBeat_ = std::max(qpb, std::round(geometry_.beatForX(e.position.x) / qpb) * qpb);
            repaint();
            return;
        }

        if (scrubbing_)
        {
            // Deliberately not restricted to the ruler: once the press has
            // started, dragging down into the lanes or off the edge should
            // keep scrubbing rather than stopping the moment the mouse
            // strays, which is how every transport scrub bar behaves.
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
        const bool snap = snapToGrid_ != e.mods.isAltDown();

        if (resizing_)
        {
            dragPreviewLength_ = std::max(kMinClipBeats,
                                          maybeSnap(currentBeat - dragPreviewStart_, snap, kMinClipBeats));
        }
        else
        {
            dragPreviewStart_ = std::max(0.0, maybeSnap(dragOriginalStart_ + (currentBeat - dragGrabBeat_),
                                                        snap, 0.0));

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

        if (draggingTempo_ >= 0)
        {
            const double from = tempoDragFromBeat_;
            const double to   = tempoDragToBeat_;

            draggingTempo_ = -1;
            repaint();

            // Only for an actual move: a plain click on a marker would
            // otherwise push a no-op onto the undo stack.
            if (std::abs(to - from) > 1.0e-9 && onTempoChangeMoved)
                onTempoChangeMoved(from, to);

            return;
        }

        if (scrubbing_)
        {
            scrubbing_ = false;
            return;
        }

        if (! dragging_)
            return;

        const bool wasResizing = resizing_;
        dragging_ = false;
        resizing_ = false;

        // Only fire for an actual change — a plain click-to-select (no drag)
        // would otherwise create a harmless but noisy no-op undo step.
        if (wasResizing)
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
        const bool onEdge = findClipAt(e.position, trackIndex, clipIndex)
                         && isOnClipRightEdge(song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex],
                                              e.position.x);
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
        const int idx = (int) ((y - geometry_.rulerHeight) / geometry_.laneHeight);
        return (idx >= 0 && idx < (int) song_.tracks.size()) ? idx : -1;
    }

    /** Finds the clip under @p pos, if any (searching by lane, then by clip rect). */
    bool findClipAt(juce::Point<float> pos, int& trackIndexOut, int& clipIndexOut) const
    {
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            const float y = geometry_.rulerHeight + (float) i * geometry_.laneHeight;
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
        const int numTracks = (int) song_.tracks.size();
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

    std::unique_ptr<juce::Drawable> unmutedIcon_, mutedIcon_, gearIcon_;
    int  hoveredMuteTrack_    = -1;
    int  hoveredGearTrack_    = -1;
    bool scrubbing_           = false;

    // Which tempo marker is being dragged, and where it has reached. The song
    // is only edited on release, so a drag in progress is drawn from here
    // rather than by rewriting the document on every mouse move.
    int    draggingTempo_     = -1;
    double tempoDragFromBeat_ = 0.0;
    double tempoDragToBeat_   = 0.0;
    int  duplicateDragTrack_  = -1;    // the header being alt-dragged, or -1
    bool duplicateDragMoved_  = false; // ...and whether it has moved far enough to count

    // Far enough that a twitch during an alt-click isn't a duplicate.
    static constexpr int kDuplicateDragPixels = 8;

    TimelineGeometry geometry_;
    model::Song      song_;
    double           playheadBeats_ = 0.0;

    static constexpr double kMinClipBeats     = 1.0;  // a clip shorter than a beat isn't useful
    static constexpr float  kResizeEdgePixels = 6.0f;
    static constexpr float  kMuteSize         = 22.0f;

    // How large each glyph is *drawn*; both clickable areas stay kMuteSize.
    // Mute is the bigger of the two: it is the control, the gear is settings.
    static constexpr float  kMuteGlyphSize    = 17.0f;
    static constexpr float  kGearGlyphSize    = 12.0f;

    // Below this, beat lines are closer together than they can be told apart
    // and the grid stops being information.
    static constexpr float  kMinGridSpacing   = 6.0f;

    /** Rounds to whole beats when snapping is on, with a floor so a snapped
        value can't collapse below its minimum. */
    static double maybeSnap(double beats, bool snap, double minimum)
    {
        const double snapped = snap ? std::round(beats) : beats;
        return std::max(minimum, snapped);
    }

    bool isOnClipRightEdge(const model::Clip& clip, float x) const
    {
        const float right = geometry_.xForBeat(clip.startBeats + clip.lengthBeats);
        return x >= right - kResizeEdgePixels && x <= right;
    }

    /** Whether a clip can be dragged from a track of type @p from onto a
        track of type @p to. Same type only, and never Audio: Instrument/
        Drum/Guitar tracks all store the same Clip/Pattern data (the same
        boundary MainComponent::setTrackType already draws), but a drag is a
        fast, low-friction gesture — unlike setTrackType, a deliberate
        whole-track decision — so it doesn't also take on "maybe reinterpret
        this clip's note numbers as a different instrument," which cross-type
        would mean. Audio clips are file-backed, not a Pattern, so they don't
        belong here at all. */
    static bool typesAreCompatibleForClipMove(model::TrackType from, model::TrackType to) noexcept
    {
        return from == to && from != model::TrackType::Audio;
    }

    bool   dragging_          = false;
    bool   resizing_          = false;
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

    bool   snapToGrid_      = true;
    bool   fileDragActive_  = false;
    double dropPreviewBeat_ = 0.0;
};

} // namespace looper
