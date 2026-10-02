# whisper.cpp 1.9.4 (MIT), for local speech transcription - see
# src/engine/Transcriber.cpp.
#
# The release's source archive, pinned by its hash, built as static libraries
# with its own CMake (whisper and the ggml it carries). CPU only, and for any
# x86-64 machine from the last decade rather than only this one: GGML_NATIVE
# would tune the build to the CPU that compiled it, and a copy run elsewhere
# could crash on an instruction it hasn't got. No OpenMP, so there's no
# runtime library to ship; ggml uses its own thread pool.
#
# No model is bundled - they're tens to hundreds of megabytes, and which one
# is a choice (speed against accuracy, English-only or not). The user picks a
# ggml model file in Preferences > Folders; Get Models opens the page they
# come from.

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

CPMAddPackage(
  NAME           whisper
  VERSION        1.9.4
  URL            https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.4.tar.gz
  URL_HASH       SHA256=57e280cee375ab02425b806ad5146b99f6eb9357e3c2b31357c8a6af2e2e44ae
  OPTIONS
    "WHISPER_BUILD_TESTS OFF"
    "WHISPER_BUILD_EXAMPLES OFF"
    "WHISPER_BUILD_SERVER OFF"
    "WHISPER_CURL OFF"
    "WHISPER_SDL2 OFF"
    "WHISPER_ALL_WARNINGS OFF"
    "GGML_NATIVE OFF"
    "GGML_OPENMP OFF"
    "GGML_CCACHE OFF"
    "GGML_AVX ON"
    "GGML_AVX2 ON"
    "GGML_FMA ON"
    "GGML_F16C ON"
    "GGML_BUILD_TESTS OFF"
    "GGML_BUILD_EXAMPLES OFF")
