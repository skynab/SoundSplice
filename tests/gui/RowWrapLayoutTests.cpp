#include <catch2/catch_test_macros.hpp>

#include <app/RowWrapLayout.h>

using namespace looper::app;

namespace
{
/** The transport's button row, which is what this was written for: five
    transport glyphs, loop, record, click, monitor, count-in. */
std::vector<RowItem> transportRow()
{
    return {
        { 32,  0, { 2, 2 } },
        { 26,  0, { 2, 2 } },
        { 30,  0, { 3, 1 } },
        { 26,  0, { 2, 2 } },
        { 32,  0, { 2, 2 } },
        { 60, 12, { 0, 0 } },
        { 30, 12, { 1, 1 } },
        { 64, 12, { 0, 0 } },
        { 78,  6, { 0, 0 } },
        { 110, 6, { 0, 2 } },
    };
}

constexpr int kRowHeight = 30;
constexpr int kRowGap    = 4;
}

TEST_CASE("Every control gets a visible rectangle at any width", "[app][layout]")
{
    // The bug this exists to prevent: a chain of removeFromLeft calls returns
    // empty rectangles once the row is exhausted, so controls past the edge are
    // laid out zero-wide and never appear. At the default window size the
    // Transport pane is around 320px and its row wants over 500, which is how
    // the count-in box came to be invisible without anyone noticing.
    const auto items = transportRow();

    // Including widths far narrower than anything designed for — a pane is a
    // dock tab, so its width is whatever the user's layout gives it.
    for (int width : { 1200, 700, 520, 420, 320, 240, 160, 100, 40 })
    {
        const int  height = wrappedRowHeight(width, kRowHeight, kRowGap, items);
        const auto bounds = wrapRow({ 0, 0, width, height }, kRowHeight, kRowGap, items);

        INFO("width " << width << " height " << height);
        REQUIRE(bounds.size() == items.size());

        for (size_t i = 0; i < bounds.size(); ++i)
        {
            INFO("control " << i << " at " << bounds[i].toString());
            CHECK(bounds[i].getWidth() > 0);
            CHECK(bounds[i].getHeight() > 0);

            // And inside the area it was given, in both directions.
            CHECK(bounds[i].getX() >= 0);
            CHECK(bounds[i].getRight() <= width);
            CHECK(bounds[i].getY() >= 0);
            CHECK(bounds[i].getBottom() <= height);
        }
    }
}

TEST_CASE("Controls on the same row never overlap", "[app][layout]")
{
    const auto items = transportRow();

    for (int width : { 1200, 520, 320, 240, 160 })
    {
        const int  height = wrappedRowHeight(width, kRowHeight, kRowGap, items);
        const auto bounds = wrapRow({ 0, 0, width, height }, kRowHeight, kRowGap, items);

        for (size_t i = 0; i < bounds.size(); ++i)
            for (size_t j = i + 1; j < bounds.size(); ++j)
            {
                INFO("width " << width << ": " << i << " " << bounds[i].toString()
                              << " vs " << j << " " << bounds[j].toString());
                CHECK_FALSE(bounds[i].intersects(bounds[j]));
            }
    }
}

TEST_CASE("A wide row does not wrap", "[app][layout]")
{
    // Wrapping is the fallback, not the normal case: given room, the transport
    // still reads as one line of buttons.
    const auto items  = transportRow();
    const int  height = wrappedRowHeight(1200, kRowHeight, kRowGap, items);

    CHECK(height == kRowHeight);

    const auto bounds = wrapRow({ 0, 0, 1200, height }, kRowHeight, kRowGap, items);
    for (const auto& r : bounds)
        CHECK(r.getY() < kRowHeight);
}

TEST_CASE("A narrow row wraps rather than dropping controls", "[app][layout]")
{
    const auto items = transportRow();

    // The default Transport pane width, near enough. This must take more than
    // one row - if it fits in one, the test below it is not testing anything.
    const int height = wrappedRowHeight(320, kRowHeight, kRowGap, items);
    CHECK(height > kRowHeight);

    const auto bounds = wrapRow({ 0, 0, 320, height }, kRowHeight, kRowGap, items);

    int distinctRows = 0;
    int lastY        = -1;
    for (const auto& r : bounds)
        if (r.getY() != lastY)
        {
            ++distinctRows;
            lastY = r.getY();
        }

    CHECK(distinctRows > 1);
}

TEST_CASE("The reported height matches the rectangles produced", "[app][layout]")
{
    // A caller reserves space from wrappedRowHeight before laying out, so the
    // two disagreeing means the row either overlaps what is below it or leaves
    // a gap.
    const auto items = transportRow();

    for (int width : { 1200, 520, 320, 240, 160, 100 })
    {
        const int  height = wrappedRowHeight(width, kRowHeight, kRowGap, items);
        const auto bounds = wrapRow({ 0, 0, width, height }, kRowHeight, kRowGap, items);

        int lowest = 0;
        for (const auto& r : bounds)
            lowest = std::max(lowest, r.getBottom());

        INFO("width " << width << ": reported " << height << " used " << lowest);
        CHECK(lowest <= height);
        CHECK(height - lowest < kRowHeight); // reserved, but not wildly over
    }
}

TEST_CASE("An item wider than the row is clipped rather than hidden", "[app][layout]")
{
    // A control too wide to fit at all is still worth showing: clipped is
    // usable, zero-wide is not.
    const std::vector<RowItem> items { { 500, 0, { 0, 0 } } };

    const auto bounds = wrapRow({ 0, 0, 80, kRowHeight }, kRowHeight, kRowGap, items);
    REQUIRE(bounds.size() == 1);
    CHECK(bounds[0].getWidth() == 80);
}

TEST_CASE("A zero-width area yields empty rectangles rather than garbage", "[app][layout]")
{
    // A pane can be collapsed to nothing mid-drag; that must not produce
    // negative or wrapped-around geometry.
    const auto bounds = wrapRow({ 0, 0, 0, 0 }, kRowHeight, kRowGap, transportRow());

    REQUIRE(bounds.size() == transportRow().size());
    for (const auto& r : bounds)
        CHECK(r.isEmpty());
}
