# RNNoise 0.2 (BSD-3-Clause, Xiph.Org/Mozilla/Amazon), for AI speech
# enhancement - see src/engine/SpeechEnhance.h.
#
# The release tarball, pinned by its hash, because it - unlike the git
# repository - carries the trained model's weights (src/rnnoise_data.c), so
# nothing is downloaded at build time or at run time. Built from its library
# sources with the portable C kernels: the x86 SSE/AVX variants need runtime
# CPU dispatch this build doesn't set up, and the portable ones are plenty for
# an offline edit.

CPMAddPackage(
  NAME           rnnoise
  VERSION        0.2
  URL            https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
  URL_HASH       SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
  DOWNLOAD_ONLY  YES)

set(rnnoise_sources
  denoise.c rnn.c pitch.c kiss_fft.c celt_lpc.c nnet.c nnet_default.c
  parse_lpcnet_weights.c rnnoise_data.c rnnoise_tables.c)
list(TRANSFORM rnnoise_sources PREPEND "${rnnoise_SOURCE_DIR}/src/")

add_library(soundsplice_rnnoise STATIC ${rnnoise_sources})
add_library(rnnoise::rnnoise ALIAS soundsplice_rnnoise)
# cmake/rnnoise holds os_support.h, which the tarball's portable kernels
# include but it doesn't ship: one macro, a memset.
target_include_directories(soundsplice_rnnoise
  PRIVATE "${rnnoise_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}/rnnoise"
  SYSTEM PUBLIC "${rnnoise_SOURCE_DIR}/include")
# Linked statically: nothing to export.
target_compile_definitions(soundsplice_rnnoise PUBLIC RNNOISE_EXPORT=)

# Third-party C: its warnings aren't ours to fix.
if(MSVC)
  target_compile_options(soundsplice_rnnoise PRIVATE /w)
  target_compile_definitions(soundsplice_rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
  target_compile_options(soundsplice_rnnoise PRIVATE -w)
  target_link_libraries(soundsplice_rnnoise PRIVATE m)
endif()
