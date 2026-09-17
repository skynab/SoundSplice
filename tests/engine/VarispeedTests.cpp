#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Varispeed.h>

#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    /** Plays a ramp (sample n has value n) through @p varispeed in output
        blocks of @p block frames, feeding input blocks of @p inputBlock, as the
        engine does. Returns everything pulled. */
    std::vector<float> playRamp(Varispeed& varispeed, double speed, int blocks, int block, int inputBlock)
    {
        std::vector<float> output;
        std::vector<float> in((size_t) inputBlock), out((size_t) block);
        float              next = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            for (int need = varispeed.framesNeeded(block, speed); need > 0;)
            {
                const int n = std::min(need, inputBlock);
                for (int i = 0; i < n; ++i)
                    in[(size_t) i] = next++;
                const float* channels[] { in.data() };
                REQUIRE(varispeed.push(channels, 1, n) == n);
                need -= n;
            }

            float* channels[] { out.data() };
            varispeed.pull(channels, 1, block, speed);
            output.insert(output.end(), out.begin(), out.end());
        }
        return output;
    }
}

TEST_CASE("At normal speed the output is the input", "[engine][varispeed]")
{
    Varispeed varispeed;
    varispeed.prepare(1, 64, 32);

    const auto output = playRamp(varispeed, 1.0, 10, 64, 32);
    for (size_t i = 0; i < output.size(); ++i)
        REQUIRE_THAT(output[i], WithinAbs((double) i, 1e-4));
}

TEST_CASE("Faster and slower read the song at that rate, continuously across blocks", "[engine][varispeed]")
{
    for (double speed : { 2.0, 0.5, 1.37, 0.25, 4.0 })
    {
        INFO("speed " << speed);
        for (int block : { 64, 37 })
        {
            Varispeed varispeed;
            varispeed.prepare(1, block, 48);

            const auto output = playRamp(varispeed, speed, 20, block, 48);
            for (size_t i = 0; i < output.size(); ++i)
                REQUIRE_THAT(output[i], WithinAbs((double) i * speed, 1e-2));
        }
    }
}

TEST_CASE("Speeds are kept to the range, and a reset drops what was held", "[engine][varispeed]")
{
    REQUIRE(Varispeed::clampSpeed(10.0) == Varispeed::kMaxSpeed);
    REQUIRE(Varispeed::clampSpeed(0.0) == Varispeed::kMinSpeed);
    REQUIRE(Varispeed::clampSpeed(std::nan("")) == 1.0);

    Varispeed varispeed;
    varispeed.prepare(2, 16, 16);
    REQUIRE(varispeed.framesNeeded(16, 1.0) == 17);

    std::vector<float> in(16, 0.5f);
    const float*       channels[] { in.data() }; // one channel given: both get it
    varispeed.push(channels, 1, 16);
    REQUIRE(varispeed.available() == 16);

    varispeed.reset();
    REQUIRE(varispeed.available() == 0);
    REQUIRE(varispeed.framesNeeded(16, 1.0) == 17);
}

TEST_CASE("Play Faster and Play Slower step through the speeds", "[engine][varispeed]")
{
    REQUIRE(Varispeed::steppedSpeed(1.0, 1) == 1.25);
    REQUIRE(Varispeed::steppedSpeed(1.0, -1) == 0.75);
    REQUIRE(Varispeed::steppedSpeed(1.1, -1) == 1.0);
    REQUIRE(Varispeed::steppedSpeed(4.0, 1) == 4.0);
    REQUIRE(Varispeed::steppedSpeed(0.25, -1) == 0.25);
}
