#include <catch2/catch_test_macros.hpp>

#include <app/ClipTimeMapping.h>

using namespace soundsplice;

namespace
{
    model::Clip audioClip(double start, double length, double offset)
    {
        model::Clip clip;
        clip.type                = model::ClipType::Audio;
        clip.audioFile           = "take.wav";
        clip.startBeats          = start;
        clip.lengthBeats         = length;
        clip.sourceOffsetSeconds = offset;
        return clip;
    }
}

TEST_CASE("A beat maps to the file sample playing there, and back", "[app][cliptime]")
{
    // 120 bpm: half a second a beat. Starts at beat 4, two seconds into its file.
    const auto clip = audioClip(4.0, 8.0, 2.0);

    REQUIRE(app::fileFrameAt(clip, 4.0, 48000.0, 120.0) == 96000);
    REQUIRE(app::fileFrameAt(clip, 5.0, 48000.0, 120.0) == 120000);
    REQUIRE(app::beatForFileFrame(clip, 120000, 48000.0, 120.0) == 5.0);

    for (const double beat : { 4.0, 4.123, 7.5, 12.0 })
    {
        const auto frame = app::fileFrameAt(clip, beat, 44100.0, 120.0);
        // Within the half-sample that rounding to a frame can move it, at two beats a second.
        REQUIRE(std::abs(app::beatForFileFrame(clip, frame, 44100.0, 120.0) - beat) <= 2.0 / 44100.0);
    }
}

TEST_CASE("The clip under a beat is found, edges included, earlier one first", "[app][cliptime]")
{
    model::Track track;
    track.clips.push_back(audioClip(4.0, 4.0, 0.0));  // 4..8
    track.clips.push_back(audioClip(0.0, 4.0, 0.0));  // 0..4, added second

    auto midi        = audioClip(10.0, 4.0, 0.0);
    midi.type        = model::ClipType::Instrument;
    track.clips.push_back(midi);

    REQUIRE(app::audioClipAt(track, 2.0) == &track.clips[1]);
    REQUIRE(app::audioClipAt(track, 6.0) == &track.clips[0]);
    REQUIRE(app::audioClipAt(track, 4.0) == &track.clips[1]); // where two meet, the earlier
    REQUIRE(app::audioClipAt(track, 8.0) == &track.clips[0]); // an end counts
    REQUIRE(app::audioClipAt(track, 9.0) == nullptr);
    REQUIRE(app::audioClipAt(track, 12.0) == nullptr);        // instrument clips have no audio
}
