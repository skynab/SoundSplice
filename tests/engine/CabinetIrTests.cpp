#include <catch2/catch_test_macros.hpp>

#include <engine/CabinetIr.h>
#include <engine/Waveshaper.h>

#include <cmath>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;
}

TEST_CASE("The impulse train starts with the direct arrival", "[engine][cabinetir]")
{
    // Zero latency is the property that lets this be used on a guitar someone
    // is playing: the first output sample must depend on the first input one.
    const auto impulse = buildCabinetImpulseTrain(kSampleRate, 480);

    REQUIRE_FALSE(impulse.empty());
    REQUIRE(impulse[0] > 0.5f);
}

TEST_CASE("The reflections land where the geometry says", "[engine][cabinetir]")
{
    const auto impulse = buildCabinetImpulseTrain(kSampleRate, 480);

    for (const auto& reflection : cabinetReflections())
    {
        const auto at = (size_t) std::lround(reflection.delayMs * 0.001 * kSampleRate);
        REQUIRE(at < impulse.size());
        REQUIRE(std::abs(impulse[at]) > 0.05f);
    }
}

TEST_CASE("A reflection can be inverted", "[engine][cabinetir]")
{
    // Diffraction off the baffle edge arrives with opposite polarity, and it
    // is that sign which makes the comb notch rather than reinforce — a
    // reflection train with every arrival positive is just a brighter cabinet.
    const auto impulse = buildCabinetImpulseTrain(kSampleRate, 480);

    const auto at = (size_t) std::lround(0.34 * 0.001 * kSampleRate);
    REQUIRE(impulse[at] < 0.0f);
}

TEST_CASE("The response is deterministic", "[engine][cabinetir]")
{
    // Its tail is noise, and a cabinet that came out subtly different on each
    // launch would be a genuinely baffling bug to chase.
    REQUIRE(buildCabinetImpulseTrain(kSampleRate, 480)
            == buildCabinetImpulseTrain(kSampleRate, 480));
}

TEST_CASE("An empty convolver passes audio through", "[engine][cabinetir]")
{
    CabinetConvolver convolver;
    REQUIRE_FALSE(convolver.isReady());
    REQUIRE(convolver.processSample(0.5f) == 0.5f);
}

TEST_CASE("A unit impulse response is an identity", "[engine][cabinetir]")
{
    CabinetConvolver convolver;
    convolver.setImpulseResponse({ 1.0f, 0.0f, 0.0f, 0.0f });

    REQUIRE(convolver.processSample(0.5f) == 0.5f);
    REQUIRE(convolver.processSample(-0.25f) == -0.25f);
    REQUIRE(convolver.processSample(0.0f) == 0.0f);
}

TEST_CASE("The convolver computes an actual convolution", "[engine][cabinetir]")
{
    // A delayed, scaled copy: the simplest response whose output can be
    // written down by hand.
    CabinetConvolver convolver;
    convolver.setImpulseResponse({ 0.0f, 0.0f, 0.5f });

    REQUIRE(convolver.processSample(1.0f) == 0.0f); // still filling
    REQUIRE(convolver.processSample(0.0f) == 0.0f);
    REQUIRE(convolver.processSample(0.0f) == 0.5f); // two samples later, halved
    REQUIRE(convolver.processSample(0.0f) == 0.0f);
}

TEST_CASE("Normalising matches energy, not peak", "[engine][cabinetir]")
{
    std::vector<float> impulse { 1.0f, 1.0f, 1.0f, 1.0f }; // energy 2
    normaliseCabinetImpulse(impulse, 1.0);

    REQUIRE(std::abs(cabinetImpulseEnergy(impulse) - 1.0) < 1.0e-6);
}

TEST_CASE("Normalising an empty or silent response does nothing",
          "[engine][cabinetir]")
{
    std::vector<float> silent { 0.0f, 0.0f, 0.0f };
    normaliseCabinetImpulse(silent, 1.0);

    for (float tap : silent)
        REQUIRE(tap == 0.0f);
}

TEST_CASE("The two cabinet modes are level-matched", "[engine][cabinetir]")
{
    // The point of matching on energy: switching modes has to change the
    // sound and not the volume, or every comparison measures the level
    // difference instead.
    auto measure = [](bool useIr)
    {
        CabinetSim cabinet;
        cabinet.prepare(kSampleRate);
        cabinet.setUseImpulseResponse(useIr);

        // Broadband, so this is a level comparison rather than a comparison at
        // one frequency the comb might happen to notch.
        uint32_t noise = 0x2468aceu;
        double   energy = 0.0;

        for (int i = 0; i < (int) kSampleRate; ++i)
        {
            noise = noise * 1664525u + 1013904223u;
            const auto white = (float) ((int32_t) noise) * (1.0f / 2147483648.0f);
            const auto out   = cabinet.processSample(white * 0.25f);
            energy += (double) out * (double) out;
        }

        return std::sqrt(energy / kSampleRate);
    };

    const double filtered  = measure(false);
    const double convolved = measure(true);

    INFO("filtered " << filtered << " convolved " << convolved);
    REQUIRE(filtered > 1.0e-4);
    REQUIRE(convolved > filtered * 0.7);
    REQUIRE(convolved < filtered * 1.4);
}

TEST_CASE("The convolved cabinet is not the filtered one", "[engine][cabinetir]")
{
    // It must differ, or the whole exercise bought nothing — the difference
    // is the comb structure the filters cannot express.
    auto render = [](bool useIr)
    {
        CabinetSim cabinet;
        cabinet.prepare(kSampleRate);
        cabinet.setUseImpulseResponse(useIr);

        std::vector<float> out(2048);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = cabinet.processSample(i == 0 ? 1.0f : 0.0f);
        return out;
    };

    const auto filtered  = render(false);
    const auto convolved = render(true);

    double difference = 0.0;
    for (size_t i = 0; i < filtered.size(); ++i)
        difference += std::abs(filtered[i] - convolved[i]);

    REQUIRE(difference > 0.05);
}

TEST_CASE("The cabinet defaults to its filter chain", "[engine][cabinetir]")
{
    // Opt-in, so nothing that has not asked for it changes.
    CabinetSim cabinet;
    cabinet.prepare(kSampleRate);
    REQUIRE_FALSE(cabinet.usesImpulseResponse());
}
