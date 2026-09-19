#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/Fft.h"
#include "engine/Generators.h"
#include "engine/RoomTone.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    std::vector<float> generate(const GeneratorSpec& spec)
    {
        Generator          generator(spec, kRate);
        std::vector<float> out((size_t) generator.totalFrames());
        generator.render(out.data(), (int) out.size());
        return out;
    }

    /** The strongest frequency in @p samples [from, from + 8192), to a
        fraction of a bin by parabolic interpolation. */
    double peakHz(const std::vector<float>& samples, size_t from)
    {
        constexpr int      n = 8192;
        std::vector<float> re(n), im(n, 0.0f);
        for (int i = 0; i < n; ++i)
            re[(size_t) i] = samples[from + (size_t) i] * (float) (0.5 - 0.5 * std::cos(2.0 * fft::kPi * i / n));
        fft::transform(re, im, false);

        int best = 1;
        for (int k = 1; k < n / 2 - 1; ++k)
            if (std::hypot(re[(size_t) k], im[(size_t) k]) > std::hypot(re[(size_t) best], im[(size_t) best]))
                best = k;
        const auto mag = [&](int k) { return std::log(std::hypot(re[(size_t) k], im[(size_t) k]) + 1e-12); };
        const double a = mag(best - 1), b = mag(best), c = mag(best + 1);
        return (best + 0.5 * (a - c) / (a - 2.0 * b + c)) * kRate / n;
    }

    double rms(const std::vector<float>& samples, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) samples[i] * samples[i];
        return std::sqrt(sum / (double) (to - from));
    }
}

TEST_CASE("Pluck rings at its pitch, and dies faster with more decay", "[engine][generators]")
{
    for (double hz : { 110.0, 440.0, 1234.5 })
    {
        GeneratorSpec spec;
        spec.kind    = GeneratorKind::Pluck;
        spec.startHz = hz;
        spec.seconds = 1.0;
        const auto out = generate(spec);
        // A fresh pluck is bright, so its strongest partial may be any
        // harmonic: whichever it is sits on an exact multiple of the pitch,
        // tuned rather than rounded to a whole period.
        const double peak     = peakHz(out, 4800);
        const double harmonic = std::max(1.0, std::round(peak / hz));
        INFO(hz << " Hz, strongest at " << peak);
        REQUIRE_THAT(peak, WithinAbs(harmonic * hz, peak * 0.003));
    }

    const auto ringAfter = [](double decay)
    {
        GeneratorSpec spec;
        spec.kind       = GeneratorKind::Pluck;
        spec.startHz    = 220.0;
        spec.pluckDecay = decay;
        spec.seconds    = 2.0;
        const auto out  = generate(spec);
        return rms(out, 48000, 52800) / rms(out, 0, 4800);
    };
    REQUIRE(ringAfter(1.0) < ringAfter(0.0) * 0.1);
    REQUIRE(ringAfter(0.0) > 0.02); // still ringing a second in, past its bright attack
}

TEST_CASE("Room tone recreates a captured room's level and colour without repeating", "[engine][generators]")
{
    // The "room": brown-ish noise, loud in the bass and quiet up top.
    std::mt19937                          random(3);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    std::vector<float>                    room((size_t) kRate * 3);
    float                                 low = 0.0f;
    for (auto& s : room)
    {
        low += 0.05f * (uniform(random) - low);
        s = low * 0.2f + uniform(random) * 0.002f;
    }

    const auto profile = RoomToneProfile::capture(room);
    REQUIRE_FALSE(profile.isEmpty());
    REQUIRE(RoomToneProfile::capture(std::vector<float>(100)).isEmpty());

    GeneratorSpec spec;
    spec.kind     = GeneratorKind::RoomTone;
    spec.roomTone = std::make_shared<const RoomToneProfile>(profile);
    spec.seconds  = 4.0;
    const auto tone = generate(spec);

    // The same level, within a decibel, away from the fades at each end.
    const double level = 20.0 * std::log10(rms(tone, 4800, tone.size() - 4800) / rms(room, 0, room.size()));
    REQUIRE_THAT(level, WithinAbs(0.0, 1.0));

    // The same colour: its own profile matches the room's, band by band.
    const auto again = RoomToneProfile::capture(std::vector<float>(tone.begin() + 4800, tone.end() - 4800));
    const auto bandDb = [](const RoomToneProfile& p, int from, int to)
    {
        double sum = 0.0;
        for (int k = from; k < to; ++k)
            sum += p.power[(size_t) k];
        return 10.0 * std::log10(sum);
    };
    for (auto [from, to] : { std::pair { 2, 20 }, std::pair { 40, 120 }, std::pair { 400, 1000 } })
        REQUIRE_THAT(bandDb(again, from, to) - bandDb(profile, from, to), WithinAbs(0.0, 1.5));

    // Faded in and out, so it drops into a gap without a click.
    REQUIRE(std::abs(tone.front()) < 1.0e-6f);
    REQUIRE(std::abs(tone.back()) < 1.0e-6f);

    // And not a loop: two stretches a frame apart don't match.
    double same = 0.0;
    for (size_t i = 0; i < 2048; ++i)
        same += std::abs(tone[48000 + i] - tone[48000 + 2048 + i]);
    REQUIRE(same > 1.0e-3);
}
