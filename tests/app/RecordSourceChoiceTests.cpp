#include <catch2/catch_test_macros.hpp>

#include <app/RecordSourceChoice.h>

using namespace looper::app;

namespace
{
    constexpr bool kMidiTrack  = true;
    constexpr bool kAudioTrack = false;
    constexpr bool kConnected  = true;
    constexpr bool kAbsent     = false;
}

// The whole table, one row at a time. This decision shipped wrong once — with
// only the armed track's type as input — so every combination is pinned rather
// than only the interesting-looking ones.

TEST_CASE("A MIDI track with a controller records MIDI", "[app][recordsource]")
{
    // Both connected: the controller wins, because a synth track cannot hold
    // an audio clip regardless.
    REQUIRE(chooseRecordSource(kMidiTrack, kConnected, kConnected, false)
            == RecordSourceDecision { RecordSource::Midi, RecordSourceReason::Ok });

    REQUIRE(chooseRecordSource(kMidiTrack, kConnected, kAbsent, false)
            == RecordSourceDecision { RecordSource::Midi, RecordSourceReason::Ok });
}

TEST_CASE("A MIDI track with no controller falls back to an audio take", "[app][recordsource]")
{
    // The regression this module exists for: choosing MIDI here produces an
    // empty take on a machine that has only a microphone, and a Record button
    // that appears to do nothing.
    const auto decision = chooseRecordSource(kMidiTrack, kAbsent, kConnected, false);

    REQUIRE(decision.source == RecordSource::Audio);
    // Flagged, not silent: the take lands on a new track rather than the
    // armed one, and the user has to be told why.
    REQUIRE(decision.reason == RecordSourceReason::FallbackToAudioNoMidi);
}

TEST_CASE("A MIDI track with nothing connected records nothing", "[app][recordsource]")
{
    REQUIRE(chooseRecordSource(kMidiTrack, kAbsent, kAbsent, false)
            == RecordSourceDecision { RecordSource::None, RecordSourceReason::NothingConnected });
}

TEST_CASE("An audio track records audio when there is an input", "[app][recordsource]")
{
    REQUIRE(chooseRecordSource(kAudioTrack, kAbsent, kConnected, false)
            == RecordSourceDecision { RecordSource::Audio, RecordSourceReason::Ok });
}

TEST_CASE("An audio track ignores a connected controller", "[app][recordsource]")
{
    // An Audio track has nowhere to put a pattern, so a controller being
    // present must not change the answer.
    REQUIRE(chooseRecordSource(kAudioTrack, kConnected, kConnected, false)
            == RecordSourceDecision { RecordSource::Audio, RecordSourceReason::Ok });

    REQUIRE(chooseRecordSource(kAudioTrack, kConnected, kAbsent, false).source
            == RecordSource::None);
}

TEST_CASE("No audio input names the permission when opening failed", "[app][recordsource]")
{
    // On macOS a denied microphone permission is overwhelmingly the cause, and
    // it is worth naming rather than making the user guess between three.
    REQUIRE(chooseRecordSource(kAudioTrack, kAbsent, kAbsent, true).reason
            == RecordSourceReason::NoAudioInputPermission);

    REQUIRE(chooseRecordSource(kAudioTrack, kAbsent, kAbsent, false).reason
            == RecordSourceReason::NoAudioInput);
}

TEST_CASE("Nothing is ever recorded from a source that isn't there", "[app][recordsource]")
{
    // The invariant underneath the whole table: a decision to record audio
    // requires an audio input, and a decision to record MIDI requires a
    // controller. Exhaustive over all four inputs.
    for (int bits = 0; bits < 16; ++bits)
    {
        const bool trackHoldsMidi  = (bits & 1) != 0;
        const bool haveMidi        = (bits & 2) != 0;
        const bool haveAudio       = (bits & 4) != 0;
        const bool audioOpenFailed = (bits & 8) != 0;

        const auto decision = chooseRecordSource(trackHoldsMidi, haveMidi, haveAudio, audioOpenFailed);

        if (decision.source == RecordSource::Audio)
            REQUIRE(haveAudio);
        if (decision.source == RecordSource::Midi)
            REQUIRE(haveMidi);
    }
}
