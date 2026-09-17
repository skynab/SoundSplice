#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/RawPcm.h>

#include <cstring>

using namespace soundsplice::engine;
using namespace soundsplice::engine::rawpcm;
using Catch::Matchers::WithinAbs;

TEST_CASE("Integer PCM decodes to full scale in either byte order", "[engine][rawpcm]")
{
    const std::uint8_t s16le[] { 0x00, 0x40 }; // 16384
    const std::uint8_t s16be[] { 0x40, 0x00 };
    REQUIRE_THAT(decodeSample(s16le, RawEncoding::Signed16, false), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(decodeSample(s16be, RawEncoding::Signed16, true), WithinAbs(0.5, 1e-6));

    const std::uint8_t s16min[] { 0x00, 0x80 };
    REQUIRE_THAT(decodeSample(s16min, RawEncoding::Signed16, false), WithinAbs(-1.0, 1e-6));

    const std::uint8_t s24le[] { 0x00, 0x00, 0xc0 }; // -4194304
    REQUIRE_THAT(decodeSample(s24le, RawEncoding::Signed24, false), WithinAbs(-0.5, 1e-6));

    const std::uint8_t s32be[] { 0x40, 0x00, 0x00, 0x00 };
    REQUIRE_THAT(decodeSample(s32be, RawEncoding::Signed32, true), WithinAbs(0.5, 1e-6));

    const std::uint8_t u8[] { 0xc0 };
    REQUIRE_THAT(decodeSample(u8, RawEncoding::Unsigned8, false), WithinAbs(0.5, 1e-6));
    const std::uint8_t s8[] { 0xc0 };
    REQUIRE_THAT(decodeSample(s8, RawEncoding::Signed8, false), WithinAbs(-0.5, 1e-6));
}

TEST_CASE("Float PCM decodes as it is, and never as a NaN", "[engine][rawpcm]")
{
    std::uint8_t bytes[8] {};
    const float  quarter = 0.25f;
    std::memcpy(bytes, &quarter, 4); // this machine's order, little-endian on every platform built for
    REQUIRE_THAT(decodeSample(bytes, RawEncoding::Float32, false), WithinAbs(0.25, 1e-7));

    const double minusHalf = -0.5;
    std::memcpy(bytes, &minusHalf, 8);
    REQUIRE_THAT(decodeSample(bytes, RawEncoding::Float64, false), WithinAbs(-0.5, 1e-7));

    const std::uint8_t nan[] { 0x00, 0x00, 0xc0, 0x7f };
    REQUIRE(decodeSample(nan, RawEncoding::Float32, false) == 0.0f);
}

TEST_CASE("U-law and A-law decode their silence and their extremes", "[engine][rawpcm]")
{
    REQUIRE(std::abs(fromMuLaw(0xff)) < 1.0e-6f);
    REQUIRE(fromMuLaw(0x80) > 0.95f);
    REQUIRE(fromMuLaw(0x00) < -0.95f);

    REQUIRE(std::abs(fromALaw(0xd5)) < 0.001f);
    REQUIRE(std::abs(fromALaw(0x55)) < 0.001f);
    REQUIRE(fromALaw(0xaa) > 0.95f);
    REQUIRE(fromALaw(0x2a) < -0.95f);
}

TEST_CASE("Frames are split into channels, skipping the header and a partial frame", "[engine][rawpcm]")
{
    RawPcmFormat format;
    format.encoding    = RawEncoding::Signed16;
    format.channels    = 2;
    format.headerBytes = 4;

    REQUIRE(format.frameBytes() == 4);
    REQUIRE(format.framesIn(4 + 8 + 3) == 2);
    REQUIRE(format.framesIn(3) == 0);

    // Left 0.5 then -0.5; right -1 then 0.
    const std::uint8_t data[] { 0x00, 0x40, 0x00, 0x80, 0x00, 0xc0, 0x00, 0x00 };
    std::vector<std::vector<float>> channels;
    decodeFrames(data, 2, format, channels);

    REQUIRE(channels.size() == 2);
    REQUIRE_THAT(channels[0][0], WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(channels[0][1], WithinAbs(-0.5, 1e-6));
    REQUIRE_THAT(channels[1][0], WithinAbs(-1.0, 1e-6));
    REQUIRE_THAT(channels[1][1], WithinAbs(0.0, 1e-6));
}
