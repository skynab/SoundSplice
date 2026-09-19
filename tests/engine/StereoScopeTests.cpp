#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/StereoScope.h"

#include <cmath>
#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    /** The correlation after a second of @p left and @p right, made per sample. */
    template <typename Make>
    double correlationOf(Make&& make)
    {
        PhaseCorrelation meter;
        meter.prepare(kRate);
        std::vector<float> left((size_t) kRate), right((size_t) kRate);
        for (size_t i = 0; i < left.size(); ++i)
            std::tie(left[i], right[i]) = make(i);
        meter.process(left.data(), right.data(), (int) left.size());
        return meter.correlation();
    }
}

TEST_CASE("Phase correlation reads mono as +1, inverted as -1 and unrelated as 0", "[engine][scope]")
{
    const auto sine = [](size_t i) { return (float) std::sin(2.0 * 3.14159265358979 * 440.0 * (double) i / kRate); };

    REQUIRE_THAT(correlationOf([&](size_t i) { return std::pair { sine(i), sine(i) * 0.5f }; }), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(correlationOf([&](size_t i) { return std::pair { sine(i), -sine(i) }; }), WithinAbs(-1.0, 1e-6));

    std::mt19937                          random(4);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    REQUIRE_THAT(correlationOf([&](size_t) { return std::pair { uniform(random), uniform(random) }; }), WithinAbs(0.0, 0.1));

    // One side silent: nothing to correlate, so 0 rather than a guess.
    REQUIRE(correlationOf([&](size_t i) { return std::pair { sine(i), 0.0f }; }) == 0.0);

    // A quarter cycle apart: two sines in quadrature have nothing in common.
    const auto cosine = [](size_t i) { return (float) std::cos(2.0 * 3.14159265358979 * 440.0 * (double) i / kRate); };
    REQUIRE_THAT(correlationOf([&](size_t i) { return std::pair { sine(i), cosine(i) }; }), WithinAbs(0.0, 0.05));
}

TEST_CASE("The scope ring hands back the latest pairs, oldest first", "[engine][scope]")
{
    ScopeRing ring;
    std::vector<float> left(ScopeRing::kSize + 100), right(left.size());
    for (size_t i = 0; i < left.size(); ++i)
    {
        left[i]  = (float) i;
        right[i] = -(float) i;
    }
    // In two pushes, the second wrapping round.
    ring.push(left.data(), right.data(), 500);
    ring.push(left.data() + 500, right.data() + 500, (int) left.size() - 500);

    std::vector<std::pair<float, float>> pairs;
    ring.copy(pairs);
    REQUIRE(pairs.size() == (size_t) ScopeRing::kSize);
    REQUIRE(pairs.front().first == 100.0f);
    REQUIRE(pairs.back().first == (float) (left.size() - 1));
    REQUIRE(pairs.back().second == -(float) (left.size() - 1));
}
