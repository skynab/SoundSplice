#include <catch2/catch_test_macros.hpp>

#include <app/TrackSelection.h>

using namespace looper;

TEST_CASE("Removing a track above the selection shifts it down", "[app][selection]")
{
    // Five tracks, third selected, first removed: the selected track is now
    // second. Without the shift the selection would silently move to a
    // different track than the one that was highlighted.
    REQUIRE(selectionAfterTrackRemoved(2, 0, 4) == 1);
    REQUIRE(selectionAfterTrackRemoved(4, 1, 4) == 3);
    REQUIRE(selectionAfterTrackRemoved(1, 0, 4) == 0);
}

TEST_CASE("Removing a track below the selection leaves it alone", "[app][selection]")
{
    REQUIRE(selectionAfterTrackRemoved(0, 1, 4) == 0);
    REQUIRE(selectionAfterTrackRemoved(2, 3, 4) == 2);
    REQUIRE(selectionAfterTrackRemoved(2, 4, 4) == 2);
}

TEST_CASE("Removing the selected track selects what took its place", "[app][selection]")
{
    // The track below slides up into the same index, which is the least
    // surprising thing to be looking at afterwards.
    REQUIRE(selectionAfterTrackRemoved(1, 1, 4) == 1);
    REQUIRE(selectionAfterTrackRemoved(0, 0, 4) == 0);
}

TEST_CASE("Removing the last track selects the new last one", "[app][selection]")
{
    // There is nothing below it to slide up, so the selection has to come
    // back rather than point past the end.
    REQUIRE(selectionAfterTrackRemoved(3, 3, 3) == 2);
    REQUIRE(selectionAfterTrackRemoved(1, 1, 1) == 0);
}

TEST_CASE("An empty song has nothing to select", "[app][selection]")
{
    REQUIRE(selectionAfterTrackRemoved(0, 0, 0) == -1);
    REQUIRE(selectionAfterTrackRemoved(3, 1, 0) == -1);
}

TEST_CASE("The result is always a real track", "[app][selection]")
{
    // Whatever it is asked, it must name a track that exists — the caller
    // indexes straight into the list with it.
    for (int selected = 0; selected < 6; ++selected)
    {
        for (int removed = 0; removed < 6; ++removed)
        {
            for (int remaining = 1; remaining < 6; ++remaining)
            {
                const int result = selectionAfterTrackRemoved(selected, removed, remaining);
                INFO("selected " << selected << " removed " << removed
                     << " remaining " << remaining << " -> " << result);
                REQUIRE(result >= 0);
                REQUIRE(result < remaining);
            }
        }
    }
}
