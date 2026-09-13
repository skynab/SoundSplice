#include <catch2/catch_test_macros.hpp>

#include <engine/AudioEdits.h>

#include <cmath>
#include <numeric>

using namespace looper::engine;

namespace
{
    /** 0,1,2,3,... so any reordering or off-by-one is visible in the values
        themselves rather than needing to be inferred. */
    std::vector<float> counting(int count)
    {
        std::vector<float> out((size_t) count);
        std::iota(out.begin(), out.end(), 0.0f);
        return out;
    }
}

// --- removeRange / keepRange / extractRange --------------------------------

TEST_CASE("Removing a range shortens by exactly that range", "[engine][audioedits]")
{
    const auto out = audioedits::removeRange(counting(10), 3, 7);
    REQUIRE(out.size() == 6);
    REQUIRE(out == std::vector<float> { 0, 1, 2, 7, 8, 9 });
}

TEST_CASE("Keeping a range gives exactly that range", "[engine][audioedits]")
{
    const auto out = audioedits::keepRange(counting(10), 3, 7);
    REQUIRE(out == std::vector<float> { 3, 4, 5, 6 });
}

TEST_CASE("Ranges are half-open", "[engine][audioedits]")
{
    // [from, to) — the sample at `to` survives a remove and is absent from a
    // keep. Getting this backwards is a one-sample error nobody would hear
    // but which accumulates over repeated edits.
    REQUIRE(audioedits::removeRange(counting(5), 1, 2) == std::vector<float> { 0, 2, 3, 4 });
    REQUIRE(audioedits::keepRange(counting(5), 1, 2) == std::vector<float> { 1 });
}

TEST_CASE("A backwards range is treated as ordered", "[engine][audioedits]")
{
    // Dragging right-to-left is as common as left-to-right; AudioRange
    // normalises, but this must not corrupt if handed a raw pair.
    REQUIRE(audioedits::removeRange(counting(10), 7, 3)
            == audioedits::removeRange(counting(10), 3, 7));
}

TEST_CASE("Ranges outside the buffer clamp rather than overrun", "[engine][audioedits]")
{
    REQUIRE(audioedits::removeRange(counting(5), -100, 2) == std::vector<float> { 2, 3, 4 });
    REQUIRE(audioedits::removeRange(counting(5), 3, 9999) == std::vector<float> { 0, 1, 2 });
    REQUIRE(audioedits::keepRange(counting(5), -10, 9999).size() == 5);
    REQUIRE(audioedits::removeRange({}, 0, 10).empty());
}

TEST_CASE("An empty range changes nothing", "[engine][audioedits]")
{
    const auto original = counting(6);
    REQUIRE(audioedits::removeRange(original, 3, 3) == original);
    REQUIRE(audioedits::silenceRange(original, 3, 3) == original);
    REQUIRE(audioedits::reverseRange(original, 3, 3) == original);
    REQUIRE(audioedits::keepRange(original, 3, 3).empty());
}

// --- insertAt --------------------------------------------------------------

TEST_CASE("Inserting splices at the right place", "[engine][audioedits]")
{
    const auto out = audioedits::insertAt(counting(4), { 90.0f, 91.0f }, 2);
    REQUIRE(out == std::vector<float> { 0, 1, 90, 91, 2, 3 });
}

TEST_CASE("Inserting at either end works", "[engine][audioedits]")
{
    REQUIRE(audioedits::insertAt(counting(3), { 9 }, 0) == std::vector<float> { 9, 0, 1, 2 });
    REQUIRE(audioedits::insertAt(counting(3), { 9 }, 3) == std::vector<float> { 0, 1, 2, 9 });
    // Past the end appends — pasting with the cursor at the end.
    REQUIRE(audioedits::insertAt(counting(3), { 9 }, 999) == std::vector<float> { 0, 1, 2, 9 });
}

TEST_CASE("Cut then paste back reconstructs the original", "[engine][audioedits]")
{
    // The round trip a user actually performs, and the one that catches a
    // half-open/closed mismatch between the two operations.
    const auto original = counting(20);

    const auto lifted    = audioedits::extractRange(original, 5, 12);
    const auto remaining = audioedits::removeRange(original, 5, 12);
    const auto restored  = audioedits::insertAt(remaining, lifted, 5);

    REQUIRE(restored == original);
}

// --- silence / reverse -----------------------------------------------------

TEST_CASE("Silencing zeroes only the range and keeps the length", "[engine][audioedits]")
{
    // Distinct from delete: everything after must stay where it was.
    const auto out = audioedits::silenceRange(counting(6), 2, 4);
    REQUIRE(out == std::vector<float> { 0, 1, 0, 0, 4, 5 });
}

TEST_CASE("Reversing a range reverses only that range", "[engine][audioedits]")
{
    const auto out = audioedits::reverseRange(counting(6), 1, 5);
    REQUIRE(out == std::vector<float> { 0, 4, 3, 2, 1, 5 });
}

TEST_CASE("Reversing twice is the identity", "[engine][audioedits]")
{
    const auto original = counting(17);
    const auto once     = audioedits::reverseRange(original, 3, 14);
    REQUIRE(audioedits::reverseRange(once, 3, 14) == original);
}

// --- fades -----------------------------------------------------------------

TEST_CASE("A fade in starts at silence and ends near unity", "[engine][audioedits]")
{
    const std::vector<float> ones(10, 1.0f);
    const auto               out = audioedits::fadeIn(ones, 0, 10);

    REQUIRE(out.front() == 0.0f);
    REQUIRE(out.back() > 0.85f);
    REQUIRE(out.back() <= 1.0f);

    // Monotonic — a fade that dipped would be audible as a wobble.
    for (size_t i = 1; i < out.size(); ++i)
        REQUIRE(out[i] >= out[i - 1]);
}

TEST_CASE("A fade out ends in exact silence", "[engine][audioedits]")
{
    // A fade to the end of a file that stopped just short of zero would
    // leave a click at the end, which is the whole point of fading.
    const std::vector<float> ones(10, 1.0f);
    const auto               out = audioedits::fadeOut(ones, 0, 10);

    REQUIRE(out.front() > 0.85f);
    REQUIRE(out.back() == 0.0f);

    for (size_t i = 1; i < out.size(); ++i)
        REQUIRE(out[i] <= out[i - 1]);
}

TEST_CASE("A fade leaves everything outside its range alone", "[engine][audioedits]")
{
    const std::vector<float> ones(10, 1.0f);
    const auto               out = audioedits::fadeIn(ones, 4, 8);

    for (int i = 0; i < 4; ++i)
        REQUIRE(out[(size_t) i] == 1.0f);
    for (int i = 8; i < 10; ++i)
        REQUIRE(out[(size_t) i] == 1.0f);
}

// --- zero crossing ---------------------------------------------------------

TEST_CASE("A zero crossing is found where the sign changes", "[engine][audioedits]")
{
    // Cutting mid-waveform leaves a step, heard as a click. The boundary
    // moves a few samples to where the signal is already at zero.
    std::vector<float> wave { 1.0f, 0.8f, 0.4f, -0.2f, -0.7f, -0.3f, 0.5f, 1.0f };

    // Asked near index 2, the crossing between 2 (+0.4) and 3 (-0.2) is at 3.
    REQUIRE(audioedits::nearestZeroCrossing(wave, 2) == 3);
    // Asked near 6, the crossing between 5 (-0.3) and 6 (+0.5) is at 6.
    REQUIRE(audioedits::nearestZeroCrossing(wave, 6) == 6);
}

TEST_CASE("Zero-crossing search gives up gracefully", "[engine][audioedits]")
{
    // A signal that never crosses (all positive) must return the requested
    // index rather than something arbitrary, so the caller can always use
    // the result unconditionally.
    const std::vector<float> allPositive(100, 0.5f);
    REQUIRE(audioedits::nearestZeroCrossing(allPositive, 40) == 40);

    // And a search radius too small to reach one.
    std::vector<float> wave(100, 0.5f);
    wave[90] = -0.5f;
    REQUIRE(audioedits::nearestZeroCrossing(wave, 10, 4) == 10);

    REQUIRE(audioedits::nearestZeroCrossing({}, 5) == 0);
}

// --- resample --------------------------------------------------------------

TEST_CASE("Resampling scales the length by the ratio", "[engine][audioedits]")
{
    // Pasting 44.1k audio into a 48k file: ratio < 1 makes it longer.
    const auto input = counting(1000);

    REQUIRE(audioedits::resample(input, 1.0).size() == 1000);
    REQUIRE(audioedits::resample(input, 2.0).size() == 500);

    const auto stretched = audioedits::resample(input, 44100.0 / 48000.0);
    REQUIRE(stretched.size() > 1080);
    REQUIRE(stretched.size() < 1090);
}

TEST_CASE("Resampling at unity preserves the samples", "[engine][audioedits]")
{
    const auto input = counting(64);
    const auto out   = audioedits::resample(input, 1.0);

    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE(std::abs(out[i] - input[i]) < 1.0e-4f);
}

TEST_CASE("Resampling nonsense is refused rather than crashing", "[engine][audioedits]")
{
    REQUIRE(audioedits::resample({}, 2.0).empty());
    REQUIRE(audioedits::resample(counting(10), 0.0).size() == 10);
    REQUIRE(audioedits::resample(counting(10), -1.0).size() == 10);
}
