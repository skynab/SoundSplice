# Precompiled headers.
#
# Nearly all of a translation unit's compile time here is the library headers,
# not our code: measured with MSVC, a MainComponent_*.cpp took 19.8 s and a
# file holding nothing but the JUCE headers it includes took 20.0 s; a headless
# test took 3.8 s, all of it Catch2's headers. Each target therefore parses its
# library headers once, into a precompiled header, rather than once per file.
#
#   soundsplice_precompile(<target> <header>...)
#
# The headers are force-included into every C++ file of the target, so list
# only stable library headers - never one of ours, or every edit to it would
# rebuild the precompiled header and the whole target with it.

option(SOUNDSPLICE_PCH "Precompile the JUCE and Catch2 headers each target uses" ON)

function(soundsplice_precompile target)
  if(NOT SOUNDSPLICE_PCH)
    return()
  endif()

  # C++ only: some targets have C sources too, which would otherwise get a C
  # precompiled header of these C++ headers. A <header>'s closing bracket would
  # end the generator expression, so it's spelled $<ANGLE-R> inside one.
  set(headers "")
  foreach(header IN LISTS ARGN)
    string(REPLACE ">" "$<ANGLE-R>" header "${header}")
    list(APPEND headers "$<$<COMPILE_LANGUAGE:CXX>:${header}>")
  endforeach()
  target_precompile_headers(${target} PRIVATE ${headers})

  # JUCE's module sources (juce_core.cpp and the like, compiled into each
  # target that links the module) define configuration macros before they
  # include their own module header, which a force-included copy would preempt.
  if(DEFINED JUCE_SOURCE_DIR)
    file(GLOB juce_module_sources
      "${JUCE_SOURCE_DIR}/modules/*/*.cpp"
      "${JUCE_SOURCE_DIR}/modules/*/*.mm")
    set_source_files_properties(${juce_module_sources}
      TARGET_DIRECTORY ${target}
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  endif()

  # A C++ precompiled header can't be used by an Objective-C++ file.
  get_target_property(sources ${target} SOURCES)
  list(FILTER sources INCLUDE REGEX "\\.mm$")
  if(sources)
    set_source_files_properties(${sources}
      TARGET_DIRECTORY ${target}
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  endif()

  # Clang stamps a precompiled header with its build time, which makes ccache
  # treat every rebuilt one as new; without the stamp it can reuse its results.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(${target} PRIVATE "SHELL:-Xclang -fno-pch-timestamp")
  endif()
endfunction()
