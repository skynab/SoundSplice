# Lua 5.4, for scripting (MIT) - see src/app/Scripting.h.
#
# The official release tarball, pinned by its published hash, and built from
# its own src/ folder: every library translation unit, minus the stand-alone
# interpreter (lua.c) and compiler (luac.c).
#
# Compiled as C++, which Lua supports and documents: its errors are then
# thrown as C++ exceptions rather than longjmp'd, so the destructors of the
# C++ functions it calls - the app's bindings - still run when a script fails
# in one. That is also why the headers are included directly, never through
# lua.hpp, whose extern "C" would declare C linkage for C++-built code.

CPMAddPackage(
  NAME           lua
  VERSION        5.4.7
  URL            https://www.lua.org/ftp/lua-5.4.7.tar.gz
  URL_HASH       SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
  DOWNLOAD_ONLY  YES)

file(GLOB lua_sources "${lua_SOURCE_DIR}/src/*.c")
list(FILTER lua_sources EXCLUDE REGEX "/(lua|luac)\\.c$")
set_source_files_properties(${lua_sources} PROPERTIES LANGUAGE CXX)

add_library(lua_static STATIC ${lua_sources})
add_library(lua::lua ALIAS lua_static)
target_compile_features(lua_static PUBLIC cxx_std_17)
target_include_directories(lua_static SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")

# Third-party code: its warnings aren't ours to fix (MSVC's about tmpnam and
# the like, in libraries the app never opens - see Scripting.h).
if(MSVC)
  target_compile_options(lua_static PRIVATE /w /EHsc /TP)
  target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(lua_static PRIVATE -w -x c++)
endif()
