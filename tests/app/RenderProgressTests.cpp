#include <catch2/catch_test_macros.hpp>

#include <app/RenderProgress.h>

using looper::app::overallProgress;

TEST_CASE("A single task's progress is its own", "[app][progress]")
{
    CHECK(overallProgress(0, 1, 0.0) == 0.0);
    CHECK(overallProgress(0, 1, 0.5) == 0.5);
    CHECK(overallProgress(0, 1, 1.0) == 1.0);
}

TEST_CASE("Progress spans the whole set rather than restarting", "[app][progress]")
{
    // The bug this exists to prevent: a bar that restarts at zero for every
    // stem says "nearly done" eight times over.
    CHECK(overallProgress(0, 4, 1.0) == 0.25);
    CHECK(overallProgress(1, 4, 0.0) == 0.25);
    CHECK(overallProgress(1, 4, 1.0) == 0.5);
    CHECK(overallProgress(3, 4, 1.0) == 1.0);
}

TEST_CASE("Progress never goes backwards", "[app][progress]")
{
    // Walked the way an export actually walks it: each task from 0 to 1 in
    // turn. A bar that dips is worse than one that is merely coarse.
    constexpr int tasks = 9; // a master mix plus eight stems

    double previous = -1.0;
    for (int task = 0; task < tasks; ++task)
    {
        for (int step = 0; step <= 10; ++step)
        {
            const double value = overallProgress(task, tasks, step / 10.0);
            INFO("task " << task << " step " << step << " -> " << value);
            CHECK(value >= previous);
            previous = value;
        }
    }

    CHECK(previous == 1.0); // and it does arrive
}

TEST_CASE("Progress stays within range whatever it is handed", "[app][progress]")
{
    // A render reporting slightly past its end (a final short block, say) must
    // not push the bar past full or wrap it round.
    CHECK(overallProgress(0, 4, 2.0) == 0.25);
    CHECK(overallProgress(0, 4, -1.0) == 0.0);
    CHECK(overallProgress(9, 4, 1.0) == 1.0);   // index past the end
    CHECK(overallProgress(-1, 4, 0.5) == 0.125); // index before the start

    for (int task = -2; task < 12; ++task)
        for (double fraction : { -5.0, 0.0, 0.5, 1.0, 5.0 })
        {
            const double value = overallProgress(task, 4, fraction);
            INFO("task " << task << " fraction " << fraction << " -> " << value);
            CHECK(value >= 0.0);
            CHECK(value <= 1.0);
        }
}

TEST_CASE("No tasks reads as finished, not as stuck", "[app][progress]")
{
    // An export with nothing to render (every track muted, stems only) must not
    // leave a bar sitting at zero forever.
    CHECK(overallProgress(0, 0, 0.0) == 1.0);
}
