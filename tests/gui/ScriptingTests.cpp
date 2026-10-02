#include <catch2/catch_test_macros.hpp>

#include <app/Scripting.h>

#include <map>

using namespace soundsplice;

namespace
{
    /** A pretend app: two tracks, a few commands, a log of what was done. */
    struct Fake
    {
        std::vector<scripting::TrackInfo>         tracks { { "Voice", "audio", 3, -2.0, 0.0, false, false, true },
                                                           { "Room Tone", "audio", 1, -12.0, 0.0, false, false, false } };
        std::vector<std::string>                  ran;
        std::vector<std::string>                  statuses;
        double                                    playhead = 4.0;
        std::optional<std::pair<double, double>>  selection;

        scripting::Host host()
        {
            scripting::Host h;
            h.commandNames = [] { return std::vector<std::string> { "Fade In", "Fade Out", "Normalize..." }; };
            h.runCommand   = [this](const std::string& name)
            {
                if (name != "Fade In" && name != "Fade Out")
                    return false;
                ran.push_back(name);
                return true;
            };
            h.macroNames = [] { return std::vector<std::string> { "Clean" }; };
            h.runMacro   = [this](const std::string& name) { ran.push_back("macro " + name); return name == "Clean"; };
            h.status     = [this](const std::string& text) { statuses.push_back(text); };
            h.tracks     = [this] { return tracks; };
            h.setTrackVolume = [this](int i, double db) { tracks[(size_t) i].volumeDb = db; return true; };
            h.setTrackMuted  = [this](int i, bool on) { tracks[(size_t) i].muted = on; return true; };
            h.renameTrack    = [this](int i, const std::string& name) { tracks[(size_t) i].name = name; return true; };
            h.playhead       = [this] { return playhead; };
            h.setPlayhead    = [this](double s) { playhead = s; };
            h.selection      = [this] { return selection; };
            h.setSelection   = [this](double a, double b) { selection = std::pair(a, b); return true; };
            h.bpm            = [] { return 120.0; };
            return h;
        }
    };
}

TEST_CASE("A script prints, and reads the tracks", "[scripting]")
{
    Fake fake;
    scripting::Engine engine(fake.host());

    const auto result = engine.run(R"lua(
        for i, t in ipairs(soundsplice.tracks()) do
            print(i, t.name, t.volume, t.clips, t.selected)
        end
        print(soundsplice.bpm(), #soundsplice.commands())
    )lua");
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.output == "1\tVoice\t-2.0\t3\ttrue\n2\tRoom Tone\t-12.0\t1\tfalse\n120.0\t3\n");
}

TEST_CASE("A script runs commands and macros, and changes tracks", "[scripting]")
{
    Fake fake;
    scripting::Engine engine(fake.host());

    const auto result = engine.run(R"lua(
        for i, t in ipairs(soundsplice.tracks()) do
            if t.name:find("Room") then
                soundsplice.setTrackMuted(i, true)
                soundsplice.renameTrack(i, "Room (muted)")
            end
        end
        soundsplice.setTrackVolume(1, -6)
        soundsplice.setSelection(10, 25)
        soundsplice.setPlayhead(10)
        assert(soundsplice.run("Fade In"))
        assert(not soundsplice.run("No Such Command"))
        assert(soundsplice.macro("Clean"))
        soundsplice.status("done")
        local a, b = soundsplice.selection()
        print(a, b, soundsplice.playhead())
    )lua");
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(fake.tracks[1].muted);
    REQUIRE(fake.tracks[1].name == "Room (muted)");
    REQUIRE(fake.tracks[0].volumeDb == -6.0);
    REQUIRE(fake.ran == std::vector<std::string> { "Fade In", "macro Clean" });
    REQUIRE(fake.statuses == std::vector<std::string> { "done" });
    REQUIRE(result.output == "10.0\t25.0\t10.0\n");
}

TEST_CASE("A script's mistakes come back as errors with their line", "[scripting]")
{
    Fake fake;
    scripting::Engine engine(fake.host());

    auto result = engine.run("print('before')\nlocal x = nil + 1\n", "Broken.lua");
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.output == "before\n"); // what it printed first is kept
    REQUIRE(result.error.find("Broken.lua:2:") != std::string::npos);

    result = engine.run("this is not lua");
    REQUIRE_FALSE(result.ok);

    // Bad arguments are caught by the bindings, not passed on.
    result = engine.run("soundsplice.setTrackVolume(7, 0)");
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("no track with that number") != std::string::npos);

    result = engine.run("soundsplice.setSelection(5, 2)");
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(fake.selection.has_value());

    // What the app didn't provide says so.
    result = engine.run("soundsplice.setTrackPan(1, 0.5)");
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("isn't available") != std::string::npos);

    // And the engine is fine to use again afterwards.
    REQUIRE(engine.run("print(1)").ok);
}

TEST_CASE("Scripts are sandboxed and can't run forever", "[scripting]")
{
    Fake fake;
    scripting::Engine engine(fake.host());

    REQUIRE(engine.run("assert(io == nil and os == nil and package == nil and debug == nil)").ok);
    REQUIRE(engine.run("assert(dofile == nil and loadfile == nil and load == nil)").ok);
    REQUIRE(engine.run("assert(string.format('%d', 3) == '3' and math.floor(2.5) == 2 and table.concat({1,2}, ',') == '1,2')").ok);

    // A fresh state each run: nothing leaks from one to the next.
    REQUIRE(engine.run("leftover = 1").ok);
    REQUIRE(engine.run("assert(leftover == nil)").ok);

    engine.setInstructionBudget(1'000'000);
    const auto result = engine.run("while true do end");
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("ran too long") != std::string::npos);
}
