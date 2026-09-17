#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/PcmContainers.h>

#include <string>
#include <vector>

using namespace soundsplice::engine;
using namespace soundsplice::engine::pcmcontainer;
using Catch::Matchers::WithinAbs;

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    void put(Bytes& out, const char* text) { out.insert(out.end(), text, text + std::strlen(text)); }

    void putLE(Bytes& out, std::uint64_t value, int count)
    {
        for (int i = 0; i < count; ++i)
            out.push_back((std::uint8_t) (value >> (8 * i)));
    }

    void putBE(Bytes& out, std::uint64_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
            out.push_back((std::uint8_t) (value >> (8 * i)));
    }

    ReadAt readerOf(const Bytes& bytes)
    {
        return [&bytes](std::uint64_t offset, std::uint8_t* destination, std::size_t count)
        {
            if (offset + count > bytes.size())
                return false;
            std::memcpy(destination, bytes.data() + offset, count);
            return true;
        };
    }

    /** A WAVEFORMATEX for 16-bit PCM. */
    Bytes pcm16Format(int channels, int rate)
    {
        Bytes fmt;
        putLE(fmt, 1, 2);
        putLE(fmt, (std::uint64_t) channels, 2);
        putLE(fmt, (std::uint64_t) rate, 4);
        putLE(fmt, (std::uint64_t) (rate * channels * 2), 4);
        putLE(fmt, (std::uint64_t) (channels * 2), 2);
        putLE(fmt, 16, 2);
        return fmt;
    }

    void putW64Chunk(Bytes& out, const char* fourcc, const Bytes& body)
    {
        const auto id = detail::w64Id(fourcc);
        out.insert(out.end(), id.begin(), id.end());
        putLE(out, 24 + body.size(), 8);
        out.insert(out.end(), body.begin(), body.end());
        while (out.size() % 8 != 0)
            out.push_back(0);
    }
}

TEST_CASE("A Wave64 file gives its format and where its samples are", "[engine][pcmcontainer]")
{
    Bytes samples;
    for (int i = 0; i < 5; ++i)
    {
        putLE(samples, 16384, 2);                  // left 0.5
        putLE(samples, (std::uint16_t) -16384, 2); // right -0.5
    }

    Bytes file;
    const auto riff = detail::w64RiffId();
    file.insert(file.end(), riff.begin(), riff.end());
    putLE(file, 0, 8); // the whole size, which isn't checked
    const auto wave = detail::w64Id("wave");
    file.insert(file.end(), wave.begin(), wave.end());

    putW64Chunk(file, "fmt ", pcm16Format(2, 96000));
    putW64Chunk(file, "junk", Bytes(13, 0xee)); // padded to 8, and skipped
    const auto dataAt = file.size() + 24;
    putW64Chunk(file, "data", samples);

    const auto parsed = parseWave64(readerOf(file), file.size());
    REQUIRE(parsed);
    REQUIRE(parsed->format.encoding == RawEncoding::Signed16);
    REQUIRE_FALSE(parsed->format.bigEndian);
    REQUIRE(parsed->format.channels == 2);
    REQUIRE(parsed->format.sampleRate == 96000.0);
    REQUIRE(parsed->dataOffset == dataAt);
    REQUIRE(parsed->frames == 5);

    const auto* first = file.data() + parsed->dataOffset;
    REQUIRE_THAT(rawpcm::decodeSample(first, parsed->format.encoding, false), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(rawpcm::decodeSample(first + 2, parsed->format.encoding, false), WithinAbs(-0.5, 1e-6));

    REQUIRE_FALSE(parseRf64(readerOf(file), file.size()));
    REQUIRE_FALSE(parseCaf(readerOf(file), file.size()));
}

TEST_CASE("RF64 and BW64 take the data size from ds64", "[engine][pcmcontainer]")
{
    for (const char* magic : { "RF64", "BW64" })
    {
        INFO(magic);
        Bytes file;
        put(file, magic);
        putLE(file, 0xffffffffu, 4);
        put(file, "WAVE");

        put(file, "ds64");
        putLE(file, 28, 4);
        putLE(file, 0, 8);  // riff size
        putLE(file, 12, 8); // data size: 3 float frames of 1 channel
        putLE(file, 3, 8);  // sample count
        putLE(file, 0, 4);  // table length

        // 32-bit float, as WAVE_FORMAT_EXTENSIBLE.
        Bytes fmt;
        putLE(fmt, 0xfffe, 2);
        putLE(fmt, 1, 2);
        putLE(fmt, 48000, 4);
        putLE(fmt, 192000, 4);
        putLE(fmt, 4, 2);
        putLE(fmt, 32, 2);
        putLE(fmt, 22, 2);
        putLE(fmt, 32, 2);
        putLE(fmt, 4, 4);
        putLE(fmt, 3, 2); // the sub-format GUID starts with the real tag
        fmt.resize(40, 0);
        put(file, "fmt ");
        putLE(file, fmt.size(), 4);
        file.insert(file.end(), fmt.begin(), fmt.end());

        put(file, "data");
        putLE(file, 0xffffffffu, 4);
        const auto dataAt = file.size();
        putLE(file, 0, 12);

        const auto parsed = parseRf64(readerOf(file), file.size());
        REQUIRE(parsed);
        REQUIRE(parsed->format.encoding == RawEncoding::Float32);
        REQUIRE(parsed->format.channels == 1);
        REQUIRE(parsed->dataOffset == dataAt);
        REQUIRE(parsed->frames == 3);
    }
}

TEST_CASE("A data chunk longer than the file counts only the frames there", "[engine][pcmcontainer]")
{
    Bytes file;
    put(file, "RIFF");
    putLE(file, 0, 4);
    put(file, "WAVE");
    const auto fmt = pcm16Format(1, 8000);
    put(file, "fmt ");
    putLE(file, fmt.size(), 4);
    file.insert(file.end(), fmt.begin(), fmt.end());
    put(file, "data");
    putLE(file, 1000, 4); // a recording cut off mid-write
    putLE(file, 0, 7);    // three whole frames and a half

    const auto parsed = parseRf64(readerOf(file), file.size());
    REQUIRE(parsed);
    REQUIRE(parsed->frames == 3);
}

namespace
{
    Bytes cafWith(const char* formatId, std::uint32_t flags, int bytesPerPacket, int channels, int bits,
                  const Bytes& samples, bool unknownDataSize = false)
    {
        Bytes file;
        put(file, "caff");
        putBE(file, 1, 2);
        putBE(file, 0, 2);

        put(file, "desc");
        putBE(file, 32, 8);
        double     rate = 44100.0;
        std::uint64_t rateBits = 0;
        std::memcpy(&rateBits, &rate, sizeof rate);
        putBE(file, rateBits, 8);
        put(file, formatId);
        putBE(file, flags, 4);
        putBE(file, (std::uint64_t) bytesPerPacket, 4);
        putBE(file, 1, 4);
        putBE(file, (std::uint64_t) channels, 4);
        putBE(file, (std::uint64_t) bits, 4);

        put(file, "free");
        putBE(file, 3, 8);
        putLE(file, 0, 3);

        put(file, "data");
        putBE(file, unknownDataSize ? ~(std::uint64_t) 0 : samples.size() + 4, 8);
        putBE(file, 0, 4); // edit count
        file.insert(file.end(), samples.begin(), samples.end());
        return file;
    }
}

TEST_CASE("CAF linear PCM in either byte order", "[engine][pcmcontainer]")
{
    Bytes bigEndian;
    putBE(bigEndian, 0x4000, 2);
    putBE(bigEndian, 0xc000, 2);

    auto file   = cafWith("lpcm", 0, 2, 1, 16, bigEndian);
    auto parsed = parseCaf(readerOf(file), file.size());
    REQUIRE(parsed);
    REQUIRE(parsed->format.encoding == RawEncoding::Signed16);
    REQUIRE(parsed->format.bigEndian);
    REQUIRE(parsed->format.sampleRate == 44100.0);
    REQUIRE(parsed->frames == 2);
    REQUIRE_THAT(rawpcm::decodeSample(file.data() + parsed->dataOffset, parsed->format.encoding, true),
                 WithinAbs(0.5, 1e-6));

    // Float, little-endian (flags 1 and 2), stereo, with the data running to
    // the end of the file.
    Bytes floats(16, 0);
    file   = cafWith("lpcm", 3, 8, 2, 32, floats, true);
    parsed = parseCaf(readerOf(file), file.size());
    REQUIRE(parsed);
    REQUIRE(parsed->format.encoding == RawEncoding::Float32);
    REQUIRE_FALSE(parsed->format.bigEndian);
    REQUIRE(parsed->format.channels == 2);
    REQUIRE(parsed->frames == 2);
}

TEST_CASE("CAF U-law and A-law are read, compressed CAF isn't", "[engine][pcmcontainer]")
{
    const Bytes bytes(10, 0xff);

    auto file = cafWith("ulaw", 0, 1, 1, 8, bytes);
    REQUIRE(parseCaf(readerOf(file), file.size())->format.encoding == RawEncoding::MuLaw);

    file = cafWith("alaw", 0, 2, 2, 8, bytes);
    const auto alaw = parseCaf(readerOf(file), file.size());
    REQUIRE(alaw->format.encoding == RawEncoding::ALaw);
    REQUIRE(alaw->frames == 5);

    file = cafWith("aac ", 0, 0, 2, 0, bytes);
    REQUIRE_FALSE(parseCaf(readerOf(file), file.size()));
}

TEST_CASE("Other files, and truncated headers, aren't taken for containers", "[engine][pcmcontainer]")
{
    Bytes junk(64, 0x42);
    REQUIRE_FALSE(parseWave64(readerOf(junk), junk.size()));
    REQUIRE_FALSE(parseRf64(readerOf(junk), junk.size()));
    REQUIRE_FALSE(parseCaf(readerOf(junk), junk.size()));

    Bytes shortCaf;
    put(shortCaf, "caff");
    REQUIRE_FALSE(parseCaf(readerOf(shortCaf), shortCaf.size()));
}
