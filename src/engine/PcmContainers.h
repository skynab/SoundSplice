#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

#include "engine/RawPcm.h"

namespace soundsplice::engine
{
/**
    Headers of the uncompressed file formats JUCE doesn't open: Sony Wave64
    (.w64), RF64 and BW64 under their own extensions, and Apple's Core Audio
    Format (.caf) holding PCM, U-law or A-law.

    Each is a header in front of plain interleaved samples, so parsing one
    comes down to a RawPcmFormat and where the samples are, and the samples are
    then read exactly as Import Raw Data reads them. JUCE-free, reading through
    a callback, so each layout is tested on bytes built in memory.
*/
struct PcmContainer
{
    RawPcmFormat  format;
    std::uint64_t dataOffset = 0; // where the first frame starts
    std::uint64_t frames     = 0;
};

namespace pcmcontainer
{
    /** Reads @p count bytes at @p offset into @p destination; false if the
        file doesn't have them. */
    using ReadAt = std::function<bool(std::uint64_t offset, std::uint8_t* destination, std::size_t count)>;

    namespace detail
    {
        inline std::uint64_t littleEndian(const std::uint8_t* bytes, int count)
        {
            std::uint64_t value = 0;
            for (int i = count - 1; i >= 0; --i)
                value = (value << 8) | bytes[i];
            return value;
        }

        inline std::uint64_t bigEndian(const std::uint8_t* bytes, int count)
        {
            std::uint64_t value = 0;
            for (int i = 0; i < count; ++i)
                value = (value << 8) | bytes[i];
            return value;
        }

        /** The encoding a WAVE format tag and sample size describe, if it's
            one this reads. 8-bit WAVE PCM is unsigned; everything wider is signed. */
        inline std::optional<RawEncoding> waveEncoding(int tag, int bits)
        {
            if (tag == 1) // PCM
            {
                switch (bits)
                {
                    case 8:  return RawEncoding::Unsigned8;
                    case 16: return RawEncoding::Signed16;
                    case 24: return RawEncoding::Signed24;
                    case 32: return RawEncoding::Signed32;
                    default: return std::nullopt;
                }
            }
            if (tag == 3) // IEEE float
                return bits == 32 ? std::optional(RawEncoding::Float32)
                     : bits == 64 ? std::optional(RawEncoding::Float64) : std::nullopt;
            if (tag == 6 && bits == 8) return RawEncoding::ALaw;
            if (tag == 7 && bits == 8) return RawEncoding::MuLaw;
            return std::nullopt;
        }

        /** A WAVEFORMATEX (and WAVEFORMATEXTENSIBLE) body into @p out. */
        inline bool parseWaveFormat(const std::uint8_t* fmt, std::uint64_t size, RawPcmFormat& out)
        {
            if (size < 16)
                return false;

            int tag = (int) littleEndian(fmt, 2);
            if (tag == 0xfffe && size >= 26) // extensible: the real tag starts the sub-format GUID
                tag = (int) littleEndian(fmt + 24, 2);

            const auto encoding = waveEncoding(tag, (int) littleEndian(fmt + 14, 2));
            if (! encoding)
                return false;

            out.encoding   = *encoding;
            out.bigEndian  = false;
            out.channels   = (int) littleEndian(fmt + 2, 2);
            out.sampleRate = (double) littleEndian(fmt + 4, 4);
            return out.isValid();
        }

        using Guid = std::array<std::uint8_t, 16>;

        /** Wave64's chunk ids: the four RIFF letters followed by the same
            tail on every id but the file's own. */
        inline Guid w64Id(const char* fourcc)
        {
            static constexpr std::uint8_t tail[] { 0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a };
            Guid id {};
            std::memcpy(id.data(), fourcc, 4);
            std::memcpy(id.data() + 4, tail, sizeof tail);
            return id;
        }

        inline Guid w64RiffId()
        {
            return { 0x72, 0x69, 0x66, 0x66, 0x2e, 0x91, 0xcf, 0x11, 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00 };
        }
    }

    /** A Sony Wave64 file: GUID chunk ids, 64-bit sizes that count the
        24-byte chunk header, and chunks padded to 8 bytes. */
    inline std::optional<PcmContainer> parseWave64(const ReadAt& read, std::uint64_t fileLength)
    {
        std::uint8_t header[40];
        if (fileLength < 40 || ! read(0, header, sizeof header))
            return std::nullopt;
        if (std::memcmp(header, detail::w64RiffId().data(), 16) != 0
            || std::memcmp(header + 24, detail::w64Id("wave").data(), 16) != 0)
            return std::nullopt;

        PcmContainer out;
        bool         haveFormat = false;

        for (std::uint64_t at = 40; at + 24 <= fileLength;)
        {
            std::uint8_t chunk[24];
            if (! read(at, chunk, sizeof chunk))
                return std::nullopt;

            const auto size = detail::littleEndian(chunk + 16, 8);
            if (size < 24)
                return std::nullopt;
            const auto body = size - 24;

            if (std::memcmp(chunk, detail::w64Id("fmt ").data(), 16) == 0)
            {
                std::uint8_t fmt[40] {};
                const auto   wanted = (std::size_t) std::min<std::uint64_t>(body, sizeof fmt);
                if (! read(at + 24, fmt, wanted) || ! detail::parseWaveFormat(fmt, wanted, out.format))
                    return std::nullopt;
                haveFormat = true;
            }
            else if (std::memcmp(chunk, detail::w64Id("data").data(), 16) == 0)
            {
                if (! haveFormat)
                    return std::nullopt;
                const auto available = std::min<std::uint64_t>(body, fileLength - (at + 24));
                out.dataOffset       = at + 24;
                out.frames           = available / (std::uint64_t) out.format.frameBytes();
                return out;
            }

            at += (size + 7) & ~(std::uint64_t) 7;
        }
        return std::nullopt;
    }

    /** An RF64 or BW64 file: RIFF with its sizes, which don't fit in 32 bits,
        moved into a ds64 chunk. JUCE reads RF64 in a .wav; this is for the
        files named for what they are, and for BW64, which it doesn't know. */
    inline std::optional<PcmContainer> parseRf64(const ReadAt& read, std::uint64_t fileLength)
    {
        std::uint8_t header[12];
        if (fileLength < 12 || ! read(0, header, sizeof header))
            return std::nullopt;
        const bool riff = std::memcmp(header, "RIFF", 4) == 0;
        if ((! riff && std::memcmp(header, "RF64", 4) != 0 && std::memcmp(header, "BW64", 4) != 0)
            || std::memcmp(header + 8, "WAVE", 4) != 0)
            return std::nullopt;

        PcmContainer  out;
        bool          haveFormat = false;
        std::uint64_t dataSize64 = 0;

        for (std::uint64_t at = 12; at + 8 <= fileLength;)
        {
            std::uint8_t chunk[8];
            if (! read(at, chunk, sizeof chunk))
                return std::nullopt;
            const auto size = detail::littleEndian(chunk + 4, 4);

            if (std::memcmp(chunk, "ds64", 4) == 0)
            {
                std::uint8_t ds64[16];
                if (size < 16 || ! read(at + 8, ds64, sizeof ds64))
                    return std::nullopt;
                dataSize64 = detail::littleEndian(ds64 + 8, 8);
            }
            else if (std::memcmp(chunk, "fmt ", 4) == 0)
            {
                std::uint8_t fmt[40] {};
                const auto   wanted = (std::size_t) std::min<std::uint64_t>(size, sizeof fmt);
                if (! read(at + 8, fmt, wanted) || ! detail::parseWaveFormat(fmt, wanted, out.format))
                    return std::nullopt;
                haveFormat = true;
            }
            else if (std::memcmp(chunk, "data", 4) == 0)
            {
                if (! haveFormat)
                    return std::nullopt;
                const auto body      = size == 0xffffffffu ? dataSize64 : size;
                const auto available = std::min<std::uint64_t>(body, fileLength - (at + 8));
                out.dataOffset       = at + 8;
                out.frames           = available / (std::uint64_t) out.format.frameBytes();
                return out;
            }

            at += 8 + size + (size & 1);
        }
        return std::nullopt;
    }

    /** A Core Audio Format file holding linear PCM, U-law or A-law. Compressed
        contents (AAC, ALAC) aren't read. */
    inline std::optional<PcmContainer> parseCaf(const ReadAt& read, std::uint64_t fileLength)
    {
        std::uint8_t header[8];
        if (fileLength < 8 || ! read(0, header, sizeof header) || std::memcmp(header, "caff", 4) != 0
            || detail::bigEndian(header + 4, 2) != 1)
            return std::nullopt;

        PcmContainer out;
        bool         haveFormat = false;

        for (std::uint64_t at = 8; at + 12 <= fileLength;)
        {
            std::uint8_t chunk[12];
            if (! read(at, chunk, sizeof chunk))
                return std::nullopt;
            const auto size = (std::int64_t) detail::bigEndian(chunk + 4, 8);

            if (std::memcmp(chunk, "desc", 4) == 0)
            {
                std::uint8_t desc[32];
                if (size < 32 || ! read(at + 12, desc, sizeof desc))
                    return std::nullopt;

                double rate = 0.0;
                const auto rateBits = detail::bigEndian(desc, 8);
                std::memcpy(&rate, &rateBits, sizeof rate);

                const auto flags    = detail::bigEndian(desc + 12, 4);
                const auto bytes    = detail::bigEndian(desc + 16, 4); // per packet
                const auto packet   = detail::bigEndian(desc + 20, 4); // frames per packet
                const int  channels = (int) detail::bigEndian(desc + 24, 4);
                const int  bits     = (int) detail::bigEndian(desc + 28, 4);
                const bool isFloat  = (flags & 1) != 0;

                std::optional<RawEncoding> encoding;
                if (std::memcmp(desc + 8, "lpcm", 4) == 0 && packet == 1)
                {
                    if (isFloat)
                        encoding = bits == 32 ? std::optional(RawEncoding::Float32)
                                 : bits == 64 ? std::optional(RawEncoding::Float64) : std::nullopt;
                    else
                        encoding = bits == 8  ? std::optional(RawEncoding::Signed8)
                                 : bits == 16 ? std::optional(RawEncoding::Signed16)
                                 : bits == 24 ? std::optional(RawEncoding::Signed24)
                                 : bits == 32 ? std::optional(RawEncoding::Signed32) : std::nullopt;
                }
                else if (std::memcmp(desc + 8, "ulaw", 4) == 0)
                    encoding = RawEncoding::MuLaw;
                else if (std::memcmp(desc + 8, "alaw", 4) == 0)
                    encoding = RawEncoding::ALaw;

                if (! encoding)
                    return std::nullopt;

                out.format.encoding   = *encoding;
                out.format.bigEndian  = (flags & 2) == 0;
                out.format.channels   = channels;
                out.format.sampleRate = rate;
                if (! out.format.isValid() || bytes != (std::uint64_t) out.format.frameBytes())
                    return std::nullopt;
                haveFormat = true;
            }
            else if (std::memcmp(chunk, "data", 4) == 0)
            {
                // Four bytes of edit count, then the samples; a size of -1
                // means the data runs to the end of the file.
                if (! haveFormat || at + 16 > fileLength)
                    return std::nullopt;
                const auto start     = at + 16;
                const auto available = size < 4 ? fileLength - start
                                                : std::min<std::uint64_t>((std::uint64_t) size - 4, fileLength - start);
                out.dataOffset = start;
                out.frames     = available / (std::uint64_t) out.format.frameBytes();
                return out;
            }

            if (size < 0)
                return std::nullopt;
            at += 12 + (std::uint64_t) size;
        }
        return std::nullopt;
    }
}

} // namespace soundsplice::engine
