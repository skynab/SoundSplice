# Decoders for the import formats JUCE doesn't read: Opus and WavPack.
#
# All BSD-licensed, and pinned to release tags. Only decoding is used, but
# libopus and libwavpack each build as one library with their encoder inside;
# the tests use those encoders to make files to read back.
#
#   libogg 1.3.5      BSD-3  the container Opus files are in
#   libopus 1.5.2     BSD-3  the codec
#   opusfile 0.12     BSD-3  seeking and decoding an Ogg Opus file
#   WavPack 5.8.1     BSD-3
#
# JUCE's Ogg Vorbis reader has its own copy of libogg, compiled as C++ inside a
# namespace, so it doesn't collide with this one.

CPMAddPackage(
  NAME ogg
  GITHUB_REPOSITORY xiph/ogg
  GIT_TAG v1.3.5
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "INSTALL_DOCS OFF"
    "INSTALL_PKG_CONFIG_MODULE OFF"
    "INSTALL_CMAKE_PACKAGE_MODULE OFF")

CPMAddPackage(
  NAME opus
  GITHUB_REPOSITORY xiph/opus
  GIT_TAG v1.5.2
  OPTIONS
    "OPUS_BUILD_SHARED_LIBRARY OFF"
    "OPUS_BUILD_TESTING OFF"
    "OPUS_BUILD_PROGRAMS OFF"
    "OPUS_INSTALL_PKG_CONFIG_MODULE OFF"
    "OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF")

# opusfile ships autotools and hand-made project files but no CMake build, so
# its four decoding sources are compiled here (http.c and wincerts.c are for
# streaming from URLs, which import never does).
CPMAddPackage(
  NAME opusfile
  GITHUB_REPOSITORY xiph/opusfile
  GIT_TAG v0.12
  DOWNLOAD_ONLY YES)

add_library(soundsplice_opusfile STATIC
  "${opusfile_SOURCE_DIR}/src/info.c"
  "${opusfile_SOURCE_DIR}/src/internal.c"
  "${opusfile_SOURCE_DIR}/src/opusfile.c"
  "${opusfile_SOURCE_DIR}/src/stream.c")
add_library(opusfile::opusfile ALIAS soundsplice_opusfile)
target_include_directories(soundsplice_opusfile SYSTEM PUBLIC "${opusfile_SOURCE_DIR}/include")
target_link_libraries(soundsplice_opusfile PUBLIC opus ogg)
# Third-party C: see cmake/lame.cmake.
target_compile_options(soundsplice_opusfile PRIVATE -w)
set_target_properties(soundsplice_opusfile PROPERTIES POSITION_INDEPENDENT_CODE ON)

CPMAddPackage(
  NAME WavPack
  GITHUB_REPOSITORY dbry/WavPack
  GIT_TAG 5.8.1
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "BUILD_TESTING OFF"
    "WAVPACK_BUILD_PROGRAMS OFF"
    "WAVPACK_BUILD_DOCS OFF"
    "WAVPACK_INSTALL_DOCS OFF"
    "WAVPACK_INSTALL_CMAKE_MODULE OFF"
    "WAVPACK_INSTALL_PKGCONFIG_MODULE OFF"
    "WAVPACK_ENABLE_LIBCRYPTO OFF")
