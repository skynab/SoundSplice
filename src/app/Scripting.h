#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Lua is built as C++ (cmake/lua.cmake), so its headers are included bare:
// lua.hpp's extern "C" would declare the wrong linkage.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

namespace soundsplice::scripting
{
/**
    Scripting (REAPER's ReaScript, Audacity's mod-script-pipe): Lua with the
    app's commands and a little of the project exposed, as the `soundsplice`
    table.

        for i, t in ipairs(soundsplice.tracks()) do
            if t.name:find("Room") then soundsplice.setTrackMuted(i, true) end
        end
        soundsplice.setSelection(10, 25)
        soundsplice.run("Fade In")
        soundsplice.macro("Podcast clean-up")

    The engine knows nothing about MainComponent: it talks to the app
    through a Host of callbacks, so it can be tested with a pretend one.

    **Sandboxed.** Only the base, string, table, math, utf8 and coroutine
    libraries are opened - no io, os, package or debug - and the base
    library's file and bytecode loaders are removed, so a script shared
    with someone can't touch their files or run native code. A script that
    runs too long (an endless loop) is stopped with an error rather than
    hanging the app: there is a budget of instructions per run.
*/

/** One track, as a script sees it. Indexes are Lua's: the first is 1. */
struct TrackInfo
{
    std::string name;
    std::string type; // "audio", "midi", "bus", ...
    int         clips   = 0;
    double      volumeDb = 0.0;
    double      pan      = 0.0;
    bool        muted    = false;
    bool        soloed   = false;
    bool        selected = false;
};

/** What a script can see and do. Every member may be left empty, and is
    then simply unavailable to scripts (they get an error saying so). */
struct Host
{
    std::function<std::vector<std::string>()>      commandNames;
    std::function<bool(const std::string&)>        runCommand; // false: no such command, or not now
    std::function<std::vector<std::string>()>      macroNames;
    std::function<bool(const std::string&)>        runMacro;
    std::function<void(const std::string&)>        status;
    std::function<std::vector<TrackInfo>()>        tracks;
    std::function<bool(int index, double db)>      setTrackVolume; // 0-based index
    std::function<bool(int index, double pan)>     setTrackPan;
    std::function<bool(int index, bool muted)>     setTrackMuted;
    std::function<bool(int index, bool soloed)>    setTrackSoloed;
    std::function<bool(int index, const std::string&)> renameTrack;
    std::function<bool(int index)>                 selectTrack;
    std::function<double()>                        playhead; // seconds
    std::function<void(double seconds)>            setPlayhead;
    std::function<std::optional<std::pair<double, double>>()> selection; // seconds
    std::function<bool(double start, double end)>  setSelection;
    std::function<double()>                        bpm;
};

struct Result
{
    bool        ok = false;
    std::string output; // what the script printed
    std::string error;  // why it stopped, with its line
};

class Engine
{
public:
    explicit Engine(Host host) : host_(std::move(host)) {}

    /** The instructions a run may take before it's stopped. */
    void setInstructionBudget(long long budget) noexcept { budget_ = budget; }

    /** Runs @p code in a fresh, sandboxed state; @p name labels its errors
        ("Podcast.lua:3: ..."). */
    Result run(const std::string& code, const std::string& name = "script")
    {
        Result result;
        lua_State* L = luaL_newstate();
        if (L == nullptr)
        {
            result.error = "Couldn't start Lua";
            return result;
        }

        output_.clear();
        used_ = 0;
        openSandbox(L);
        installApi(L);

        // The budget: a hook every kHookEvery instructions counts them off.
        *static_cast<Engine**>(lua_getextraspace(L)) = this;
        lua_sethook(L, &Engine::countHook, LUA_MASKCOUNT, kHookEvery);

        const auto chunk = "=" + name; // '=': use the name as is in messages
        int status = luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t");
        if (status == LUA_OK)
        {
            lua_pushcfunction(L, &Engine::traceback);
            lua_insert(L, -2);
            status = lua_pcall(L, 0, 0, -2);
        }

        if (status != LUA_OK)
        {
            const char* message = lua_tostring(L, -1);
            result.error = message != nullptr ? message : "the script failed";
        }
        result.ok     = status == LUA_OK;
        result.output = output_;
        lua_close(L);
        return result;
    }

    /** The functions scripts have, for the Script pane's help. */
    static std::vector<std::pair<std::string, std::string>> reference()
    {
        return {
            { "soundsplice.commands()", "every command's name" },
            { "soundsplice.run(name)", "runs a command by name; true if it ran" },
            { "soundsplice.macros()", "every macro's name" },
            { "soundsplice.macro(name)", "runs a macro; true if every step ran" },
            { "soundsplice.tracks()", "the tracks: name, type, clips, volume, pan, muted, soloed, selected" },
            { "soundsplice.setTrackVolume(i, dB)", "a track's fader, in dB" },
            { "soundsplice.setTrackPan(i, pan)", "-1 left to 1 right" },
            { "soundsplice.setTrackMuted(i, on)", "" },
            { "soundsplice.setTrackSoloed(i, on)", "" },
            { "soundsplice.renameTrack(i, name)", "" },
            { "soundsplice.selectTrack(i)", "" },
            { "soundsplice.playhead()", "where the playhead is, in seconds" },
            { "soundsplice.setPlayhead(seconds)", "" },
            { "soundsplice.selection()", "the time selection's start and end in seconds, or nil" },
            { "soundsplice.setSelection(start, end)", "a time selection across every audio track" },
            { "soundsplice.bpm()", "the project's tempo" },
            { "soundsplice.status(text)", "shows a message in the status bar" },
            { "print(...)", "writes to the script's output" },
        };
    }

private:
    static constexpr int       kHookEvery     = 1000;
    static constexpr long long kDefaultBudget = 200'000'000;

    static Engine& self(lua_State* L) { return **static_cast<Engine**>(lua_getextraspace(L)); }

    static void countHook(lua_State* L, lua_Debug*)
    {
        auto& engine = self(L);
        engine.used_ += kHookEvery;
        if (engine.used_ > engine.budget_)
            luaL_error(L, "the script ran too long and was stopped (an endless loop?)");
    }

    static int traceback(lua_State* L)
    {
        const char* message = lua_tostring(L, 1);
        luaL_traceback(L, L, message != nullptr ? message : "error", 1);
        return 1;
    }

    static void openSandbox(lua_State* L)
    {
        static const luaL_Reg libraries[] {
            { LUA_GNAME, luaopen_base },          { LUA_STRLIBNAME, luaopen_string },
            { LUA_TABLIBNAME, luaopen_table },    { LUA_MATHLIBNAME, luaopen_math },
            { LUA_UTF8LIBNAME, luaopen_utf8 },    { LUA_COLIBNAME, luaopen_coroutine },
        };
        for (const auto& library : libraries)
        {
            luaL_requiref(L, library.name, library.func, 1);
            lua_pop(L, 1);
        }

        // The base library's ways to reach files, and binary chunks (which
        // can crash the interpreter on purpose).
        for (const char* name : { "dofile", "loadfile", "load", "collectgarbage" })
        {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }

        lua_pushcfunction(L, &Engine::print);
        lua_setglobal(L, "print");
    }

    static int print(lua_State* L)
    {
        auto&     engine = self(L);
        const int count  = lua_gettop(L);
        for (int i = 1; i <= count; ++i)
        {
            size_t      length = 0;
            const char* text   = luaL_tolstring(L, i, &length);
            if (i > 1)
                engine.output_ += '\t';
            engine.output_.append(text, length);
            lua_pop(L, 1);
        }
        engine.output_ += '\n';
        return 0;
    }

    // ---- the soundsplice table

    template <typename Fn>
    static const Fn& need(lua_State* L, const Fn& fn, const char* what)
    {
        if (! fn)
            luaL_error(L, "%s isn't available here", what);
        return fn;
    }

    static int trackIndex(lua_State* L, int arg)
    {
        const auto index = luaL_checkinteger(L, arg);
        const auto& tracks = need(L, self(L).host_.tracks, "tracks")();
        luaL_argcheck(L, index >= 1 && index <= (lua_Integer) tracks.size(), arg, "no track with that number");
        return (int) index - 1;
    }

    static void pushStrings(lua_State* L, const std::vector<std::string>& strings)
    {
        lua_createtable(L, (int) strings.size(), 0);
        for (size_t i = 0; i < strings.size(); ++i)
        {
            lua_pushlstring(L, strings[i].data(), strings[i].size());
            lua_rawseti(L, -2, (lua_Integer) i + 1);
        }
    }

    static int commands(lua_State* L)
    {
        pushStrings(L, need(L, self(L).host_.commandNames, "commands")());
        return 1;
    }

    static int run(lua_State* L)
    {
        const std::string name = luaL_checkstring(L, 1);
        lua_pushboolean(L, need(L, self(L).host_.runCommand, "run")(name));
        return 1;
    }

    static int macros(lua_State* L)
    {
        pushStrings(L, need(L, self(L).host_.macroNames, "macros")());
        return 1;
    }

    static int macro(lua_State* L)
    {
        const std::string name = luaL_checkstring(L, 1);
        lua_pushboolean(L, need(L, self(L).host_.runMacro, "macro")(name));
        return 1;
    }

    static int status(lua_State* L)
    {
        size_t      length = 0;
        const char* text   = luaL_tolstring(L, 1, &length);
        need(L, self(L).host_.status, "status")(std::string(text, length));
        return 0;
    }

    static int tracks(lua_State* L)
    {
        const auto list = need(L, self(L).host_.tracks, "tracks")();
        lua_createtable(L, (int) list.size(), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            const auto& t = list[i];
            lua_createtable(L, 0, 8);
            lua_pushlstring(L, t.name.data(), t.name.size()); lua_setfield(L, -2, "name");
            lua_pushstring(L, t.type.c_str());               lua_setfield(L, -2, "type");
            lua_pushinteger(L, t.clips);                     lua_setfield(L, -2, "clips");
            lua_pushnumber(L, t.volumeDb);                   lua_setfield(L, -2, "volume");
            lua_pushnumber(L, t.pan);                        lua_setfield(L, -2, "pan");
            lua_pushboolean(L, t.muted);                     lua_setfield(L, -2, "muted");
            lua_pushboolean(L, t.soloed);                    lua_setfield(L, -2, "soloed");
            lua_pushboolean(L, t.selected);                  lua_setfield(L, -2, "selected");
            lua_rawseti(L, -2, (lua_Integer) i + 1);
        }
        return 1;
    }

    static int setTrackVolume(lua_State* L)
    {
        const int    index = trackIndex(L, 1);
        const double db    = luaL_checknumber(L, 2);
        lua_pushboolean(L, need(L, self(L).host_.setTrackVolume, "setTrackVolume")(index, db));
        return 1;
    }

    static int setTrackPan(lua_State* L)
    {
        const int    index = trackIndex(L, 1);
        const double pan   = luaL_checknumber(L, 2);
        lua_pushboolean(L, need(L, self(L).host_.setTrackPan, "setTrackPan")(index, pan));
        return 1;
    }

    static int setTrackMuted(lua_State* L)
    {
        const int index = trackIndex(L, 1);
        lua_pushboolean(L, need(L, self(L).host_.setTrackMuted, "setTrackMuted")(index, lua_toboolean(L, 2) != 0));
        return 1;
    }

    static int setTrackSoloed(lua_State* L)
    {
        const int index = trackIndex(L, 1);
        lua_pushboolean(L, need(L, self(L).host_.setTrackSoloed, "setTrackSoloed")(index, lua_toboolean(L, 2) != 0));
        return 1;
    }

    static int renameTrack(lua_State* L)
    {
        const int         index = trackIndex(L, 1);
        const std::string name  = luaL_checkstring(L, 2);
        lua_pushboolean(L, need(L, self(L).host_.renameTrack, "renameTrack")(index, name));
        return 1;
    }

    static int selectTrack(lua_State* L)
    {
        const int index = trackIndex(L, 1);
        lua_pushboolean(L, need(L, self(L).host_.selectTrack, "selectTrack")(index));
        return 1;
    }

    static int playhead(lua_State* L)
    {
        lua_pushnumber(L, need(L, self(L).host_.playhead, "playhead")());
        return 1;
    }

    static int setPlayhead(lua_State* L)
    {
        const double seconds = luaL_checknumber(L, 1);
        luaL_argcheck(L, seconds >= 0.0, 1, "can't be before the start");
        need(L, self(L).host_.setPlayhead, "setPlayhead")(seconds);
        return 0;
    }

    static int selection(lua_State* L)
    {
        const auto range = need(L, self(L).host_.selection, "selection")();
        if (! range)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, range->first);
        lua_pushnumber(L, range->second);
        return 2;
    }

    static int setSelection(lua_State* L)
    {
        const double start = luaL_checknumber(L, 1);
        const double end   = luaL_checknumber(L, 2);
        luaL_argcheck(L, start >= 0.0, 1, "can't be before the start");
        luaL_argcheck(L, end > start, 2, "has to be after the start");
        lua_pushboolean(L, need(L, self(L).host_.setSelection, "setSelection")(start, end));
        return 1;
    }

    static int bpm(lua_State* L)
    {
        lua_pushnumber(L, need(L, self(L).host_.bpm, "bpm")());
        return 1;
    }

    void installApi(lua_State* L)
    {
        static const luaL_Reg api[] {
            { "commands", &Engine::commands },         { "run", &Engine::run },
            { "macros", &Engine::macros },             { "macro", &Engine::macro },
            { "status", &Engine::status },             { "tracks", &Engine::tracks },
            { "setTrackVolume", &Engine::setTrackVolume }, { "setTrackPan", &Engine::setTrackPan },
            { "setTrackMuted", &Engine::setTrackMuted }, { "setTrackSoloed", &Engine::setTrackSoloed },
            { "renameTrack", &Engine::renameTrack },   { "selectTrack", &Engine::selectTrack },
            { "playhead", &Engine::playhead },         { "setPlayhead", &Engine::setPlayhead },
            { "selection", &Engine::selection },       { "setSelection", &Engine::setSelection },
            { "bpm", &Engine::bpm },                   { nullptr, nullptr },
        };
        luaL_newlib(L, api);
        lua_setglobal(L, "soundsplice");
    }

    Host        host_;
    std::string output_;
    long long   budget_ = kDefaultBudget;
    long long   used_   = 0;
};

} // namespace soundsplice::scripting
