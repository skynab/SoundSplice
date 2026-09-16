#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <app/ClipWindow.h>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

namespace
{
    // At 120bpm a beat is half a second, which keeps every expected value
    // exact enough to read at a glance.
    constexpr double kBpm = 120.0;
    constexpr double kRate = 48000.0;

    model::Clip audioClip(double startBeats, double lengthBeats, double offsetSeconds = 0.0)
    {
        model::Clip clip;
        clip.type                = model::ClipType::Audio;
        clip.audioFile           = "take.wav";
        clip.startBeats          = startBeats;
        clip.lengthBeats         = lengthBeats;
        clip.sourceOffsetSeconds = offsetSeconds;
        return clip;
    }
}

TEST_CASE("A clip's window starts at its offset and runs for its length", "[app][clipwindow]")
{
    // Four beats is two seconds, starting one second into the file.
    const auto window = clipSampleWindow(audioClip(0.0, 4.0, 1.0), (int) (10 * kRate), kRate, kBpm);

    REQUIRE(window.start == 48000);
    REQUIRE(window.end == 144000);
}

TEST_CASE("A clip's window never runs past the end of its file", "[app][clipwindow]")
{
    const auto clip = audioClip(0.0, 40.0, 1.0); // twenty seconds asked for

    const auto window = clipSampleWindow(clip, (int) (2 * kRate), kRate, kBpm);
    REQUIRE(window.start == 48000);
    REQUIRE(window.end == 96000);
    REQUIRE_THAT(clipAudibleSeconds(clip, 2.0, kBpm), WithinAbs(1.0, 1e-9));

    // An offset past the end plays nothing at all.
    REQUIRE(clipSampleWindow(audioClip(0.0, 4.0, 5.0), (int) (2 * kRate), kRate, kBpm).isEmpty());
    REQUIRE(clipAudibleSeconds(audioClip(0.0, 4.0, 5.0), 2.0, kBpm) == 0.0);
}

TEST_CASE("Splicing replaces only the window, at any length", "[app][clipwindow]")
{
    const std::vector<float> file { 1, 2, 3, 4, 5, 6 };
    const SampleWindow       window { 2, 4 };

    REQUIRE(windowSamples(file, window) == std::vector<float> { 3, 4 });
    REQUIRE(spliceWindow(file, window, { 9 }) == std::vector<float> { 1, 2, 9, 5, 6 });
    REQUIRE(spliceWindow(file, window, { 7, 8, 9 }) == std::vector<float> { 1, 2, 7, 8, 9, 5, 6 });
}

TEST_CASE("Splitting leaves two clips that play the same audio back to back", "[app][clipwindow]")
{
    const auto [first, second] = splitClipAt(audioClip(2.0, 8.0, 0.5), 1.0, kBpm);

    REQUIRE_THAT(first.startBeats, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(first.lengthBeats, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(first.sourceOffsetSeconds, WithinAbs(0.5, 1e-9));

    REQUIRE_THAT(second.startBeats, WithinAbs(4.0, 1e-9));
    REQUIRE_THAT(second.lengthBeats, WithinAbs(6.0, 1e-9));
    REQUIRE_THAT(second.sourceOffsetSeconds, WithinAbs(1.5, 1e-9));

    // No gap and no overlap: the second picks up exactly where the first stops.
    REQUIRE(first.startBeats + first.lengthBeats == second.startBeats);
    REQUIRE(second.audioFile == first.audioFile);
}

TEST_CASE("Splitting keeps the outer fades and adds none at the cut", "[app][clipwindow]")
{
    auto clip = audioClip(0.0, 8.0);
    clip.fades.inSeconds  = 0.5;
    clip.fades.outSeconds = 1.0;

    const auto [first, second] = splitClipAt(clip, 2.0, kBpm);

    REQUIRE(first.fades.inSeconds == 0.5);
    REQUIRE(first.fades.outSeconds == 0.0);
    REQUIRE(second.fades.inSeconds == 0.0);
    REQUIRE(second.fades.outSeconds == 1.0);
}

TEST_CASE("Trimming to a range keeps the kept audio where it was", "[app][clipwindow]")
{
    const auto trimmed = trimClipToRange(audioClip(4.0, 8.0), 1.0, 2.5, kBpm);

    REQUIRE_THAT(trimmed.startBeats, WithinAbs(6.0, 1e-9));
    REQUIRE_THAT(trimmed.lengthBeats, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(trimmed.sourceOffsetSeconds, WithinAbs(1.0, 1e-9));
}

TEST_CASE("Dragging a clip's start moves the edge but not the audio", "[app][clipwindow]")
{
    const auto clip = audioClip(4.0, 8.0, 1.0);

    const auto shorter = trimClipStart(clip, 5.0, kBpm, 1.0);
    REQUIRE_THAT(shorter.startBeats, WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(shorter.lengthBeats, WithinAbs(7.0, 1e-9));
    REQUIRE_THAT(shorter.sourceOffsetSeconds, WithinAbs(1.5, 1e-9));

    // Back out again, revealing audio the offset had been hiding.
    const auto longer = trimClipStart(clip, 3.0, kBpm, 1.0);
    REQUIRE_THAT(longer.startBeats, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(longer.lengthBeats, WithinAbs(9.0, 1e-9));
    REQUIRE_THAT(longer.sourceOffsetSeconds, WithinAbs(0.5, 1e-9));

    // Either way the clip still ends where it did.
    REQUIRE_THAT(shorter.startBeats + shorter.lengthBeats, WithinAbs(12.0, 1e-9));
    REQUIRE_THAT(longer.startBeats + longer.lengthBeats, WithinAbs(12.0, 1e-9));
}

TEST_CASE("A clip's start can't be dragged before its file begins", "[app][clipwindow]")
{
    // One second of offset at 120bpm puts the file's first sample at beat 2.
    const auto trimmed = trimClipStart(audioClip(4.0, 8.0, 1.0), 0.0, kBpm, 1.0);

    REQUIRE_THAT(trimmed.startBeats, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(trimmed.lengthBeats, WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(trimmed.sourceOffsetSeconds, WithinAbs(0.0, 1e-9));
}

TEST_CASE("A clip's start can't be dragged past its minimum length", "[app][clipwindow]")
{
    const auto trimmed = trimClipStart(audioClip(4.0, 8.0), 20.0, kBpm, 1.0);

    REQUIRE_THAT(trimmed.startBeats, WithinAbs(11.0, 1e-9));
    REQUIRE_THAT(trimmed.lengthBeats, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(trimmed.sourceOffsetSeconds, WithinAbs(3.5, 1e-9));
}

TEST_CASE("A clip's start can't be dragged before beat zero", "[app][clipwindow]")
{
    const auto trimmed = trimClipStart(audioClip(1.0, 4.0, 5.0), -3.0, kBpm, 1.0);

    REQUIRE_THAT(trimmed.startBeats, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(trimmed.lengthBeats, WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(trimmed.sourceOffsetSeconds, WithinAbs(4.5, 1e-9));
}

TEST_CASE("Slipping a clip moves its audio but not its edges", "[app][clipwindow]")
{
    // Four beats (two seconds) starting three seconds into a ten-second file.
    const auto clip = audioClip(4.0, 4.0, 3.0);

    const auto later = slipClip(clip, 1.0, 10.0, kBpm);
    REQUIRE_THAT(later.sourceOffsetSeconds, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(later.startBeats, WithinAbs(4.0, 1e-9));
    REQUIRE_THAT(later.lengthBeats, WithinAbs(4.0, 1e-9));

    const auto earlier = slipClip(clip, -2.5, 10.0, kBpm);
    REQUIRE_THAT(earlier.sourceOffsetSeconds, WithinAbs(5.5, 1e-9));
}

TEST_CASE("A clip can't slip past either end of its file", "[app][clipwindow]")
{
    const auto clip = audioClip(4.0, 4.0, 3.0);

    REQUIRE_THAT(slipClip(clip, 10.0, 10.0, kBpm).sourceOffsetSeconds, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(slipClip(clip, -10.0, 10.0, kBpm).sourceOffsetSeconds, WithinAbs(8.0, 1e-9));

    // Longer than what's left of its file: back towards the audio only.
    const auto overhanging = audioClip(0.0, 8.0, 7.0); // four seconds from seven of ten
    REQUIRE_THAT(slipClip(overhanging, -1.0, 10.0, kBpm).sourceOffsetSeconds, WithinAbs(7.0, 1e-9));
    REQUIRE_THAT(slipClip(overhanging, 2.0, 10.0, kBpm).sourceOffsetSeconds, WithinAbs(5.0, 1e-9));

    // A file still being scanned: only the start is known.
    REQUIRE_THAT(slipClip(clip, -4.0, 0.0, kBpm).sourceOffsetSeconds, WithinAbs(7.0, 1e-9));
    REQUIRE_THAT(slipClip(clip, 4.0, 0.0, kBpm).sourceOffsetSeconds, WithinAbs(0.0, 1e-9));
}
