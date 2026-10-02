#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/ExportTags.h"

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

    /** The two formats written by their own libraries rather than a JUCE
        writer. Each writes @p audio whole, with @p tags, or nothing.

        WavPack: lossless, @p bits 16, 24 or 32 (float), @p level 0-3 (fast,
        normal, high, very high), TPDF dither below 32 bits when @p dither;
        tags as APEv2, the cover as a binary item. */
    bool writeWavPack(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate, int bits,
                      int level, bool dither, const ExportTags& tags);

    /** Ogg Opus at @p bitrateKbps, one or two channels, resampled to the
        48 kHz Opus always runs at (the original rate goes in its header);
        tags as Vorbis comments, the cover as METADATA_BLOCK_PICTURE. */
    bool writeOpus(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate, int bitrateKbps,
                   const ExportTags& tags);
}

} // namespace soundsplice::engine
