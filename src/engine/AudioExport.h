#pragma once

#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/ExportTags.h"
#include "engine/AudioFormats.h"
#include "engine/Dither.h"
#include "engine/Mp3Encoder.h"

namespace soundsplice::engine
{
/**
    Writing a rendered mix to a file, in whichever format was asked for.

    One place that knows what each format can and cannot do, so the export
    dialog can be built from these queries rather than from a second, parallel
    table of its own. That matters more than it sounds: an unsupported
    combination (FLAC at 32 bits, say) doesn't fail loudly, it returns a null
    writer - and it does so at the *end* of a long render, after the work is
    already done. A UI that can't offer the bad combination in the first place
    is the fix.
*/
enum class ExportFormat
{
    Wav = 0,
    Aiff,
    Flac,
    OggVorbis,
    Mp3,
    Opus,
    WavPack,
    M4a, // Windows only: the system's AAC encoder (see writeM4a)
};

/** How many formats this platform can export: M4A comes last, and only
    where the system has an AAC encoder to write it with. */
#if JUCE_WINDOWS
inline constexpr int kNumExportFormats = 8;
#else
inline constexpr int kNumExportFormats = 7;
#endif

inline const std::array<ExportFormat, kNumExportFormats>& allExportFormats()
{
    static const std::array<ExportFormat, kNumExportFormats> formats {
        ExportFormat::Wav, ExportFormat::Aiff, ExportFormat::Flac, ExportFormat::WavPack,
        ExportFormat::OggVorbis, ExportFormat::Opus, ExportFormat::Mp3,
       #if JUCE_WINDOWS
        ExportFormat::M4a,
       #endif
    };
    return formats;
}

/** What an export writes: the master mix, the individual tracks, or both. */
enum class ExportContents
{
    MasterMix = 0,
    MasterMixAndStems,
    StemsOnly
};

inline constexpr int kNumExportContents = 3;

inline juce::String displayNameFor (ExportContents contents)
{
    switch (contents)
    {
        case ExportContents::MasterMix:         return "Master mix";
        case ExportContents::MasterMixAndStems: return "Master mix + stems";
        case ExportContents::StemsOnly:         return "Stems only";
    }
    return "Master mix";
}

inline bool writesMasterMix (ExportContents contents)
{
    return contents != ExportContents::StemsOnly;
}

inline bool writesStems (ExportContents contents)
{
    return contents != ExportContents::MasterMix;
}

struct ExportOptions
{
    ExportFormat   format   = ExportFormat::Wav;
    ExportContents contents = ExportContents::MasterMix;

    /** 16, 24, or 32. For WAV and AIFF, 32 means IEEE float rather than
        32-bit integer - a float file is what you want when the mix will be
        processed further, since it cannot clip on the way out. Ignored by the
        formats where usesBitDepth() is false. */
    int          bitsPerSample = 24;

    double       sampleRate    = 48000.0;

    /** Index into qualityOptionsFor(format). Ignored where that is empty. */
    int          qualityIndex  = 3;

    /** Adds TPDF dither before the writer quantises - see engine::TpdfDither.
        On by default, because reducing a float mix to a fixed word length
        without it adds distortion rather than noise, and the place it is heard
        is fades and reverb tails.

        Worth turning off in one case: when the file is going to be processed
        again somewhere else, since dithering twice adds the noise twice. It is
        ignored where nothing is quantised - 32-bit float, and the lossy
        formats, which do their own thing entirely. */
    bool         dither        = true;

    /** With dither: shape its noise out of the ear's most sensitive band
        (engine::NoiseShapedDither). For a 16-bit master, mostly. */
    bool         noiseShaping  = false;

    /** Loudness-normalize on export (engine/ExportLoudness.h): the mix
        brought to this many LUFS with its true peak held under
        truePeakCeilingDb. 0 leaves the loudness as it is. */
    double       loudnessLufs      = 0.0;
    double       truePeakCeilingDb = -1.0;

    /** Tags and chapters, in whichever form the format has (ExportTags.h).
        Empty writes none. */
    ExportTags   tags;
};

/** The file extension, without the dot. */
inline juce::String extensionFor (ExportFormat format)
{
    switch (format)
    {
        case ExportFormat::Wav:       return "wav";
        case ExportFormat::Aiff:      return "aiff";
        case ExportFormat::Flac:      return "flac";
        case ExportFormat::OggVorbis: return "ogg";
        case ExportFormat::Mp3:       return "mp3";
        case ExportFormat::Opus:      return "opus";
        case ExportFormat::WavPack:   return "wv";
        case ExportFormat::M4a:       return "m4a";
    }
    return "wav";
}

/** What the format is called in the export dialog, with the trade it makes -
    a format list that just says "FLAC" assumes the reader already knows. */
inline juce::String displayNameFor (ExportFormat format)
{
    switch (format)
    {
        case ExportFormat::Wav:       return "WAV (uncompressed)";
        case ExportFormat::Aiff:      return "AIFF (uncompressed)";
        case ExportFormat::Flac:      return "FLAC (lossless, compressed)";
        case ExportFormat::OggVorbis: return "Ogg Vorbis (lossy)";
        case ExportFormat::Mp3:       return "MP3 (lossy)";
        case ExportFormat::Opus:      return "Opus (lossy, best for speech and streaming)";
        case ExportFormat::WavPack:   return "WavPack (lossless, compressed)";
        case ExportFormat::M4a:       return "M4A / AAC (lossy, for Apple devices and podcasts)";
    }
    return "WAV";
}

namespace detail
{
    inline std::unique_ptr<juce::AudioFormat> audioFormatFor (ExportFormat format)
    {
        switch (format)
        {
            case ExportFormat::Wav:       return std::make_unique<juce::WavAudioFormat>();
            case ExportFormat::Aiff:      return std::make_unique<juce::AiffAudioFormat>();
            case ExportFormat::Flac:      return std::make_unique<juce::FlacAudioFormat>();
            case ExportFormat::OggVorbis: return std::make_unique<juce::OggVorbisAudioFormat>();
            case ExportFormat::Mp3:       return std::make_unique<Mp3AudioFormat>();
            case ExportFormat::Opus:
            case ExportFormat::WavPack:
            case ExportFormat::M4a:       break; // written by their own libraries: see writeAudioFile
        }
        return nullptr;
    }
}

/** Whether a bit depth means anything for this format. False for the lossy
    ones, which store frequency-domain data and have no sample depth at all. */
inline bool usesBitDepth (ExportFormat format)
{
    return format == ExportFormat::Wav
        || format == ExportFormat::Aiff
        || format == ExportFormat::Flac
        || format == ExportFormat::WavPack;
}

/** The bit depths this format can actually write, out of the ones worth
    offering.

    Asked of the codec rather than hand-listed. A hand-written table here was
    wrong twice on its first outing - it claimed 32 bits for AIFF, which JUCE's
    writer refuses - and the failure mode is the bad kind: a null writer at the
    *end* of a long render, after all the work is done. The codec is the only
    thing that actually knows.

    8-bit is filtered out rather than offered: WAV and AIFF can both write it,
    and nobody exporting a finished mix wants it. */
inline juce::Array<int> possibleBitDepths (ExportFormat format)
{
    if (! usesBitDepth (format))
        return {};

    static const juce::Array<int> worthOffering { 16, 24, 32 };
    if (format == ExportFormat::WavPack)
        return worthOffering; // its library takes any of them

    juce::Array<int> depths;
    if (auto codec = detail::audioFormatFor (format))
        for (int bits : codec->getPossibleBitDepths())
            if (worthOffering.contains (bits))
                depths.add (bits);

    return depths;
}

/** The sample rates this format can write, out of the ones worth offering.

    Per format, not global: MPEG Layer III stops at 48kHz, so a single shared
    list would put 88.2 and 96 in front of someone choosing MP3 and fail at the
    end of the render. */
inline juce::Array<int> possibleSampleRates (ExportFormat format)
{
    // 44.1 and 48 are the two that matter; the higher pair is for handing work
    // to someone else's session.
    static const juce::Array<int> worthOffering { 44100, 48000, 88200, 96000 };
    if (format == ExportFormat::WavPack)
        return worthOffering;
    if (format == ExportFormat::Opus)
        return { 48000 }; // what Opus always runs at
    if (format == ExportFormat::M4a)
        return { 44100, 48000 }; // what the system's AAC encoder takes

    juce::Array<int> rates;
    if (auto codec = detail::audioFormatFor (format))
        for (int rate : codec->getPossibleSampleRates())
            if (worthOffering.contains (rate))
                rates.add (rate);

    return rates;
}

/** Human-readable quality settings, or empty for the lossless formats where
    quality is not a choice. */
inline juce::StringArray qualityOptionsFor (ExportFormat format)
{
    if (format == ExportFormat::WavPack)
        return { "Fast", "Normal", "High", "Very high" }; // how hard it compresses: the audio is the same
    if (format == ExportFormat::Opus)
        return { "64 kbps", "96 kbps", "128 kbps", "160 kbps", "192 kbps", "256 kbps" };
    if (format == ExportFormat::M4a)
        return { "96 kbps", "128 kbps", "160 kbps", "192 kbps" };
    if (usesBitDepth (format))
        return {};

    if (auto codec = detail::audioFormatFor (format))
        return codec->getQualityOptions();

    return {};
}

namespace detail
{
    /** Whether the export will actually quantise, which is the only case where
        dither means anything. 32-bit is float, and the lossy formats discard
        the concept of a sample depth altogether. */
    inline bool willQuantise (const ExportOptions& options, int bits)
    {
        return usesBitDepth (options.format) && bits < 32;
    }

    /**
        Pushes @p buffer through @p writer, dithering on the way if the export
        is going to quantise.

        Block at a time rather than in one call, because dither has to be added
        to a *copy* - the caller's mix is const, and rightly so - and copying a
        whole export doubles its memory for as long as the write takes. A
        ten-minute stereo master at 96k is over 400MB, so the difference
        between a block and the lot is not academic.
    */
    inline bool writeSamples (juce::AudioFormatWriter& writer,
                              const juce::AudioBuffer<float>& buffer,
                              const ExportOptions& options)
    {
        const int total = buffer.getNumSamples();

        if (! willQuantise (options, options.bitsPerSample) || ! options.dither)
            return writer.writeFromAudioSampleBuffer (buffer, 0, total);

        constexpr int kBlock = 8192;

        const int  channels = buffer.getNumChannels();
        TpdfDither dither (options.bitsPerSample);

        // Shaped: one per channel, each with its own feedback and its own
        // noise (seeded apart, so the channels' noise isn't correlated).
        std::vector<NoiseShapedDither> shapers;
        if (options.noiseShaping)
            for (int ch = 0; ch < channels; ++ch)
                shapers.emplace_back (options.bitsPerSample, options.sampleRate, 0x9E3779B9u + 0x1000193u * (uint32_t) ch);

        juce::AudioBuffer<float> block (channels, juce::jmin (kBlock, total));

        for (int pos = 0; pos < total; pos += kBlock)
        {
            const int n = juce::jmin (kBlock, total - pos);
            block.setSize (channels, n, false, false, true);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* in  = buffer.getReadPointer (ch, pos);
                auto*       out = block.getWritePointer (ch);

                // One draw per sample per channel, so the channels get
                // independent noise. Sharing it would put a correlated hiss
                // dead centre in the stereo image, which is exactly where a
                // listener notices it.
                if (! shapers.empty())
                    for (int i = 0; i < n; ++i)
                        out[i] = shapers[(size_t) ch].processSample (in[i]);
                else
                    for (int i = 0; i < n; ++i)
                        out[i] = dither.processSample (in[i]);
            }

            if (! writer.writeFromAudioSampleBuffer (block, 0, n))
                return false;
        }

        return true;
    }
}

/**
    Writes @p buffer to @p file. Returns false, and leaves no file behind, on
    any failure.

    Deleting a failed export rather than leaving a truncated one is deliberate:
    a zero-byte or half-written .wav in the place the user chose looks like a
    successful export until they try to play it.
*/
inline bool writeAudioFile (const juce::File& file,
                            const juce::AudioBuffer<float>& buffer,
                            const ExportOptions& options)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return false;

    if (options.format == ExportFormat::M4a)
    {
        static constexpr int kM4aKbps[] { 96, 128, 160, 192 };
        const bool ok = audioformats::writeM4a (file, buffer, options.sampleRate,
                                                kM4aKbps[juce::jlimit (0, 3, options.qualityIndex)]);
        if (! ok)
            file.deleteFile();
        return ok;
    }

    // The two written by their own libraries.
    if (options.format == ExportFormat::WavPack || options.format == ExportFormat::Opus)
    {
        static constexpr int kOpusKbps[] { 64, 96, 128, 160, 192, 256 };
        const bool ok = options.format == ExportFormat::WavPack
                          ? audioformats::writeWavPack (file, buffer, options.sampleRate, options.bitsPerSample,
                                                        options.qualityIndex, options.dither, options.tags, options.noiseShaping)
                          : audioformats::writeOpus (file, buffer, options.sampleRate,
                                                     kOpusKbps[juce::jlimit (0, 5, options.qualityIndex)], options.tags);
        if (! ok)
            file.deleteFile();
        return ok;
    }

    auto format = detail::audioFormatFor (options.format);
    if (format == nullptr)
        return false;

    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    // The depth the writer will really use, which is not always the one asked
    // for — possibleBitDepths clamps it. Dither has to be scaled to what is
    // actually written or it is the wrong size.
    int resolvedBits = options.bitsPerSample;

    auto writerOptions = juce::AudioFormatWriterOptions{}
                             .withSampleRate (options.sampleRate)
                             .withNumChannels (buffer.getNumChannels())
                             .withQualityOptionIndex (options.qualityIndex)
                             .withMetadataValues (tags::writerMetadata (options.tags,
                                                                        options.format == ExportFormat::Wav,
                                                                        options.format == ExportFormat::OggVorbis));

    if (usesBitDepth (options.format))
    {
        const auto depths = possibleBitDepths (options.format);
        const int  bits   = depths.contains (options.bitsPerSample) ? options.bitsPerSample
                                                                    : depths.getLast();

        writerOptions = writerOptions.withBitsPerSample (bits);
        resolvedBits  = bits;

        // 32-bit means *float* for WAV and AIFF. Written as 32-bit integer
        // instead, a mix that touches full scale would come back subtly
        // different, and the whole point of asking for 32 is that it doesn't.
        if (bits == 32)
            writerOptions = writerOptions.withSampleFormat (
                juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    }
    else
    {
        // The lossy formats still need a depth for the writer's bookkeeping,
        // and 16 is what either of them decodes to.
        writerOptions = writerOptions.withBitsPerSample (16);
    }

    // Consumed on success; see Mp3AudioFormat::createWriterFor for the one
    // case where a failure also takes the stream, which is why the file is
    // deleted below rather than only when the stream survives.
    auto writer = format->createWriterFor (stream, writerOptions);
    if (writer == nullptr)
    {
        file.deleteFile();
        return false;
    }

    ExportOptions resolved = options;
    resolved.bitsPerSample = resolvedBits;

    const bool ok = detail::writeSamples (*writer, buffer, resolved);

    // Before the file is judged: the writer flushes its last frame and closes
    // the stream in its destructor, and for the compressed formats that is
    // where a meaningful part of the data gets written.
    writer.reset();

    // The tags the writer had no way to take: FLAC's blocks and MP3's ID3.
    const bool tagged = ok && tags::finishFile (file, options.format == ExportFormat::Flac,
                                                options.format == ExportFormat::Mp3, options.tags);

    if (! ok || ! tagged)
        file.deleteFile();

    return ok && tagged;
}

} // namespace soundsplice::engine
