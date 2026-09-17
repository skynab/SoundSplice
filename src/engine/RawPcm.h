#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace soundsplice::engine
{
/**
    Headerless audio: samples with nothing in the file to say how to read them,
    as Audacity's Import Raw Data opens. A dump from a hardware recorder or an
    embedded device, a file whose header was lost, or a stream captured
    straight off a bus.

    The person importing says what the bytes are (RawPcmFormat); this turns
    them into floats a block at a time, so a file of any length is converted
    without being held in memory. JUCE-free, so every encoding is tested on
    its own.
*/
enum class RawEncoding
{
    Signed8    = 0,
    Unsigned8  = 1,
    Signed16   = 2,
    Signed24   = 3,
    Signed32   = 4,
    Float32    = 5,
    Float64    = 6,
    MuLaw      = 7,
    ALaw       = 8
};

inline constexpr int kNumRawEncodings = 9;

inline const char* displayNameFor(RawEncoding encoding)
{
    switch (encoding)
    {
        case RawEncoding::Signed8:   return "Signed 8-bit PCM";
        case RawEncoding::Unsigned8: return "Unsigned 8-bit PCM";
        case RawEncoding::Signed16:  return "Signed 16-bit PCM";
        case RawEncoding::Signed24:  return "Signed 24-bit PCM";
        case RawEncoding::Signed32:  return "Signed 32-bit PCM";
        case RawEncoding::Float32:   return "32-bit float";
        case RawEncoding::Float64:   return "64-bit float";
        case RawEncoding::MuLaw:     return "U-Law";
        case RawEncoding::ALaw:      return "A-Law";
    }
    return "";
}

/** How many bytes one sample of @p encoding takes. */
inline int bytesPerSample(RawEncoding encoding)
{
    switch (encoding)
    {
        case RawEncoding::Signed16: return 2;
        case RawEncoding::Signed24: return 3;
        case RawEncoding::Signed32:
        case RawEncoding::Float32:  return 4;
        case RawEncoding::Float64:  return 8;
        default:                    return 1;
    }
}

struct RawPcmFormat
{
    RawEncoding    encoding     = RawEncoding::Signed16;
    bool           bigEndian    = false;
    int            channels     = 1;
    double         sampleRate   = 44100.0;
    std::uint64_t  headerBytes  = 0; // skipped at the start of the file

    /** Bytes in one frame: a sample on every channel, interleaved. */
    int frameBytes() const { return bytesPerSample(encoding) * std::max(1, channels); }

    bool isValid() const { return channels >= 1 && channels <= 32 && sampleRate > 0.0; }

    /** Whole frames in a file of @p fileBytes: a partial frame at the end is
        left off rather than read as garbage. */
    std::uint64_t framesIn(std::uint64_t fileBytes) const
    {
        if (! isValid() || fileBytes <= headerBytes)
            return 0;
        return (fileBytes - headerBytes) / (std::uint64_t) frameBytes();
    }
};

namespace rawpcm
{
    /** G.711 mu-law byte to a linear sample in [-1, 1). */
    inline float fromMuLaw(std::uint8_t byte)
    {
        const int value     = ~byte & 0xff;
        const int sign      = value & 0x80;
        const int exponent  = (value >> 4) & 0x07;
        const int mantissa  = value & 0x0f;
        const int magnitude = (((mantissa << 3) + 0x84) << exponent) - 0x84;
        return (float) (sign != 0 ? -magnitude : magnitude) / 32768.0f;
    }

    /** G.711 A-law byte to a linear sample in [-1, 1). */
    inline float fromALaw(std::uint8_t byte)
    {
        const int value    = byte ^ 0x55;
        const int exponent = (value >> 4) & 0x07;
        const int mantissa = value & 0x0f;
        int       magnitude = exponent == 0 ? (mantissa << 4) + 8 : ((mantissa << 4) + 0x108) << (exponent - 1);
        return (float) ((value & 0x80) != 0 ? magnitude : -magnitude) / 32768.0f;
    }

    /** One sample of @p encoding from the @p bytesPerSample bytes at @p data. */
    inline float decodeSample(const std::uint8_t* data, RawEncoding encoding, bool bigEndian)
    {
        const int size = bytesPerSample(encoding);

        // Assembled little-endian first, whatever the file's order.
        std::uint64_t word = 0;
        for (int i = 0; i < size; ++i)
        {
            const int from = bigEndian ? size - 1 - i : i;
            word |= (std::uint64_t) data[from] << (8 * i);
        }

        switch (encoding)
        {
            case RawEncoding::Signed8:   return (float) (std::int8_t) (std::uint8_t) word / 128.0f;
            case RawEncoding::Unsigned8: return ((float) word - 128.0f) / 128.0f;
            case RawEncoding::Signed16:  return (float) (std::int16_t) (std::uint16_t) word / 32768.0f;
            case RawEncoding::Signed24:
            {
                auto value = (std::int32_t) word;
                if ((value & 0x800000) != 0)
                    value -= 0x1000000;
                return (float) value / 8388608.0f;
            }
            case RawEncoding::Signed32:  return (float) ((double) (std::int32_t) (std::uint32_t) word / 2147483648.0);
            case RawEncoding::Float32:
            {
                const auto bits = (std::uint32_t) word;
                float      value = 0.0f;
                std::memcpy(&value, &bits, sizeof value);
                return std::isfinite(value) ? value : 0.0f;
            }
            case RawEncoding::Float64:
            {
                double value = 0.0;
                std::memcpy(&value, &word, sizeof value);
                return std::isfinite(value) ? (float) value : 0.0f;
            }
            case RawEncoding::MuLaw:     return fromMuLaw((std::uint8_t) word);
            case RawEncoding::ALaw:      return fromALaw((std::uint8_t) word);
        }
        return 0.0f;
    }

    /** Decodes @p frames interleaved frames at @p data into @p out, one
        vector per channel, each resized to @p frames. */
    inline void decodeFrames(const std::uint8_t* data, std::size_t frames, const RawPcmFormat& format,
                             std::vector<std::vector<float>>& out)
    {
        const int channels = std::max(1, format.channels);
        const int size     = bytesPerSample(format.encoding);

        out.resize((std::size_t) channels);
        for (auto& channel : out)
            channel.resize(frames);

        for (std::size_t frame = 0; frame < frames; ++frame)
            for (int ch = 0; ch < channels; ++ch)
                out[(std::size_t) ch][frame] =
                    decodeSample(data + (frame * (std::size_t) channels + (std::size_t) ch) * (std::size_t) size,
                                 format.encoding, format.bigEndian);
    }
} // namespace rawpcm

} // namespace soundsplice::engine
