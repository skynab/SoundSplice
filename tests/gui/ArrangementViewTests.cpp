#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ArrangementView.h>
#include <app/TrackColours.h>

#include <algorithm>
#include <string>

using namespace looper;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };
}

TEST_CASE("Timeline zoom stays inside its range", "[gui][arrangement]")
{
    JuceFixture fixture;
    ArrangementView view;

    view.setZoom(1000.0f);
    REQUIRE(view.zoom() == ArrangementView::kMaxZoom);

    view.setZoom(0.0f);
    REQUIRE(view.zoom() == ArrangementView::kMinZoom);
}

TEST_CASE("The zoom buttons know when they would do nothing", "[gui][arrangement]")
{
    // This is what greys them out. A button that silently does nothing at the
    // limit is indistinguishable from one that's broken — which is roughly
    // what "I don't understand what these are doing" describes.
    JuceFixture fixture;
    ArrangementView view;

    view.setZoom(ArrangementView::kMaxZoom);
    REQUIRE_FALSE(view.canZoomIn());
    REQUIRE(view.canZoomOut());

    view.setZoom(ArrangementView::kMinZoom);
    REQUIRE(view.canZoomIn());
    REQUIRE_FALSE(view.canZoomOut());

    view.setZoom(1.0f);
    REQUIRE(view.canZoomIn());
    REQUIRE(view.canZoomOut());
}

TEST_CASE("Zooming changes how much timeline a pixel covers", "[gui][arrangement]")
{
    // The whole point of the control: if this didn't change, the buttons
    // really would be doing nothing.
    JuceFixture fixture;
    ArrangementView view;
    view.setSize(800, 400);

    view.setZoom(1.0f);
    const double beatAtMiddleNormal = view.beatForXForTesting(400.0f);

    view.setZoom(2.0f);
    const double beatAtMiddleZoomed = view.beatForXForTesting(400.0f);

    REQUIRE(beatAtMiddleZoomed < beatAtMiddleNormal);
}

namespace
{
    /** A song with @p count tracks, each holding one clip. */
    model::Song songWithTracks(int count)
    {
        model::Song song;
        for (int i = 0; i < count; ++i)
        {
            const int id = model::addTrack(song, model::TrackType::Instrument,
                                           "Track " + std::to_string(i + 1)).id;
            model::Clip clip;
            clip.type                = model::ClipType::Instrument;
            clip.lengthBeats         = 4.0;
            clip.pattern.lengthBeats = 4.0;
            model::addClip(song, id, clip);
        }
        return song;
    }

    std::unique_ptr<ArrangementView> viewWith(int trackCount)
    {
        auto view = std::make_unique<ArrangementView>();
        view->setVisible(true);
        view->setSize(900, 500);
        view->setSong(songWithTracks(trackCount));
        return view;
    }
}

TEST_CASE("Every track gets a mute button, inside the gutter", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto bounds = view->muteButtonBoundsForTesting(i);
        INFO("track " << i << " mute at " << bounds.toString());

        REQUIRE(bounds.getWidth() > 0.0f);
        REQUIRE(bounds.getHeight() > 0.0f);
        REQUIRE(bounds.getRight() <= view->gutterWidthForTesting()); // clear of the timeline
        REQUIRE(bounds.getX() >= 0.0f);
    }
}

TEST_CASE("Mute buttons don't overlap each other", "[gui][arrangement]")
{
    // One per lane: overlapping ones would mute the wrong track at the edges.
    JuceFixture fixture;
    auto view = viewWith(5);

    for (int i = 1; i < 5; ++i)
    {
        const auto above = view->muteButtonBoundsForTesting(i - 1);
        const auto here  = view->muteButtonBoundsForTesting(i);
        INFO("tracks " << (i - 1) << " and " << i);
        REQUIRE(here.getY() >= above.getBottom());
    }
}

TEST_CASE("A click on a mute button reports that track and no other", "[gui][arrangement]")
{
    // The hit-test and the painting must agree about where the button is.
    // Worked out separately they drift, and the control responds somewhere
    // other than where it's drawn.
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto centre = view->muteButtonBoundsForTesting(i).getCentre();
        INFO("track " << i << " centre " << centre.toString());
        REQUIRE(view->muteButtonAtForTesting(centre) == i);
    }
}

TEST_CASE("A click on the timeline is not a mute", "[gui][arrangement]")
{
    // The gutter check runs before the clip hit-test, so it must not claim
    // anything outside the gutter — otherwise clicking a clip would mute.
    JuceFixture fixture;
    auto view = viewWith(3);

    const float laneY = view->muteButtonBoundsForTesting(1).getCentreY();
    REQUIRE(view->muteButtonAtForTesting({ view->gutterWidthForTesting() + 50.0f, laneY }) == -1);
    REQUIRE(view->muteButtonAtForTesting({ 400.0f, laneY }) == -1);
}

TEST_CASE("A click in the gutter but off a button is not a mute", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(3);

    // Over the track name, well left of the button.
    const float laneY = view->muteButtonBoundsForTesting(0).getCentreY();
    REQUIRE(view->muteButtonAtForTesting({ 10.0f, laneY }) == -1);
}

TEST_CASE("The gear sits beside the mute without overlapping it", "[gui][arrangement]")
{
    // mouseDown checks mute first, so any overlap makes the gear unreachable
    // in exactly that region — a button that works everywhere except where
    // it's drawn.
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto mute = view->muteButtonBoundsForTesting(i);
        const auto gear = view->gearButtonBoundsForTesting(i);

        INFO("track " << i << " mute " << mute.toString() << " gear " << gear.toString());
        REQUIRE_FALSE(mute.intersects(gear));
        REQUIRE(gear.getRight() <= view->gutterWidthForTesting());
        REQUIRE(gear.getWidth() > 0.0f);
    }
}

TEST_CASE("A click on the gear reports that track, and mute doesn't claim it", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto centre = view->gearButtonBoundsForTesting(i).getCentre();
        INFO("track " << i);
        REQUIRE(view->gearButtonAtForTesting(centre) == i);
        REQUIRE(view->muteButtonAtForTesting(centre) == -1); // checked first, must not win here
    }
}

TEST_CASE("The gear claims nothing outside itself", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(3);

    const float laneY = view->gearButtonBoundsForTesting(1).getCentreY();
    REQUIRE(view->gearButtonAtForTesting({ 10.0f, laneY }) == -1);                                  // the name
    REQUIRE(view->gearButtonAtForTesting({ view->gutterWidthForTesting() + 40.0f, laneY }) == -1);   // the timeline
    REQUIRE(view->gearButtonAtForTesting(view->muteButtonBoundsForTesting(1).getCentre()) == -1);
}

TEST_CASE("Every track type has a tag, and they are distinct", "[gui][arrangement]")
{
    // Renaming a track is only free if something else still says what kind it
    // is. Two types sharing a tag would defeat that for one of them.
    const model::TrackType types[] = { model::TrackType::Instrument, model::TrackType::Audio,
                                       model::TrackType::Drum, model::TrackType::Guitar };

    std::vector<std::string> tags;
    for (auto type : types)
    {
        const juce::String tag = trackTypeTag(type);
        INFO("type " << (int) type << " tag " << tag);
        REQUIRE(tag.isNotEmpty());
        REQUIRE(tag.length() <= 4); // it has to fit the badge
        tags.push_back(tag.toStdString());
    }

    std::sort(tags.begin(), tags.end());
    REQUIRE(std::adjacent_find(tags.begin(), tags.end()) == tags.end());
}

TEST_CASE("The default colour leaves a track looking as it always did", "[gui][arrangement]")
{
    // Colour 0 means "untouched", and an existing project must not change
    // appearance because the feature was added.
    REQUIRE(trackColour(0) == juce::Colour(kDefaultTrackColour));
    REQUIRE(trackColour(0xff36618e) == juce::Colour(0xff36618e));
}

TEST_CASE("The palette offers distinguishable colours", "[gui][arrangement]")
{
    // The point of a fixed palette rather than a picker is telling parts
    // apart; two entries that look alike would waste a slot.
    for (int i = 1; i < kNumTrackColours; ++i)
    {
        for (int j = i + 1; j < kNumTrackColours; ++j)
        {
            const auto a = juce::Colour(kTrackColours[i].argb);
            const auto b = juce::Colour(kTrackColours[j].argb);
            INFO(kTrackColours[i].name << " vs " << kTrackColours[j].name);
            REQUIRE(std::abs(a.getHue() - b.getHue()) > 0.03f);
        }
    }
}

TEST_CASE("The gear is drawn smaller than the area it responds to", "[gui][arrangement]")
{
    // The gear's artwork is square and fills its box, while the speaker
    // beside it is wider than tall and fills only part of one — at equal box
    // sizes the gear reads as the heaviest thing in the gutter. It is drawn
    // smaller deliberately, and the hit target is deliberately not, so
    // shrinking the glyph doesn't make the button harder to hit.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float glyph = ArrangementView::gearGlyphSizeForTesting();
    const float hit   = ArrangementView::muteSizeForTesting();

    REQUIRE(glyph < hit);
    REQUIRE(glyph >= 8.0f); // still legible as a gear rather than a dot

    // Mute is drawn inside its hit area too, and larger than the gear: it is
    // the control, the gear is settings.
    const float mute = ArrangementView::muteGlyphSizeForTesting();
    REQUIRE(mute < hit);
    REQUIRE(mute > glyph);

    for (int i = 0; i < 2; ++i)
    {
        const auto bounds = view->gearButtonBoundsForTesting(i);
        INFO("track " << i << " hit area " << bounds.toString());
        REQUIRE(bounds.getWidth() == hit);   // unchanged by the glyph shrinking
        REQUIRE(bounds.getHeight() == hit);
    }
}

TEST_CASE("The ruler is a scrub target, the gutter above it isn't", "[gui][arrangement]")
{
    // The strip over the gutter is above the track names, not above any part
    // of the timeline, so there is no position for it to scrub to.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float rulerY = view->rulerHeightForTesting() * 0.5f;
    const float gutter = view->gutterWidthForTesting();

    REQUIRE(view->isOnRulerForTesting({ gutter + 10.0f, rulerY }));
    REQUIRE(view->isOnRulerForTesting({ gutter + 400.0f, rulerY }));
    REQUIRE_FALSE(view->isOnRulerForTesting({ gutter - 10.0f, rulerY })); // over the names
    REQUIRE_FALSE(view->isOnRulerForTesting({ 4.0f, rulerY }));
}

TEST_CASE("The ruler ends where the lanes begin", "[gui][arrangement]")
{
    // Below the ruler a press belongs to the clips, not to scrubbing.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float x      = view->gutterWidthForTesting() + 50.0f;
    const float height = view->rulerHeightForTesting();

    REQUIRE(view->isOnRulerForTesting({ x, 0.0f }));
    REQUIRE(view->isOnRulerForTesting({ x, height - 0.5f }));
    REQUIRE_FALSE(view->isOnRulerForTesting({ x, height }));
    REQUIRE_FALSE(view->isOnRulerForTesting({ x, height + 20.0f }));
}

TEST_CASE("Scrubbing across the ruler maps to increasing beats", "[gui][arrangement]")
{
    // What the drag actually reports. If this didn't rise with x, dragging
    // would move the playhead somewhere unrelated to the mouse.
    JuceFixture fixture;
    auto view = viewWith(2);
    view->setZoom(1.0f);

    const float gutter = view->gutterWidthForTesting();

    const double atStart  = view->beatForXForTesting(gutter);
    const double atMiddle = view->beatForXForTesting(gutter + 100.0f);
    const double atEnd    = view->beatForXForTesting(gutter + 400.0f);

    REQUIRE(atStart == 0.0);
    REQUIRE(atMiddle > atStart);
    REQUIRE(atEnd > atMiddle);

    // Dragging left of the timeline pins to the start rather than going
    // negative — a playhead before bar 1 isn't a position.
    REQUIRE(view->beatForXForTesting(gutter - 50.0f) == 0.0);
}

TEST_CASE("A point in the gutter maps to the lane it is over", "[gui][arrangement]")
{
    // Alt-dragging a header duplicates that track, so picking the wrong lane
    // here duplicates the wrong track — and the copy looks plausible enough
    // that it might not be noticed until later.
    JuceFixture fixture;
    auto view = viewWith(4);

    const float ruler = view->rulerHeightForTesting();
    const float lane  = view->laneHeightForTesting();

    for (int i = 0; i < 4; ++i)
    {
        const float middle = ruler + lane * (float) i + lane * 0.5f;
        INFO("lane " << i << " at y " << middle);
        REQUIRE(view->trackAtYForTesting(middle) == i);
    }
}

TEST_CASE("Lane boundaries belong to the lane below them", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(3);

    const float ruler = view->rulerHeightForTesting();
    const float lane  = view->laneHeightForTesting();

    REQUIRE(view->trackAtYForTesting(ruler) == 0);                    // first pixel of lane 0
    REQUIRE(view->trackAtYForTesting(ruler + lane - 0.5f) == 0);      // last of lane 0
    REQUIRE(view->trackAtYForTesting(ruler + lane) == 1);             // first of lane 1
}

TEST_CASE("The ruler and the space past the last lane are not tracks", "[gui][arrangement]")
{
    // Alt-dragging above or below the tracks must duplicate nothing rather
    // than clamping onto the nearest one.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float ruler = view->rulerHeightForTesting();
    const float lane  = view->laneHeightForTesting();

    REQUIRE(view->trackAtYForTesting(0.0f) == -1);
    REQUIRE(view->trackAtYForTesting(ruler - 0.5f) == -1);
    REQUIRE(view->trackAtYForTesting(ruler + lane * 2.0f) == -1);      // just past the last
    REQUIRE(view->trackAtYForTesting(ruler + lane * 50.0f) == -1);
}

TEST_CASE("A clip can only be dragged onto a track of the same type", "[gui][arrangement]")
{
    // Instrument/Drum/Guitar all store the same Clip/Pattern data (see
    // MainComponent::setTrackType), but a drag doesn't get to silently
    // reinterpret a melodic part's note numbers as drum-pad triggers the
    // way an explicit, one-at-a-time type change is allowed to.
    using Type = model::TrackType;

    REQUIRE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Instrument, Type::Instrument));
    REQUIRE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Drum, Type::Drum));
    REQUIRE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Guitar, Type::Guitar));

    REQUIRE_FALSE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Instrument, Type::Drum));
    REQUIRE_FALSE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Drum, Type::Guitar));
    REQUIRE_FALSE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Guitar, Type::Instrument));

    // Audio never qualifies, not even against itself: a file-backed clip
    // has no Pattern to move onto another track's timeline the same way.
    REQUIRE_FALSE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Audio, Type::Audio));
    REQUIRE_FALSE(ArrangementView::typesAreCompatibleForClipMoveForTesting(Type::Instrument, Type::Audio));
}

namespace
{
    /** A song with @p count tracks of mixed types, one clip each, for
        exercising cross-track drag compatibility end to end. Track 0 and 1
        are both Instrument (compatible with each other); track 2, if
        present, is Drum (incompatible with the other two). */
    model::Song mixedTypeSongWithTracks(int count)
    {
        model::Song song;
        for (int i = 0; i < count; ++i)
        {
            const auto type = i == 2 ? model::TrackType::Drum : model::TrackType::Instrument;
            const int  id   = model::addTrack(song, type, "Track " + std::to_string(i + 1)).id;
            model::Clip clip;
            clip.type                = model::ClipType::Instrument;
            clip.lengthBeats         = 4.0;
            clip.pattern.lengthBeats = 4.0;
            model::addClip(song, id, clip);
        }
        return song;
    }
}

namespace
{
    /** A synthetic mouse event at @p position, with @p mouseDownPosition as
        where the gesture started — matching PianoRollTests.cpp's clickAt
        helper's verified-working juce::MouseEvent constructor call, extended
        with a mouse-down position so a drag sequence (down at one point, up
        at another) is expressible, not just a single click. */
    juce::MouseEvent dragEventAt(juce::Component& target, juce::Point<float> position,
                                 juce::Point<float> mouseDownPosition)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now     = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, juce::ModifierKeys(), 1.0f,
                                0.0f, 0.0f, 0.0f, 0.0f, &target, &target,
                                now, mouseDownPosition, now, 1, false);
    }

    /** ArrangementView's mouseDown/mouseDrag/mouseUp overrides are private
        (they only need to be reachable as juce::Component event-handler
        overrides, not as part of this class's own public surface) - a test
        driving them synthetically dispatches through the base class
        reference instead of adding test-only public access to the
        production class. */
    void sendMouseDown(ArrangementView& view, const juce::MouseEvent& e) { static_cast<juce::Component&>(view).mouseDown(e); }
    void sendMouseDrag(ArrangementView& view, const juce::MouseEvent& e) { static_cast<juce::Component&>(view).mouseDrag(e); }
    void sendMouseUp(ArrangementView& view, const juce::MouseEvent& e)   { static_cast<juce::Component&>(view).mouseUp(e); }
}

TEST_CASE("Dragging a clip onto a compatible track fires onClipMovedToTrack, not onClipMoved",
          "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = std::make_unique<ArrangementView>();
    view->setVisible(true);
    view->setSize(900, 500);
    view->setSong(mixedTypeSongWithTracks(3)); // 0: Instrument, 1: Instrument, 2: Drum
    view->setZoom(1.0f);

    bool movedSameTrack = false;
    int  destTrack = -1, srcTrack = -1, srcClip = -1;
    view->onClipMoved = [&](int, int, double) { movedSameTrack = true; };
    view->onClipMovedToTrack = [&](int s, int c, int d, double) { srcTrack = s; srcClip = c; destTrack = d; };

    const float lane0Y = view->rulerHeightForTesting() + view->laneHeightForTesting() * 0.5f;
    const float lane1Y = view->rulerHeightForTesting() + view->laneHeightForTesting() * 1.5f;
    const float clipX  = view->gutterWidthForTesting() + 20.0f; // inside the one clip on track 0, away from its resize edge

    const juce::Point<float> grab { clipX, lane0Y };
    sendMouseDown(*view, dragEventAt(*view, grab, grab));
    sendMouseDrag(*view, dragEventAt(*view, { clipX, lane1Y }, grab));
    sendMouseUp(*view, dragEventAt(*view, { clipX, lane1Y }, grab));

    REQUIRE_FALSE(movedSameTrack);
    REQUIRE(srcTrack == 0);
    REQUIRE(srcClip == 0);
    REQUIRE(destTrack == 1);
}

TEST_CASE("Dragging a clip onto an incompatible track is refused", "[gui][arrangement]")
{
    // Track 2 is Drum; tracks 0/1 are Instrument. The ghost never follows
    // into track 2's lane (see mouseDrag's compatibility gate), so a
    // mouse-up there must still read as "stayed on its own track."
    JuceFixture fixture;
    auto view = std::make_unique<ArrangementView>();
    view->setVisible(true);
    view->setSize(900, 500);
    view->setSong(mixedTypeSongWithTracks(3)); // 0: Instrument, 1: Instrument, 2: Drum
    view->setZoom(1.0f);

    bool movedToTrack = false;
    bool movedSameTrack = false;
    view->onClipMovedToTrack = [&](int, int, int, double) { movedToTrack = true; };
    view->onClipMoved = [&](int, int, double) { movedSameTrack = true; };

    const float lane0Y = view->rulerHeightForTesting() + view->laneHeightForTesting() * 0.5f;
    const float lane2Y = view->rulerHeightForTesting() + view->laneHeightForTesting() * 2.5f;
    const float clipX  = view->gutterWidthForTesting() + 20.0f;

    const juce::Point<float> grab { clipX, lane0Y };
    sendMouseDown(*view, dragEventAt(*view, grab, grab));
    sendMouseDrag(*view, dragEventAt(*view, { clipX, lane2Y }, grab));
    sendMouseUp(*view, dragEventAt(*view, { clipX, lane2Y }, grab));

    REQUIRE_FALSE(movedToTrack);
    // The x position alone moved (clipX is unchanged here, so onClipMoved
    // shouldn't fire either) - this asserts no cross-track move happened,
    // which is the property under test.
    REQUIRE_FALSE(movedSameTrack);
}

namespace
{
    /** Drags the one clip on track 0 sideways by @p pixels and reports where
        it was dropped, or a negative number if the drag never landed. */
    double dragClipSideways(ArrangementView& view, float pixels)
    {
        double landedOn = -1.0;
        view.onClipMoved = [&](int, int, double startBeats) { landedOn = startBeats; };

        const float laneY = view.rulerHeightForTesting() + view.laneHeightForTesting() * 0.5f;
        const float clipX = view.gutterWidthForTesting() + 20.0f;

        const juce::Point<float> grab { clipX, laneY };
        const juce::Point<float> to   { clipX + pixels, laneY };

        sendMouseDown(view, dragEventAt(view, grab, grab));
        sendMouseDrag(view, dragEventAt(view, to, grab));
        sendMouseUp(view, dragEventAt(view, to, grab));

        return landedOn;
    }

    std::unique_ptr<ArrangementView> viewForSnapTest()
    {
        auto view = std::make_unique<ArrangementView>();
        view->setVisible(true);
        view->setSize(900, 500);
        view->setSong(mixedTypeSongWithTracks(2));
        view->setZoom(1.0f);
        return view;
    }
}

TEST_CASE("Snapping is on by default and rounds a drag to whole beats",
          "[gui][arrangement]")
{
    JuceFixture fixture;
    auto        view = viewForSnapTest();

    REQUIRE(view->snapsToGrid());

    // A deliberately fractional distance: at the default zoom this is not a
    // whole number of beats, so an unsnapped drag could not land on one by
    // accident.
    const double landedOn = dragClipSideways(*view, 37.0f);

    INFO("landed on beat " << landedOn);
    REQUIRE(landedOn >= 0.0); // the drag really happened
    REQUIRE(std::abs(landedOn - std::round(landedOn)) < 1.0e-9);
}

TEST_CASE("Turning snapping off lets a clip land off the grid", "[gui][arrangement]")
{
    // The whole point of the option: the same drag that rounded before must
    // now be able to stop between beats.
    JuceFixture fixture;
    auto        view = viewForSnapTest();
    view->setSnapToGrid(false);

    REQUIRE_FALSE(view->snapsToGrid());

    const double landedOn = dragClipSideways(*view, 37.0f);

    INFO("landed on beat " << landedOn);
    REQUIRE(landedOn >= 0.0);
    REQUIRE(std::abs(landedOn - std::round(landedOn)) > 1.0e-9);
}
