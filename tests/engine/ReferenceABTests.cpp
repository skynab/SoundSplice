#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/ReferenceAB.h>

#include <cmath>
#include <vector>

using namespace soundsplice::engine;
using Catch::Matchers::WithinAbs;

namespace
{
    /** A block of the mix: every sample 0.5. */
    struct Block
    {
        std::vector<float> left = std::vector<float>(64, 0.5f), right = std::vector<float>(64, 0.5f);
        float* channels[2] { left.data(), right.data() };
    };

    /** A reference whose sample n is n / 1000, at @p rate. */
    ReferenceAudio* ramp(double rate, int length)
    {
        auto* audio       = new ReferenceAudio();
        audio->sampleRate = rate;
        audio->channels.emplace_back();
        for (int i = 0; i < length; ++i)
            audio->channels[0].push_back((float) i / 1000.0f);
        return audio;
    }
}

TEST_CASE("The louder of mix and reference is turned down to the quieter", "[engine][reference]")
{
    float mix = 0.0f, reference = 0.0f;
    matchedGains(-14.0, -8.0, mix, reference); // the reference 6 dB louder
    REQUIRE(mix == 1.0f);
    REQUIRE_THAT(reference, WithinAbs(std::pow(10.0, -6.0 / 20.0), 1e-6));

    matchedGains(-10.0, -16.0, mix, reference); // the mix louder
    REQUIRE_THAT(mix, WithinAbs(std::pow(10.0, -6.0 / 20.0), 1e-6));
    REQUIRE(reference == 1.0f);

    matchedGains(-10.0, -INFINITY, mix, reference); // nothing to match
    REQUIRE((mix == 1.0f && reference == 1.0f));
}

TEST_CASE("A/B leaves the mix, turns it down, or plays the reference at the playhead", "[engine][reference]")
{
    ReferenceAB ab;
    ab.setAudio(ramp(48000.0, 10000));
    ab.setGains(0.5f, 2.0f);

    // Off: the mix as it is (and the reference adopted along the way).
    Block off;
    ab.process(off.channels, 2, 64, 0, 48000.0, true);
    REQUIRE(off.left[10] == 0.5f);

    // A: the mix at its matched level.
    ab.setMode(ReferenceAB::A);
    Block a;
    ab.process(a.channels, 2, 64, 0, 48000.0, true);
    REQUIRE(a.left[10] == 0.25f);

    // B: the reference from the playhead, at its level, on both channels.
    ab.setMode(ReferenceAB::B);
    Block b;
    ab.process(b.channels, 2, 64, 500, 48000.0, true);
    REQUIRE_THAT(b.left[0], WithinAbs(2.0 * 0.5, 1e-6));    // sample 500
    REQUIRE_THAT(b.right[10], WithinAbs(2.0 * 0.51, 1e-6)); // sample 510, mono on both

    // At another rate the reference keeps time: device 96k, reference 48k.
    Block slow;
    ab.process(slow.channels, 2, 64, 1000, 96000.0, true);
    REQUIRE_THAT(slow.left[0], WithinAbs(2.0 * 0.5, 1e-5)); // halfway through the same second
    REQUIRE_THAT(slow.left[1], WithinAbs(2.0 * 0.5005, 1e-5)); // between two samples

    // It loops past its end.
    Block looped;
    ab.process(looped.channels, 2, 64, 10000 + 3, 48000.0, true);
    REQUIRE_THAT(looped.left[0], WithinAbs(2.0 * 0.003, 1e-6));

    // Stopped: silence, not a held sample.
    Block stopped;
    ab.process(stopped.channels, 2, 64, 500, 48000.0, false);
    REQUIRE(stopped.left[0] == 0.0f);

    // A new reference replaces the old one; the old is reclaimed.
    ab.setAudio(ramp(48000.0, 100));
    Block replaced;
    ab.process(replaced.channels, 2, 64, 150, 48000.0, true);
    REQUIRE_THAT(replaced.left[0], WithinAbs(2.0 * 0.05, 1e-6)); // 150 wraps to 50
    ab.collectRetired();
}
