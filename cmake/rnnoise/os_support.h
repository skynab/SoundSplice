/* The one thing RNNoise 0.2's portable kernels (src/vec.h) need from Opus's
   os_support.h, which its release tarball includes but doesn't ship. See
   cmake/rnnoise.cmake. */
#ifndef SOUNDSPLICE_RNNOISE_OS_SUPPORT_H
#define SOUNDSPLICE_RNNOISE_OS_SUPPORT_H

#include <string.h>

#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n) * sizeof(*(dst))))

#endif
