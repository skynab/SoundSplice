#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/Convolver.h"

#include <random>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE("The convolver matches direct convolution, a block late", "[engine][convolution]")
{
    std::mt19937                          random(7);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);

    // A response several partitions long, not a whole number of them.
    std::vector<float> impulse(Convolver::kBlock * 5 + 37);
    for (auto& s : impulse)
        s = uniform(random) * 0.2f;
    std::vector<float> input(Convolver::kBlock * 12 + 11);
    for (auto& s : input)
        s = uniform(random);

    Convolver convolver;
    convolver.setImpulse(impulse);
    REQUIRE(convolver.isReady());

    std::vector<float> output;
    for (float x : input)
        output.push_back(convolver.processSample(x));

    for (size_t n = 0; n < output.size(); ++n)
    {
        // Output n is the convolution at n - kBlock.
        double expected = 0.0;
        if (n >= (size_t) Convolver::kBlock)
        {
            const size_t at = n - (size_t) Convolver::kBlock;
            for (size_t k = 0; k < impulse.size() && k <= at; ++k)
                expected += (double) impulse[k] * input[at - k];
        }
        INFO("sample " << n);
        REQUIRE_THAT(output[n], WithinAbs(expected, 1.0e-3));
    }
}

TEST_CASE("A single-sample response is a plain delay; an empty one is silence", "[engine][convolution]")
{
    Convolver unit;
    unit.setImpulse({ 1.0f });
    std::vector<float> out;
    for (int n = 0; n < Convolver::kBlock * 3; ++n)
        out.push_back(unit.processSample(n == 10 ? 1.0f : 0.0f));
    for (int n = 0; n < (int) out.size(); ++n)
        REQUIRE_THAT(out[(size_t) n], WithinAbs(n == 10 + Convolver::kBlock ? 1.0 : 0.0, 1.0e-5));

    Convolver silent;
    silent.setImpulse({});
    for (int n = 0; n < Convolver::kBlock * 2; ++n)
        REQUIRE(silent.processSample(1.0f) == 0.0f);
}

TEST_CASE("The built-in hall is stereo, decays and is normalised", "[engine][convolution]")
{
    const auto hall = convolution::syntheticHall(48000.0, 1.0);
    REQUIRE(hall.size() == 2);
    REQUIRE(hall[0] != hall[1]); // different in each ear, so it's wide

    double energy = 0.0, early = 0.0, late = 0.0;
    for (size_t i = 0; i < hall[0].size(); ++i)
    {
        const double e = (double) hall[0][i] * hall[0][i];
        energy += e;
        if (i < 4800)
            early += e;
        if (i >= hall[0].size() - 4800)
            late += e;
    }
    REQUIRE_THAT(energy, WithinAbs(1.0, 0.1)); // both channels together are normalised
    REQUIRE(late < early * 1.0e-3); // quieter by more than 30 dB at the end
}
