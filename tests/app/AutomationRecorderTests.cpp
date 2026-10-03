#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <app/AutomationRecorder.h>

using namespace soundsplice;
using soundsplice::app::AutomationRecorder;
using Catch::Approx;

namespace
{
    model::History<model::Song> oneTrack()
    {
        model::Song song;
        model::addTrack(song, model::TrackType::Audio, "Vox");
        return model::History<model::Song>(song);
    }

    const LaneKey gain = LaneKey::trackParam(0, model::TrackParam::Gain);
    const LaneKey pan  = LaneKey::trackParam(0, model::TrackParam::Pan);

    const model::AutomationLane* gainLane(const model::Song& song)
    {
        return song.tracks[0].lane(model::TrackParam::Gain);
    }
}

TEST_CASE("Read mode and a stopped transport write nothing", "[app][automation]")
{
    auto               history = oneTrack();
    AutomationRecorder recorder(history);

    recorder.controlMoved(gain, -6.0f, true, true, 0.0); // Read
    REQUIRE_FALSE(recorder.isPassOpen());

    recorder.setMixMode(model::AutomationMode::Latch);
    recorder.controlMoved(gain, -6.0f, true, false, 0.0); // stopped
    REQUIRE_FALSE(recorder.isPassOpen());
    REQUIRE_FALSE(recorder.isWriting(gain));
}

TEST_CASE("A pass is one undo step, and undo takes the lane away again", "[app][automation]")
{
    auto               history = oneTrack();
    AutomationRecorder recorder(history);
    recorder.setMixMode(model::AutomationMode::Latch);

    recorder.controlMoved(gain, -6.0f, true, true, 1.0);
    REQUIRE(recorder.isWriting(gain));
    recorder.tick(true, 2.0);
    recorder.controlMoved(gain, -12.0f, true, true, 2.0);
    recorder.tick(true, 3.0);

    REQUIRE_FALSE(history.canUndo()); // nothing committed mid-pass

    REQUIRE(recorder.tick(false, 3.0)); // stopping closes it
    REQUIRE_FALSE(recorder.isPassOpen());
    REQUIRE(history.size() == 2);
    REQUIRE(history.undoLabel() == "Record automation");

    const auto* lane = gainLane(history.current());
    REQUIRE(lane != nullptr);
    REQUIRE(lane->valueAt(3.0) == Approx(-12.0f));

    history.undo();
    const auto* undone = gainLane(history.current());
    REQUIRE((undone == nullptr || undone->empty()));
}

TEST_CASE("A pass that writes nothing new takes no undo step", "[app][automation]")
{
    auto               history = oneTrack();
    AutomationRecorder recorder(history);
    recorder.setMixMode(model::AutomationMode::Touch);

    // Touch: a nudge with nothing held doesn't start anything.
    recorder.controlMoved(gain, -6.0f, false, true, 1.0);
    REQUIRE_FALSE(recorder.isPassOpen());

    REQUIRE_FALSE(recorder.closePass());
    REQUIRE_FALSE(history.canUndo());
}

TEST_CASE("Touch writes while held, and hands the lane back to the engine on release", "[app][automation]")
{
    auto               history = oneTrack();
    AutomationRecorder recorder(history);
    recorder.setMixMode(model::AutomationMode::Touch);

    std::vector<int> pushed;
    recorder.onEngineLanesChanged = [&](int track) { pushed.push_back(track); };

    recorder.controlMoved(gain, -6.0f, true, true, 1.0);
    REQUIRE(pushed == std::vector<int> { 0 }); // the control drives it now

    engine::TrackAutomation curves;
    curves.gain.addPoint(0.0, 0.0f);
    curves.pan.addPoint(0.0, 0.5f);
    recorder.withoutWrittenLanes(0, curves);
    REQUIRE(curves.gain.empty());       // being written
    REQUIRE_FALSE(curves.pan.empty()); // not

    recorder.tick(true, 2.0);
    recorder.controlReleased(gain, 2.0);
    REQUIRE_FALSE(recorder.isWriting(gain));
    REQUIRE(pushed == std::vector<int> { 0, 0 }); // and the lane again
    REQUIRE(recorder.isPassOpen());               // until playback stops
}

TEST_CASE("Latch carries on after release", "[app][automation]")
{
    auto               history = oneTrack();
    AutomationRecorder recorder(history);
    recorder.setMixMode(model::AutomationMode::Latch);

    recorder.controlMoved(pan, 0.25f, true, true, 1.0);
    recorder.controlReleased(pan, 1.5);
    REQUIRE(recorder.isWriting(pan));
}

TEST_CASE("Write mode takes every Write track's volume and pan from playback", "[app][automation]")
{
    auto history = oneTrack();
    history.edit("Second", [](model::Song& s) { model::addTrack(s, model::TrackType::Audio, "Gtr"); });
    history.edit("Modes", [](model::Song& s)
    {
        s.tracks[0].automationMode = (int) model::AutomationMode::Write;
        s.tracks[1].automationMode = (int) model::AutomationMode::Read;
    });
    AutomationRecorder recorder(history);

    REQUIRE(recorder.modeFor(0) == model::AutomationMode::Write);
    REQUIRE(recorder.modeFor(1) == model::AutomationMode::Read);
    REQUIRE(recorder.modeFor(-1) == model::AutomationMode::Read); // the master follows the mix

    recorder.tick(true, 0.0);
    REQUIRE(recorder.isPassOpen());
    REQUIRE(recorder.isWriting(gain));
    REQUIRE(recorder.isWriting(pan));
    REQUIRE_FALSE(recorder.isWriting(LaneKey::trackParam(1, model::TrackParam::Gain)));
}

TEST_CASE("Held controls on an effect slot are found for letting go", "[app][automation]")
{
    auto history = oneTrack();
    history.edit("Delay", [](model::Song& s)
    {
        model::EffectSlot delay;
        delay.kind = model::EffectKind::Delay;
        s.tracks[0].effectChain.push_back(delay);
    });
    AutomationRecorder recorder(history);
    recorder.setMixMode(model::AutomationMode::Latch);

    const auto mix = LaneKey::effect(0, 0, model::EffectKind::Delay, "mix");
    recorder.controlMoved(mix, 0.5f, true, true, 1.0);
    recorder.controlMoved(gain, -3.0f, true, true, 1.0);

    REQUIRE(recorder.heldOnSlot(0, 0) == std::vector<LaneKey> { mix });
}
