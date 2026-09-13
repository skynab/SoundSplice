#include <catch2/catch_test_macros.hpp>
#include <app/DragCommit.h>

using namespace looper;

namespace
{
    struct Fake { float gain = 0.0f; int untouched = 7; };
    auto writeGain = [](Fake& f, float v) { f.gain = v; };

    // A stand-in for something like model::EffectSlot: several fields
    // changed together as one unit, with no meaningful numeric "how far did
    // it move" — only whether it changed at all.
    struct Payload
    {
        int   mode = 0;
        float amount = 0.0f;
        bool  operator==(const Payload&) const = default;
    };

    struct FakeWithPayload { Payload payload; int untouched = 7; };
    auto writePayload = [](FakeWithPayload& f, Payload p) { f.payload = p; };
}

TEST_CASE("A drag becomes one undo step back to where it started", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    for (float v : { -5.0f, -3.0f, 0.0f, 2.0f })
        history.mutableCurrent().gain = v;
    REQUIRE_FALSE(history.canUndo());
    REQUIRE(commitDrag(history, "Set gain", -6.0f, history.current().gain, writeGain));
    REQUIRE(history.current().gain == 2.0f);
    REQUIRE(history.canUndo());
    history.undo();
    REQUIRE(history.current().gain == -6.0f);
    history.redo();
    REQUIRE(history.current().gain == 2.0f);
}

TEST_CASE("Without the rewind, undo lands mid-drag", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    history.mutableCurrent().gain = 2.0f;
    history.edit("Set gain", [](Fake& f) { f.gain = 2.0f; }); // the naive/broken version
    history.undo();
    REQUIRE(history.current().gain == 2.0f); // ...not -6, which is the bug
}

TEST_CASE("A drag that goes nowhere leaves no step", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    REQUIRE_FALSE(commitDrag(history, "Set gain", -6.0f, -6.0f, writeGain));
    REQUIRE_FALSE(history.canUndo());
    REQUIRE_FALSE(commitDrag(history, "Set gain", -6.0f, -6.0000001f, writeGain));
    REQUIRE_FALSE(history.canUndo());
}

TEST_CASE("Committing a drag disturbs nothing else", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    history.mutableCurrent().gain = 3.0f;
    history.mutableCurrent().untouched = 42;
    REQUIRE(commitDrag(history, "Set gain", -6.0f, 3.0f, writeGain));
    REQUIRE(history.current().gain == 3.0f);
    REQUIRE(history.current().untouched == 42);
    history.undo();
    REQUIRE(history.current().gain == -6.0f);
    REQUIRE(history.current().untouched == 42);
}

TEST_CASE("A struct drag becomes one undo step back to where it started", "[app][drag]")
{
    const Payload start { 0, 0.0f };
    model::History<FakeWithPayload> history(FakeWithPayload { start, 7 });

    for (Payload p : { Payload { 1, 0.2f }, Payload { 2, 0.5f }, Payload { 2, 0.9f } })
        history.mutableCurrent().payload = p;
    REQUIRE_FALSE(history.canUndo());

    const Payload landedOn = history.current().payload;
    REQUIRE(commitStructDrag(history, "Set params", start, landedOn, writePayload));
    REQUIRE(history.current().payload == landedOn);
    REQUIRE(history.canUndo());

    history.undo();
    REQUIRE(history.current().payload == start);
    history.redo();
    REQUIRE(history.current().payload == landedOn);
}

TEST_CASE("A struct drag that lands back where it started leaves no step", "[app][drag]")
{
    const Payload start { 3, 1.5f };
    model::History<FakeWithPayload> history(FakeWithPayload { start, 7 });

    REQUIRE_FALSE(commitStructDrag(history, "Set params", start, start, writePayload));
    REQUIRE_FALSE(history.canUndo());
}

TEST_CASE("Any change at all commits a struct drag, unlike the tolerance on a numeric one", "[app][drag]")
{
    // There's no meaningful "how far did it move" for a struct with mixed
    // fields, so even the smallest real change commits — equality is the
    // whole test, not a threshold.
    const Payload start { 0, 0.0f };
    const Payload landedOn { 0, 0.0000001f };
    model::History<FakeWithPayload> history(FakeWithPayload { start, 7 });

    REQUIRE(commitStructDrag(history, "Set params", start, landedOn, writePayload));
    REQUIRE(history.canUndo());
}
