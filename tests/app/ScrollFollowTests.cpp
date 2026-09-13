#include <catch2/catch_test_macros.hpp>

#include <app/ScrollFollow.h>

using namespace looper;

namespace
{
    constexpr int kView    = 800;
    constexpr int kContent = 3200;
    constexpr int kMargin  = 24;
}

TEST_CASE("A position already in view doesn't scroll", "[app][scroll]")
{
    // The view has to stay still while the playhead crosses it. Scrolling on
    // every frame is what centring does, and it makes the bar lines slide
    // continuously under the eye.
    REQUIRE(scrollToFollow(400, 0, kView, kContent, kMargin) == 0);
    REQUIRE(scrollToFollow(100, 0, kView, kContent, kMargin) == 0);
    REQUIRE(scrollToFollow(700, 0, kView, kContent, kMargin) == 0);

    // ...and the same once scrolled along.
    REQUIRE(scrollToFollow(1200, 1000, kView, kContent, kMargin) == 1000);
}

TEST_CASE("Running off the right pages forward", "[app][scroll]")
{
    // The position lands just inside the left of the new view, so there is a
    // full page of travel before the next jump rather than another one a few
    // pixels later.
    const int offset = scrollToFollow(800, 0, kView, kContent, kMargin);

    REQUIRE(offset > 0);
    REQUIRE(offset == 800 - kMargin);

    // And it really is in view now.
    REQUIRE(800 >= offset);
    REQUIRE(800 < offset + kView);

    // A second frame a little later must not page again immediately.
    REQUIRE(scrollToFollow(820, offset, kView, kContent, kMargin) == offset);
}

TEST_CASE("Jumping backwards scrolls back", "[app][scroll]")
{
    // A seek to the start, or a loop wrapping, moves the playhead behind the
    // view. It has to come back rather than being left off-screen.
    const int offset = scrollToFollow(0, 2000, kView, kContent, kMargin);
    REQUIRE(offset == 0);

    const int mid = scrollToFollow(1500, 2400, kView, kContent, kMargin);
    REQUIRE(mid == 1500 - kMargin);
}

TEST_CASE("Scrolling stays inside the content", "[app][scroll]")
{
    // Past the end of the content there is nothing to show, so the offset
    // pins rather than scrolling into blank space.
    const int atEnd = scrollToFollow(kContent, 0, kView, kContent, kMargin);
    REQUIRE(atEnd == kContent - kView);

    const int wayPast = scrollToFollow(999999, 0, kView, kContent, kMargin);
    REQUIRE(wayPast == kContent - kView);

    REQUIRE(scrollToFollow(-500, 1000, kView, kContent, kMargin) == 0);
}

TEST_CASE("Content that fits has nowhere to scroll", "[app][scroll]")
{
    // At x1 zoom the grid fills the pane exactly, which is the normal case.
    REQUIRE(scrollToFollow(400, 0, kView, kView, kMargin) == 0);
    REQUIRE(scrollToFollow(400, 0, kView, 200, kMargin) == 0);
    REQUIRE(scrollToFollow(400, 999, kView, kView, kMargin) == 0);
}

TEST_CASE("A margin too big for the view doesn't thrash", "[app][scroll]")
{
    // A margin at or past half the view would leave no comfortable band, and
    // every single frame would trigger a scroll.
    const int huge = kView; // absurd on purpose
    const int first = scrollToFollow(400, 0, kView, kContent, huge);
    REQUIRE(scrollToFollow(400, first, kView, kContent, huge) == first); // settles
}

TEST_CASE("A degenerate viewport is handled rather than dividing by nothing", "[app][scroll]")
{
    REQUIRE(scrollToFollow(400, 0, 0, kContent, kMargin) == 0);
    REQUIRE(scrollToFollow(400, 0, -10, kContent, kMargin) == 0);
}
