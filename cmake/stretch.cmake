# Signalsmith Stretch, for time-stretching and pitch-shifting.
#
# Both MIT-licensed and header-only:
#
#   Signalsmith Stretch  main at 57b93f4 (Jan 2026)  the stretcher. Pinned to a
#                        commit, not the 1.1.0 tag: formant preservation, which
#                        Change Pitch uses, came after it.
#   Signalsmith Linear   0.3.1                        its FFTs, the version the
#                        stretcher's own CMake asks for.
#
# Fetched as sources only: Stretch's CMakeLists would fetch Linear itself with
# FetchContent, unpinned from here, so both are declared directly instead and
# their include folders put on soundsplice::rt, which the engine, the app and
# the tests all build against. The engine wraps it in engine/HqStretch.h.

CPMAddPackage(
  NAME signalsmith_linear
  GITHUB_REPOSITORY Signalsmith-Audio/linear
  GIT_TAG 0.3.1
  DOWNLOAD_ONLY YES)

CPMAddPackage(
  NAME signalsmith_stretch
  GITHUB_REPOSITORY Signalsmith-Audio/signalsmith-stretch
  GIT_TAG 57b93f4e9206a089a45387eaa39bdc9f310d3308
  DOWNLOAD_ONLY YES)

target_include_directories(soundsplice_rt SYSTEM INTERFACE
  "${signalsmith_stretch_SOURCE_DIR}/include"
  "${signalsmith_linear_SOURCE_DIR}/include")

# Linear's templates outgrow MSVC's default object-file section count.
if(MSVC)
  target_compile_options(soundsplice_rt INTERFACE /bigobj)
endif()
