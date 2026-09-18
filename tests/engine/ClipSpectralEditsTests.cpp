#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/ClipSpectralEdits.h"

#include <cmath>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr double kPi   = 3.14159265358979;

    double levelAt(const std::vector<float>& audio, double hz, int from, int to)
    {
        double re = 0.0, im = 0.0;
        for (int i = from; i < to; ++i)
        {
            re += audio[(size_t) i] * std::cos(2.0 * kPi * hz * i / kRate);
            im += audio[(size_t) i] * std::sin(2.0 * kPi * hz * i / kRate);
        }
        return 2.0 * std::hypot(re, im) / (to - from);
    }
}

TEST_CASE("A clip's spectral edits apply where they are in the file, from any starting frame", "[spectral]")
{
    std::vector<float> file(48000 * 4);
    for (size_t i = 0; i < file.size(); ++i)
        file[i] = 0.3f * (float) std::sin(2.0 * kPi * 1000.0 * (double) i / kRate)
                + 0.3f * (float) std::sin(2.0 * kPi * 6000.0 * (double) i / kRate);

    SpectralRegion out;
    out.startSeconds = 1.5;
    out.endSeconds   = 2.5;
    out.lowHz        = 4000.0;
    out.highHz       = 8000.0;
    out.gainDb       = SpectralRegion::kSilenceDb;
    REQUIRE(out.gain() == 0.0f);

    SpectralRegion quieter = out;
    quieter.startSeconds = 3.0;
    quieter.endSeconds   = 3.8;
    quieter.lowHz        = 500.0;
    quieter.highHz       = 2000.0;
    quieter.gainDb       = -6.0f;

    const SpectralRegions regions { out, quieter };

    // The same edit on the whole file and on a piece of it read from 1 s on
    // (as the render reads only what the edits touch).
    auto whole = file;
    REQUIRE(clipspectral::apply(whole, 0, kRate, regions));

    std::vector<float> piece(file.begin() + 48000, file.begin() + 48000 * 3);
    REQUIRE(clipspectral::apply(piece, 48000, kRate, regions));

    REQUIRE(levelAt(whole, 6000.0, 80000, 112000) < 0.01);
    REQUIRE_THAT(levelAt(whole, 1000.0, 80000, 112000), WithinAbs(0.3, 0.01));
    REQUIRE_THAT(levelAt(whole, 1000.0, 148000, 178000), WithinAbs(0.3 * 0.501, 0.01));
    REQUIRE_THAT(levelAt(whole, 6000.0, 4000, 60000), WithinAbs(0.3, 0.01));
    for (size_t i = 0; i < piece.size(); i += 101)
        REQUIRE_THAT(piece[i], WithinAbs(whole[i + 48000], 1e-4));
}

TEST_CASE("The render edits only the stretches the regions touch", "[spectral]")
{
    SpectralRegion a;
    a.startSeconds = 1.0;
    a.endSeconds   = 1.2;
    a.lowHz        = 100.0;
    a.highHz       = 200.0;
    SpectralRegion b = a;
    b.startSeconds   = 1.1;
    b.endSeconds     = 1.4;
    SpectralRegion c = a;
    c.startSeconds   = 5.0;
    c.endSeconds     = 5.5;
    SpectralRegion invalid;

    const auto spans = clipspectral::spans({ c, a, b, invalid }, kRate, (std::int64_t) (kRate * 5.2));
    REQUIRE(spans.size() == 2);
    REQUIRE(spans[0].first == 48000 - clipspectral::kContextFrames);
    REQUIRE(spans[0].second == (std::int64_t) (1.4 * kRate) + clipspectral::kContextFrames);
    REQUIRE(spans[1].first == 240000 - clipspectral::kContextFrames);
    REQUIRE(spans[1].second == (std::int64_t) (kRate * 5.2)); // cut at the end of the file
}
