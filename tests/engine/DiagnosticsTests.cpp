#include <catch2/catch_test_macros.hpp>

#include <engine/Diagnostics.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;

namespace
{
    constexpr double kRate = 48000.0;

    /** Runs @p signal through a scanner a chunk at a time, as a scan does. */
    std::vector<diagnostics::Issue> scan(const std::vector<float>& signal, int chunk = 10000)
    {
        diagnostics::Scanner scanner;
        scanner.prepare(kRate, {});
        for (size_t at = 0; at < signal.size(); at += (size_t) chunk)
        {
            const int    n          = (int) std::min<size_t>((size_t) chunk, signal.size() - at);
            const float* channels[] = { signal.data() + at, signal.data() + at };
            scanner.process(channels, 2, n);
        }
        return scanner.finish();
    }

    std::vector<float> tone(double seconds, float level = 0.3f)
    {
        std::vector<float> out((size_t) (seconds * kRate));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = level * (float) std::sin(2.0 * 3.14159265358979 * 220.0 * (double) i / kRate);
        return out;
    }

    int count(const std::vector<diagnostics::Issue>& issues, diagnostics::Kind kind)
    {
        int n = 0;
        for (const auto& issue : issues)
            n += issue.kind == kind ? 1 : 0;
        return n;
    }
}

TEST_CASE("A clean tone has nothing wrong with it", "[engine][diagnostics]")
{
    REQUIRE(scan(tone(3.0)).empty());
}

TEST_CASE("Clicks are found where they are, across chunk and block edges", "[engine][diagnostics]")
{
    auto signal = tone(4.0);
    const std::vector<int> at { 30000, 65530, 131000 }; // the second near a block edge
    for (const int i : at)
        signal[(size_t) i] += 0.6f;

    const auto issues = scan(signal);
    REQUIRE(count(issues, diagnostics::Kind::Click) == 3);
    int k = 0;
    for (const auto& issue : issues)
        if (issue.kind == diagnostics::Kind::Click)
        {
            REQUIRE(issue.from <= at[(size_t) k]);
            REQUIRE(issue.to > at[(size_t) k]);
            ++k;
        }
}

TEST_CASE("Clipping, silence and DC offset are each found", "[engine][diagnostics]")
{
    auto signal = tone(4.0, 0.3f);
    for (size_t i = 20000; i < 20100; ++i)
        signal[i] = 1.0f; // clipped flat
    for (size_t i = (size_t) (2.0 * kRate); i < (size_t) (3.5 * kRate); ++i)
        signal[i] = 0.0f; // a second and a half of silence

    auto issues = scan(signal);
    REQUIRE(count(issues, diagnostics::Kind::Clipping) == 1);
    REQUIRE(count(issues, diagnostics::Kind::Silence) == 1);
    REQUIRE(count(issues, diagnostics::Kind::DcOffset) == 0);

    for (auto& sample : signal)
        sample += 0.02f; // a DC offset
    issues = scan(signal);
    REQUIRE(count(issues, diagnostics::Kind::DcOffset) == 1);
    REQUIRE(count(issues, diagnostics::Kind::Silence) == 0); // offset silence isn't silent
    REQUIRE(issues.front().from <= issues.back().from);      // time order
}
