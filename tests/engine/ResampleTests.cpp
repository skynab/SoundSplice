#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Resample.h>

#include <cmath>
#include <utility>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    std::vector<float> sine(double frequency, double rate, int frames)
    {
        std::vector<float> out((size_t) frames);
        for (int n = 0; n < frames; ++n)
            out[(size_t) n] = (float) std::sin(2.0 * kPi * frequency * n / rate);
        return out;
    }

    double rms(const std::vector<float>& samples, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) samples[i] * samples[i];
        return std::sqrt(sum / (double) (to - from));
    }
}

TEST_CASE("Resampling keeps the duration", "[engine][resample]")
{
    REQUIRE(Resampler(44100.0, 48000.0).outputLength(44100) == 48000);
    REQUIRE(Resampler(48000.0, 8000.0).outputLength(48000) == 8000);
    REQUIRE(Resampler(48000.0, 96000.0).outputLength(10) == 20);
    REQUIRE(Resampler(48000.0, 44100.0).outputLength(0) == 0);
}

TEST_CASE("A tone comes out as the same tone at the new rate", "[engine][resample]")
{
    for (auto [from, to] : { std::pair { 44100.0, 48000.0 }, std::pair { 48000.0, 44100.0 },
                             std::pair { 22050.0, 96000.0 }, std::pair { 96000.0, 32000.0 } })
    {
        INFO(from << " -> " << to);
        const Resampler resampler(from, to);
        const auto      output = resampler.processAll(sine(1000.0, from, (int) from / 2));
        const auto      wanted = sine(1000.0, to, (int) output.size());

        // Away from the ends, where the kernel runs off the file.
        for (size_t i = 200; i + 200 < output.size(); ++i)
            REQUIRE_THAT(output[i], WithinAbs(wanted[i], 2.0e-3));
    }
}

TEST_CASE("Constant level is kept, up or down", "[engine][resample]")
{
    for (double to : { 8000.0, 44100.0, 192000.0 })
    {
        const Resampler resampler(48000.0, to);
        const auto      output = resampler.processAll(std::vector<float>(48000, 0.5f));
        for (size_t i = output.size() / 4; i < output.size() * 3 / 4; ++i)
            REQUIRE_THAT(output[i], WithinAbs(0.5, 1.0e-3));
    }
}

TEST_CASE("Downsampling removes what the new rate can't hold instead of folding it back", "[engine][resample]")
{
    // 6 kHz is above 8 kHz's Nyquist; kept, it would alias to 2 kHz.
    const Resampler resampler(48000.0, 8000.0);
    const auto      output = resampler.processAll(sine(6000.0, 48000.0, 48000));
    REQUIRE(rms(output, 1000, output.size() - 1000) < 1.0e-3);
}

TEST_CASE("Converting a chunk at a time gives what converting it whole does", "[engine][resample]")
{
    const Resampler resampler(44100.0, 48000.0);
    auto            input = sine(3000.0, 44100.0, 20000);
    for (size_t i = 0; i < input.size(); i += 7)
        input[i] += 0.25f; // something less smooth than a sine

    const auto whole  = resampler.processAll(input);
    const auto length = (std::int64_t) input.size();

    std::vector<float> chunked(whole.size());
    for (std::int64_t out = 0; out < (std::int64_t) whole.size(); out += 1000)
    {
        const int    count = (int) std::min<std::int64_t>(1000, (std::int64_t) whole.size() - out);
        std::int64_t first = 0, end = 0;
        resampler.inputRangeFor(out, count, first, end);

        // Only the frames asked for, zero outside the file, as a reader gives them.
        std::vector<float> held((size_t) (end - first), 0.0f);
        for (std::int64_t i = first; i < end; ++i)
            if (i >= 0 && i < length)
                held[(size_t) (i - first)] = input[(size_t) i];

        resampler.process(held.data(), first, (int) held.size(), length, out, count, chunked.data() + out);
    }

    for (size_t i = 0; i < whole.size(); ++i)
        REQUIRE(chunked[i] == whole[i]);
}
