#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace soundsplice::engine
{
/**
    Every audio file format SoundSplice reads: JUCE's own (WAV, AIFF, FLAC,
    Ogg Vorbis, MP3), plus Opus and WavPack through their BSD-licensed
    libraries (cmake/codecs.cmake), and Wave64, RF64/BW64 and CAF PCM through
    engine/PcmContainers.h. Decode only.

    Anything that opens audio a person brought in registers these, not
    registerBasicFormats, or a clip imported from one of them would play as
    silence. Implemented in AudioFormats.cpp, which every target that links
    JUCE's audio formats compiles.
*/
namespace audioformats
{
    /** registerBasicFormats, and the formats above after it. */
    void registerAll(juce::AudioFormatManager& formats);
}

} // namespace soundsplice::engine
