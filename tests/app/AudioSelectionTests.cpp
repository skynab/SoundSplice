#include <catch2/catch_test_macros.hpp>

#include <app/AudioSelection.h>

#include <cmath>

using namespace looper;

namespace
{
    AudioSelection makeSelection(double fileSeconds, double secondsPerPixel, double start = 0.0)
    {
        AudioSelection s;
        s.fileLengthSeconds   = fileSeconds;
        s.secondsPerPixel     = secondsPerPixel;
        s.visibleStartSeconds = start;
        s.contentLeft         = 0.0f;
        return s;
    }
}

TEST_CASE("Seconds and pixels round-trip", "[app][audioselection]")
{
    const auto s = makeSelection(60.0, 0.01, 5.0);
    for (double seconds : { 5.0, 10.0, 12.345, 20.0 })
    {
        const double back = s.secondsForX(s.xForSeconds(seconds));
        INFO("seconds " << seconds);
        REQUIRE(std::abs(back - seconds) < 1.0e-6);
    }
}

TEST_CASE("The scroll offset shifts what a pixel means", "[app][audioselection]")
{
    // Scrolling is the whole point of visibleStartSeconds: the same x must
    // mean a later position once the view has moved.
    const auto atZero = makeSelection(60.0, 0.01, 0.0);
    const auto atTen  = makeSelection(60.0, 0.01, 10.0);
    REQUIRE(std::abs(atZero.secondsForX(100.0f) - 1.0) < 1.0e-9);
    REQUIRE(std::abs(atTen.secondsForX(100.0f) - 11.0) < 1.0e-9);
}

TEST_CASE("A position outside the file clamps into it", "[app][audioselection]")
{
    // A drag that leaves the component is normal, not an error — the
    // selection has to saturate at the file's ends rather than run past them
    // and later index out of bounds.
    const auto s = makeSelection(10.0, 0.01);
    REQUIRE(s.secondsForX(-500.0f) == 0.0);
    REQUIRE(s.secondsForX(999999.0f) == 10.0);
}

TEST_CASE("An empty file clamps everything to zero", "[app][audioselection]")
{
    // The state the pane is in before a thumbnail has loaded, so it has to be
    // safe rather than merely unlikely.
    const auto s = makeSelection(0.0, 0.01);
    REQUIRE(s.secondsForX(100.0f) == 0.0);
    REQUIRE(s.clampToFile(5.0) == 0.0);
}

TEST_CASE("A zero or negative zoom can't divide by zero", "[app][audioselection]")
{
    auto s = makeSelection(10.0, 0.0);
    REQUIRE(std::isfinite(s.xForSeconds(1.0)));
    REQUIRE(std::isfinite(s.secondsForX(10.0f)));

    s.secondsPerPixel = -1.0;
    REQUIRE(std::isfinite(s.xForSeconds(1.0)));
    REQUIRE(std::isfinite(s.secondsForX(10.0f)));
}

TEST_CASE("Scrolling stops at both ends of the file", "[app][audioselection]")
{
    const auto s = makeSelection(60.0, 0.01); // 1000px shows 10s
    REQUIRE(s.clampedStart(-5.0, 1000.0f) == 0.0);
    REQUIRE(std::abs(s.clampedStart(999.0, 1000.0f) - 50.0) < 1.0e-9); // 60 - 10
    REQUIRE(std::abs(s.clampedStart(20.0, 1000.0f) - 20.0) < 1.0e-9);
}

TEST_CASE("A file shorter than the view sits at the start", "[app][audioselection]")
{
    // Otherwise a short file could be scrolled off to the left, leaving the
    // pane apparently blank with no way to tell why.
    const auto s = makeSelection(2.0, 0.01); // 1000px shows 10s, file is 2s
    REQUIRE(s.clampedStart(5.0, 1000.0f) == 0.0);
}

TEST_CASE("Fitting the file chooses a zoom that shows all of it", "[app][audioselection]")
{
    auto s = makeSelection(30.0, 0.01);
    s.secondsPerPixel = s.secondsPerPixelToFit(600.0f);
    REQUIRE(std::abs(s.visibleSeconds(600.0f) - 30.0) < 1.0e-9);
}

TEST_CASE("A backwards drag still yields an ordered range", "[app][audioselection]")
{
    // Dragging right-to-left is as common as left-to-right, and every
    // consumer wants start <= end.
    const auto forward  = AudioRange::fromDrag(2.0, 8.0);
    const auto backward = AudioRange::fromDrag(8.0, 2.0);
    REQUIRE(forward == backward);
    REQUIRE(forward.startSeconds == 2.0);
    REQUIRE(forward.endSeconds == 8.0);
}

TEST_CASE("A zero-length range is empty, and that isn't the whole file", "[app][audioselection]")
{
    // Actions fall back to the whole clip when nothing is selected, so
    // "empty" and "everything" must stay distinguishable — conflating them
    // would make a stray click silently process the entire file.
    REQUIRE(AudioRange::fromDrag(4.0, 4.0).isEmpty());
    REQUIRE(AudioRange::fromDrag(4.0, 4.0).lengthSeconds() == 0.0);

    AudioRange whole { 0.0, 10.0 };
    REQUIRE_FALSE(whole.isEmpty());
    REQUIRE(whole.lengthSeconds() == 10.0);
}

TEST_CASE("A range clamps into the file it belongs to", "[app][audioselection]")
{
    const AudioRange overrun { -3.0, 25.0 };
    const auto       fixed = overrun.clampedTo(10.0);
    REQUIRE(fixed.startSeconds == 0.0);
    REQUIRE(fixed.endSeconds == 10.0);

    // Entirely past the end collapses to empty rather than inverting.
    const auto beyond = AudioRange { 20.0, 30.0 }.clampedTo(10.0);
    REQUIRE(beyond.isEmpty());
}
