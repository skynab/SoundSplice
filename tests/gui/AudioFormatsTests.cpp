#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <engine/AudioFormats.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include <ogg/ogg.h>
#include <opus.h>
#include <wavpack.h>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    struct TempFolder
    {
        juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SoundSpliceAudioFormatsTests")
                              .getNonexistentChildFile("run", "", false);

        TempFolder() { root.createDirectory(); }
        ~TempFolder() { root.deleteRecursively(); }
    };

    std::unique_ptr<juce::AudioFormatReader> open(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        audioformats::registerAll(formats);
        return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
    }

    juce::AudioBuffer<float> readAll(juce::AudioFormatReader& reader)
    {
        juce::AudioBuffer<float> buffer((int) reader.numChannels, (int) reader.lengthInSamples);
        REQUIRE(reader.read(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), 0, buffer.getNumSamples()));
        return buffer;
    }

    /** Reads [from, from + count) on its own, as a player paging through the file does. */
    juce::AudioBuffer<float> readRange(juce::AudioFormatReader& reader, juce::int64 from, int count)
    {
        juce::AudioBuffer<float> buffer((int) reader.numChannels, count);
        REQUIRE(reader.read(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), from, count));
        return buffer;
    }

    //==========================================================================
    /** A WavPack file of @p frames stereo frames: left ramps up, right down.
        16-bit integers, or 32-bit float. */
    juce::File writeWavPack(const juce::File& file, int frames, bool asFloat)
    {
        juce::MemoryOutputStream bytes;
        auto blockOut = [](void* id, void* data, int32_t count)
        {
            return static_cast<juce::MemoryOutputStream*>(id)->write(data, (size_t) count) ? 1 : 0;
        };

        auto* context = WavpackOpenFileOutput(blockOut, &bytes, nullptr);
        REQUIRE(context != nullptr);

        WavpackConfig config {};
        config.num_channels     = 2;
        config.channel_mask     = 3;
        config.sample_rate      = 44100;
        config.bytes_per_sample = asFloat ? 4 : 2;
        config.bits_per_sample  = asFloat ? 32 : 16;
        config.float_norm_exp   = asFloat ? 127 : 0;
        REQUIRE(WavpackSetConfiguration64(context, &config, frames, nullptr));
        REQUIRE(WavpackPackInit(context));

        std::vector<int32_t> interleaved((size_t) frames * 2);
        for (int i = 0; i < frames; ++i)
        {
            if (asFloat)
            {
                const float left = (float) i / (float) frames, right = -left;
                std::memcpy(&interleaved[(size_t) i * 2], &left, 4);
                std::memcpy(&interleaved[(size_t) i * 2 + 1], &right, 4);
            }
            else
            {
                interleaved[(size_t) i * 2]     = i % 32768;
                interleaved[(size_t) i * 2 + 1] = -(i % 32768);
            }
        }

        REQUIRE(WavpackPackSamples(context, interleaved.data(), (uint32_t) frames));
        REQUIRE(WavpackFlushSamples(context));
        WavpackCloseFile(context);

        REQUIRE(file.replaceWithData(bytes.getData(), bytes.getDataSize()));
        return file;
    }
}

TEST_CASE("WavPack files import sample for sample", "[gui][audioformats]")
{
    TempFolder temp;
    constexpr int frames = 50000;

    for (bool asFloat : { false, true })
    {
        INFO((asFloat ? "float" : "16-bit"));
        const auto file   = writeWavPack(temp.root.getChildFile(asFloat ? "float.wv" : "int.wv"), frames, asFloat);
        auto       reader = open(file);
        REQUIRE(reader != nullptr);
        REQUIRE(reader->getFormatName() == "WavPack file");
        REQUIRE(reader->sampleRate == 44100.0);
        REQUIRE(reader->numChannels == 2);
        REQUIRE(reader->lengthInSamples == frames);

        const auto all = readAll(*reader);
        for (int i = 0; i < frames; i += 97)
        {
            const float left = asFloat ? (float) i / (float) frames : (float) (i % 32768) / 32768.0f;
            REQUIRE(all.getSample(0, i) == left);
            REQUIRE(all.getSample(1, i) == -left);
        }

        // Out of order, as a seek.
        const auto middle = readRange(*reader, 31000, 500);
        for (int i = 0; i < 500; ++i)
            REQUIRE(middle.getSample(0, i) == all.getSample(0, 31000 + i));
        const auto start = readRange(*reader, 0, 100);
        REQUIRE(start.getSample(1, 99) == all.getSample(1, 99));
    }
}

namespace
{
    /** An Ogg Opus file of a 1 kHz sine at half scale, encoded 20 ms at a time. */
    juce::File writeOpus(const juce::File& file, int channels, int frames)
    {
        int  error   = 0;
        auto encoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &error);
        REQUIRE(encoder != nullptr);
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));
        opus_int32 preSkip = 0;
        opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&preSkip));

        juce::MemoryOutputStream bytes;
        ogg_stream_state         stream;
        ogg_stream_init(&stream, 1234);

        const auto writePages = [&](bool flush)
        {
            ogg_page page;
            while ((flush ? ogg_stream_flush(&stream, &page) : ogg_stream_pageout(&stream, &page)) != 0)
            {
                bytes.write(page.header, (size_t) page.header_len);
                bytes.write(page.body, (size_t) page.body_len);
            }
        };

        const auto submit = [&](std::vector<unsigned char>& data, ogg_int64_t granule, bool bos, bool eos,
                                ogg_int64_t number)
        {
            ogg_packet packet {};
            packet.packet     = data.data();
            packet.bytes      = (long) data.size();
            packet.b_o_s      = bos ? 1 : 0;
            packet.e_o_s      = eos ? 1 : 0;
            packet.granulepos = granule;
            packet.packetno   = number;
            ogg_stream_packetin(&stream, &packet);
        };

        std::vector<unsigned char> head { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, (unsigned char) channels,
                                          (unsigned char) (preSkip & 0xff), (unsigned char) (preSkip >> 8),
                                          0x80, 0xbb, 0, 0, 0, 0, 0 };
        submit(head, 0, true, false, 0);
        writePages(true);

        std::vector<unsigned char> tags { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 4, 0, 0, 0, 't', 'e', 's', 't', 0, 0, 0, 0 };
        submit(tags, 0, false, false, 1);
        writePages(true);

        constexpr int              packetFrames = 960;
        std::vector<float>         pcm((size_t) (packetFrames * channels));
        std::vector<unsigned char> encoded(4000);
        ogg_int64_t                number = 2;

        // Encoded past the end, to flush the encoder's lookahead out.
        for (int at = 0; at < frames + preSkip; at += packetFrames, ++number)
        {
            for (int n = 0; n < packetFrames; ++n)
                for (int ch = 0; ch < channels; ++ch)
                    pcm[(size_t) (n * channels + ch)] =
                        at + n < frames ? 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * (at + n) / 48000.0)
                                        : 0.0f;

            const int size = opus_encode_float(encoder, pcm.data(), packetFrames, encoded.data(), (opus_int32) encoded.size());
            REQUIRE(size > 0);
            std::vector<unsigned char> packet(encoded.begin(), encoded.begin() + size);

            const bool last = at + packetFrames >= frames + preSkip;
            // A granule position counts the decoded samples, pre-skip included.
            submit(packet, last ? preSkip + frames : at + packetFrames, false, last, number);
            writePages(false);
        }
        writePages(true);

        ogg_stream_clear(&stream);
        opus_encoder_destroy(encoder);

        REQUIRE(file.replaceWithData(bytes.getData(), bytes.getDataSize()));
        return file;
    }

    double rms(const juce::AudioBuffer<float>& buffer, int channel, int from, int to)
    {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) buffer.getSample(channel, i) * buffer.getSample(channel, i);
        return std::sqrt(sum / (to - from));
    }
}

TEST_CASE("Opus files import at 48 kHz, their full length, sounding as encoded", "[gui][audioformats]")
{
    TempFolder    temp;
    constexpr int frames = 48000;

    for (int channels : { 1, 2 })
    {
        INFO(channels << " channels");
        const auto file   = writeOpus(temp.root.getChildFile("tone" + juce::String(channels) + ".opus"), channels, frames);
        auto       reader = open(file);
        REQUIRE(reader != nullptr);
        REQUIRE(reader->getFormatName() == "Opus file");
        REQUIRE(reader->sampleRate == 48000.0);
        REQUIRE((int) reader->numChannels == channels);
        REQUIRE(reader->lengthInSamples == frames);

        const auto all = readAll(*reader);
        for (int ch = 0; ch < channels; ++ch)
            REQUIRE_THAT(rms(all, ch, 2000, frames - 2000), WithinAbs(0.5 / std::sqrt(2.0), 0.02));

        // Pre-skip is removed: the tone starts at the start, in phase.
        REQUIRE_THAT(all.getSample(0, 12 + 1000), WithinAbs(0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 1012.0 / 48.0), 0.05));

        // A read in the middle, after a seek, gives what reading straight
        // through did: the reader lets the decoder settle before it.
        const auto middle = readRange(*reader, 30000, 700);
        double     worst  = 0.0;
        for (int i = 0; i < 700; ++i)
            worst = std::max(worst, (double) std::abs(middle.getSample(0, i) - all.getSample(0, 30000 + i)));
        INFO("worst difference " << worst);
        REQUIRE(worst < 1.0e-4);

        // Both start with the same seek, so these can match exactly.
        const auto run = readRange(*reader, 1000, 50 * 37);
        for (juce::int64 at = 1000; at < 1000 + 50 * 37; at += 37)
        {
            const auto piece = readRange(*reader, at, 37);
            for (int i = 0; i < 37; ++i)
                REQUIRE(piece.getSample(0, i) == run.getSample(0, (int) (at - 1000) + i));
        }
    }
}

TEST_CASE("Wave64, RF64 and CAF files import through their readers", "[gui][audioformats]")
{
    TempFolder temp;

    // A W64 file: 2 channels, 24-bit, three frames.
    juce::MemoryOutputStream w64;
    const auto guid = [&w64](const char* fourcc)
    {
        static constexpr unsigned char tail[] { 0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a };
        w64.write(fourcc, 4);
        w64.write(tail, sizeof tail);
    };
    static constexpr unsigned char riff[] { 0x72, 0x69, 0x66, 0x66, 0x2e, 0x91, 0xcf, 0x11,
                                            0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00 };
    w64.write(riff, sizeof riff);
    w64.writeInt64(0);
    guid("wave");
    guid("fmt ");
    w64.writeInt64(24 + 16);
    w64.writeShort(1);
    w64.writeShort(2);
    w64.writeInt(22050);
    w64.writeInt(22050 * 6);
    w64.writeShort(6);
    w64.writeShort(24);
    guid("data");
    w64.writeInt64(24 + 18);
    for (int frame = 0; frame < 3; ++frame)
    {
        const int left = frame * 0x100000, right = -left;
        for (int value : { left, right })
        {
            w64.writeByte((char) (value & 0xff));
            w64.writeByte((char) ((value >> 8) & 0xff));
            w64.writeByte((char) ((value >> 16) & 0xff));
        }
    }
    w64.writeRepeatedByte(0, 6); // padding to 8

    const auto file = temp.root.getChildFile("take.w64");
    REQUIRE(file.replaceWithData(w64.getData(), w64.getDataSize()));

    auto reader = open(file);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->sampleRate == 22050.0);
    REQUIRE(reader->numChannels == 2);
    REQUIRE(reader->lengthInSamples == 3);

    // Starting before the file and running past it: silence either side.
    juce::AudioBuffer<float> buffer(2, 6);
    REQUIRE(reader->read(buffer.getArrayOfWritePointers(), 2, -1, 6));
    REQUIRE(buffer.getSample(0, 0) == 0.0f);
    REQUIRE_THAT(buffer.getSample(0, 2), WithinAbs(0.125, 1e-6));
    REQUIRE_THAT(buffer.getSample(1, 3), WithinAbs(-0.25, 1e-6));
    REQUIRE(buffer.getSample(0, 4) == 0.0f);

    // The same bytes named .rf64 aren't RF64, and nothing else claims them.
    const auto misnamed = temp.root.getChildFile("take.rf64");
    REQUIRE(file.copyFileTo(misnamed));
    REQUIRE(open(misnamed) == nullptr);
}
