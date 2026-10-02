#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/DeliverySpec.h>

#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double kRate = 48000.0;

    /** @p head seconds of room noise at @p noiseDb, @p body seconds of a
        tone at @p level, then @p tail seconds of the room again. */
    std::vector<float> programme(double head, double body, double tail, float level, double noiseDb)
    {
        std::mt19937                     random(7);
        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
        const float noiseLevel = (float) std::pow(10.0, noiseDb / 20.0) * std::sqrt(3.0f); // uniform noise: RMS = level
        std::vector<float> out((size_t) ((head + body + tail) * kRate));
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = i / kRate;
            float v = noiseLevel * noise(random);
            if (t >= head && t < head + body)
                v += level * (float) std::sin(2.0 * 3.14159265358979 * 300.0 * t);
            out[i] = v;
        }
        return out;
    }

    const delivery::Spec& spec(const char* name)
    {
        for (const auto& s : delivery::all())
            if (std::string(s.name) == name)
                return s;
        FAIL("no spec " << name);
        return delivery::all().front();
    }

    const delivery::Result& line(const std::vector<delivery::Result>& results, const char* measure)
    {
        for (const auto& r : results)
            if (r.measure == measure)
                return r;
        FAIL("no line " << measure);
        return results.front();
    }
}

TEST_CASE("Every delivery spec has a name, an export target that meets it, and checks", "[engine][delivery]")
{
    std::set<std::string> names;
    for (const auto& s : delivery::all())
    {
        INFO(s.name);
        REQUIRE(names.insert(s.name).second);
        // The export it suggests lands inside its own loudness range.
        if (std::isfinite(s.lufsMin))
            REQUIRE((s.exportLufs >= s.lufsMin && s.exportLufs <= s.lufsMax));
        REQUIRE(s.exportCeilingDb <= std::min(s.truePeakMax, s.samplePeakMax));
        REQUIRE_FALSE(delivery::check(s, {}).empty());
    }
}

TEST_CASE("A mix is measured: loudness, peaks, noise floor, head and tail", "[engine][delivery]")
{
    // 0.7 s of -70 dBFS room, 8 s of a tone at 0.2, 2 s of room.
    const auto audio = programme(0.7, 8.0, 2.0, 0.2f, -70.0);
    const auto m     = delivery::measure(audio.data(), audio.data(), (int) audio.size(), kRate);

    REQUIRE_THAT(m.seconds, WithinAbs(10.7, 1e-3));
    REQUIRE_THAT(m.samplePeakDb, WithinAbs(20.0 * std::log10(0.2), 0.1));
    REQUIRE_THAT(m.noiseFloorDb, WithinAbs(-70.0, 1.5));
    REQUIRE_THAT(m.headSeconds, WithinAbs(0.7, 0.02));
    REQUIRE_THAT(m.tailSeconds, WithinAbs(2.0, 0.02));
    REQUIRE(std::isfinite(m.integratedLufs));
}

TEST_CASE("ACX passes a mix that meets it, and says why one doesn't", "[engine][delivery]")
{
    const auto& acx = spec("ACX (Audible)");

    // RMS of a 0.141 sine is about -20 dBFS; peak -17; room at -70; 0.7 s and 2 s.
    auto good = programme(0.7, 20.0, 2.0, 0.141f, -70.0);
    auto m    = delivery::measure(good.data(), good.data(), (int) good.size(), kRate);
    auto r    = delivery::check(acx, m);
    INFO(r[0].measured << " " << r[1].measured << " " << r[2].measured);
    REQUIRE(delivery::passes(r));

    // Too loud a peak, a noisy room, and no room tone at the start.
    auto bad = programme(0.1, 20.0, 2.0, 0.9f, -50.0);
    m        = delivery::measure(bad.data(), bad.data(), (int) bad.size(), kRate);
    r        = delivery::check(acx, m);
    REQUIRE_FALSE(delivery::passes(r));
    REQUIRE_FALSE(line(r, "Peak").pass);
    REQUIRE_FALSE(line(r, "Noise floor").pass);
    REQUIRE_FALSE(line(r, "Room tone at the start").pass);
    REQUIRE(line(r, "Room tone at the start").advice.find("Room Tone") != std::string::npos);
    REQUIRE(line(r, "Room tone at the end").pass);
}

TEST_CASE("A loudness spec checks integrated loudness and true peak", "[engine][delivery]")
{
    const auto& apple = spec("Apple Podcasts");
    auto audio = programme(0.5, 20.0, 0.5, 0.1f, -80.0);
    const auto m = delivery::measure(audio.data(), audio.data(), (int) audio.size(), kRate);
    const auto r = delivery::check(apple, m);
    REQUIRE(r.size() == 2); // loudness and true peak: nothing else is asked
    REQUIRE(line(r, "Integrated loudness").pass == (m.integratedLufs >= -17.0 && m.integratedLufs <= -15.0));
    REQUIRE(line(r, "True peak").pass);
}
