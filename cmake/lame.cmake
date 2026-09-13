# libmp3lame, vendored.
#
# MP3 is the one export format JUCE cannot produce: MP3AudioFormat exists but
# its createWriterFor is `jassertfalse; return nullptr;` - decode only. The
# framework's other option, LAMEEncoderAudioFormat, shells out to an installed
# `lame` executable through a temporary WAV file, which makes MP3 export depend
# on whatever the user happens to have on their PATH.
#
# So the encoder is built from source here. LAME ships no CMake build and its
# config.h is autoconf-generated, so this file supplies both. Only the encoder
# is compiled - see cmake/lame-config.h.in for what is left out and why.
#
# Verified to compile clean from unmodified 3.100 sources on Apple clang
# (arm64). No source patches are applied, and none should be added without
# saying here what broke: the value of vendoring an untouched upstream tarball
# is that it can be re-fetched and checked.

CPMAddPackage(
  NAME           lame
  VERSION        3.100
  URL            https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
  # Pinned: this is the published hash of the official 3.100 release, and the
  # tarball comes from a mirror network rather than a single host.
  URL_HASH       SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
  DOWNLOAD_ONLY  YES)

set(LOOPER_LAME_PACKAGE "lame")
set(LOOPER_LAME_VERSION "3.100")

set(lame_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/lame-generated")
configure_file("${CMAKE_CURRENT_LIST_DIR}/lame-config.h.in"
               "${lame_generated_dir}/config.h" @ONLY)

# The encoder's translation units, matching libmp3lame/Makefile.am's
# libmp3lame_la_SOURCES minus mpglib_interface.c (the decoder - see the config
# header for why it's out).
set(lame_sources
  VbrTag.c bitstream.c encoder.c fft.c gain_analysis.c id3tag.c lame.c
  newmdct.c presets.c psymodel.c quantize.c quantize_pvt.c reservoir.c
  set_get.c tables.c takehiro.c util.c vbrquantize.c version.c)
list(TRANSFORM lame_sources PREPEND "${lame_SOURCE_DIR}/libmp3lame/")

add_library(lame_mp3 STATIC ${lame_sources})
add_library(lame::mp3 ALIAS lame_mp3)

target_compile_definitions(lame_mp3 PRIVATE HAVE_CONFIG_H=1)

target_include_directories(lame_mp3
  PRIVATE
    "${lame_generated_dir}"
    "${lame_SOURCE_DIR}"
    "${lame_SOURCE_DIR}/libmp3lame"
  # SYSTEM so that lame.h's own declarations don't trip the strict warning set
  # this project compiles its own code under.
  SYSTEM PUBLIC
    "${lame_SOURCE_DIR}/include")

# Third-party C from 2017 compiled under 2020s warning flags: the warnings are
# real but they are not ours to fix, and letting them into the build makes this
# project's own warnings harder to see. Errors still stop the build.
target_compile_options(lame_mp3 PRIVATE -w)

# libm is inside libSystem on macOS and the CRT on Windows; only Linux needs it
# named.
target_link_libraries(lame_mp3 PRIVATE $<$<PLATFORM_ID:Linux>:m>)

set_target_properties(lame_mp3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
