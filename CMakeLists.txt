cmake_minimum_required(VERSION 3.24)

project(LooperAudio
  VERSION 0.0.1
  DESCRIPTION "Looper-Audio — a loop-centric, AI-assisted DAW"
  HOMEPAGE_URL "https://github.com/skynab/Looper-Audio"
  LANGUAGES C CXX)

# ---- Global settings ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release build for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# JUCE refuses to compile under Xcode 15.0's Clang, citing a linker bug that
# can produce broken binaries — accepted here for local dev/test builds via
# JUCE's own escape hatch. A release build meant to ship should still be made
# on (or verified against) Xcode 15.1 or later.
add_compile_definitions(JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)

option(LOOPER_BUILD_APP   "Build the Looper-Audio application (pulls JUCE)" ON)
option(LOOPER_BUILD_TESTS "Build unit tests (pulls Catch2)"                 ON)
option(LOOPER_BUILD_TOOLS "Build command-line tools (needs JUCE)"          ON)

enable_testing()

include(cmake/get_cpm.cmake)

# ---- Framework-independent layers -------------------------------------------
# The real-time primitives are pure C++ with no JUCE dependency, so they build
# and test fast and in isolation. Everything on the audio thread trusts them.
add_subdirectory(src/rt)

# ---- Application (JUCE) ------------------------------------------------------
if(LOOPER_BUILD_APP)
  CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.14
    VERSION 8.0.14)

  # The MP3 encoder. JUCE can write every other export format but not this one
  # — see cmake/lame.cmake. Included here rather than in src/app because the
  # bounce tool and the GUI tests link the same export code.
  include(cmake/lame.cmake)

  add_subdirectory(src/app)

  # Tools link JUCE, so they build only when the app (and thus JUCE) is enabled.
  if(LOOPER_BUILD_TOOLS)
    add_subdirectory(tools/bounce)
  endif()
endif()

# ---- Tests -------------------------------------------------------------------
if(LOOPER_BUILD_TESTS)
  CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.5.2
    VERSION 3.5.2)
  add_subdirectory(tests)
endif()

# ---- Configure summary -------------------------------------------------------
message(STATUS "")
message(STATUS "Looper-Audio ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build app  : ${LOOPER_BUILD_APP}")
message(STATUS "  Build tests: ${LOOPER_BUILD_TESTS}")
message(STATUS "")
