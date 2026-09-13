#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <engine/TempoDetect.h>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 44100.0;

    /** A short percussive click: a fast-decaying burst of noise-ish tone.
        Broadband enough to move a lot of FFT bins at once, which is what
        spectral flux responds to. */
    void addClick(std::vector<float>& samples, size_t at, float amplitude = 1.0f)
    {
        const int length = (int) (kSampleRate * 0.02); // 20ms

        for (int i = 0; i < length; ++i)
        {
            const size_t index = at + (size_t) i;
            if (index >= samples.size())
                return;

            const double decay = std::exp(-40.0 * i / kSampleRate);
            // A couple of inharmonic partials rather than one sine: a single
            // frequency only lights up one bin, and a real drum hit doesn't.
            const double tone = std::sin(2.0 * 3.14159265358979 * 180.0 * i / kSampleRate)
                              + 0.7 * std::sin(2.0 * 3.14159265358979 * 1330.0 * i / kSampleRate)
                              + 0.5 * std::sin(2.0 * 3.14159265358979 * 3070.0 * i / kSampleRate);

            samples[index] += (float) (amplitude * decay * tone * 0.3);
        }
    }

    /** @p seconds of silence with a click every beat at @p bpm. */
    std::vector<float> clickTrain(double bpm, double seconds)
    {
        std::vector<float> samples((size_t) (kSampleRate * seconds), 0.0f);

        const double samplesPerBeat = kSampleRate * 60.0 / bpm;
        for (double at = 0.0; at < (double) samples.size(); at += samplesPerBeat)
            addClick(samples, (size_t) at);

        return samples;
    }

    bool nearBpm(double actual, double expected, double tolerance = 2.0)
    {
        return std::abs(actual - expected) <= tolerance;
    }
}

TEST_CASE("A click train is detected at its own tempo", "[engine][tempodetect]")
{
    // The whole reason this module is JUCE-free and headless: an exact
    // expected answer, rather than something only checkable by ear.
    for (double bpm : { 90.0, 100.0, 120.0, 128.0, 140.0 })
    {
        const auto estimate = detectTempo(clickTrain(bpm, 8.0), kSampleRate);

        INFO("expected " << bpm << ", got " << estimate.bpm);
        REQUIRE(estimate.isUsable());
        REQUIRE(nearBpm(estimate.bpm, bpm));
    }
}

TEST_CASE("A fast loop is not reported at half speed", "[engine][tempodetect]")
{
    // The classic failure: onsets that repeat every beat also repeat every two
    // beats, so the autocorrelation peak is ambiguous by factors of two. A 174
    // BPM break reported as 87 warps to half speed — not a small error.
    const auto estimate = detectTempo(clickTrain(174.0, 8.0), kSampleRate);

    REQUIRE(estimate.isUsable());
    INFO("got " << estimate.bpm);
    REQUIRE(nearBpm(estimate.bpm, 174.0, 3.0));
}

TEST_CASE("A slow loop is not reported at double speed", "[engine][tempodetect]")
{
    const auto estimate = detectTempo(clickTrain(76.0, 10.0), kSampleRate);

    REQUIRE(estimate.isUsable());
    INFO("got " << estimate.bpm);
    REQUIRE(nearBpm(estimate.bpm, 76.0, 3.0));
}

TEST_CASE("Every detected tempo lands in the range tempos are named in", "[engine][tempodetect]")
{
    // Octave resolution's actual contract: whatever the ambiguity, the answer
    // is reported the way a person would say it.
    for (double bpm : { 76.0, 90.0, 120.0, 140.0, 174.0 })
    {
        const auto estimate = detectTempo(clickTrain(bpm, 8.0), kSampleRate);
        REQUIRE(estimate.isUsable());
        REQUIRE(estimate.bpm >= tempodetect::kPreferredMinBpm - 1.0);
        REQUIRE(estimate.bpm <= tempodetect::kPreferredMaxBpm + 1.0);
    }
}

TEST_CASE("A backbeat pattern is detected at the beat, not the bar", "[engine][tempodetect]")
{
    // Kick on 1 and 3, snare on 2 and 4 — onsets on every beat, but the
    // *pattern* repeats every two beats. Following the pattern instead of the
    // beat would report half the real tempo.
    const double bpm            = 120.0;
    const double samplesPerBeat = kSampleRate * 60.0 / bpm;

    std::vector<float> samples((size_t) (kSampleRate * 8.0), 0.0f);
    int beat = 0;
    for (double at = 0.0; at < (double) samples.size(); at += samplesPerBeat, ++beat)
        addClick(samples, (size_t) at, beat % 2 == 0 ? 1.0f : 0.75f);

    const auto estimate = detectTempo(samples, kSampleRate);

    REQUIRE(estimate.isUsable());
    INFO("got " << estimate.bpm);
    REQUIRE(nearBpm(estimate.bpm, bpm, 3.0));
}

TEST_CASE("Silence yields no estimate", "[engine][tempodetect]")
{
    const std::vector<float> silence((size_t) (kSampleRate * 4.0), 0.0f);
    REQUIRE_FALSE(detectTempo(silence, kSampleRate).isUsable());
}

TEST_CASE("A clip too short to hold a beat yields no estimate", "[engine][tempodetect]")
{
    // Better to say "I don't know" than to report a tempo derived from a
    // fragment that never contained one.
    REQUIRE_FALSE(detectTempo(clickTrain(120.0, 0.1), kSampleRate).isUsable());
    // Spelled out rather than `{}`: with both a mono and a multi-channel
    // overload, an empty braced list is genuinely ambiguous.
    REQUIRE_FALSE(detectTempo(std::vector<float>{}, kSampleRate).isUsable());
}

TEST_CASE("An invalid sample rate yields no estimate", "[engine][tempodetect]")
{
    REQUIRE_FALSE(detectTempo(clickTrain(120.0, 4.0), 0.0).isUsable());
}

TEST_CASE("A rhythmic loop is more confident than a sustained tone", "[engine][tempodetect]")
{
    // Confidence exists so a weak result can be offered rather than silently
    // applied — a detector with no way to tell a confident 128 from a
    // coin-flip 91 will eventually warp a pad to a beat it invented.
    const auto rhythmic = detectTempo(clickTrain(120.0, 8.0), kSampleRate);

    std::vector<float> pad((size_t) (kSampleRate * 8.0), 0.0f);
    for (size_t i = 0; i < pad.size(); ++i)
        pad[i] = (float) (0.3 * std::sin(2.0 * 3.14159265358979 * 220.0 * (double) i / kSampleRate));

    const auto sustained = detectTempo(pad, kSampleRate);

    INFO("rhythmic " << rhythmic.confidence << " vs sustained " << sustained.confidence);
    REQUIRE(rhythmic.confidence > sustained.confidence);
}

TEST_CASE("The onset envelope spikes where the clicks are", "[engine][tempodetect]")
{
    // The stage worth inspecting on its own when a detection looks wrong.
    const double bpm      = 120.0; // a click every 0.5s
    const auto   envelope = tempodetect::onsetEnvelope(clickTrain(bpm, 4.0), kSampleRate);

    REQUIRE_FALSE(envelope.empty());

    const double envelopeRate = kSampleRate / (double) tempodetect::kHop;

    // The envelope value at the second click should stand well above the
    // value midway between clicks, where nothing happens.
    const size_t atClick   = (size_t) (envelopeRate * 0.5);
    const size_t atSilence = (size_t) (envelopeRate * 0.75);

    REQUIRE(atSilence < envelope.size());

    float peakNearClick = 0.0f;
    for (size_t i = atClick - 2; i <= atClick + 2 && i < envelope.size(); ++i)
        peakNearClick = std::max(peakNearClick, envelope[i]);

    REQUIRE(peakNearClick > envelope[atSilence] * 4.0f);
}

TEST_CASE("Stereo input is analysed as its mono average", "[engine][tempodetect]")
{
    const auto mono = clickTrain(128.0, 8.0);

    const std::vector<std::vector<float>> stereo { mono, mono };
    const auto estimate = detectTempo(stereo, kSampleRate);

    REQUIRE(estimate.isUsable());
    REQUIRE(nearBpm(estimate.bpm, 128.0));
}

TEST_CASE("An empty channel list yields no estimate", "[engine][tempodetect]")
{
    REQUIRE_FALSE(detectTempo(std::vector<std::vector<float>>{}, kSampleRate).isUsable());
}

TEST_CASE("A faster loop is stretched longer to fit a slower project", "[engine][tempodetect]")
{
    // The inversion is the easiest mistake here and the hardest to notice —
    // the result is still "in time", just at the wrong tempo. A 174 BPM break
    // in a 120 BPM song has to last longer, not shorter.
    const double factor = warpStretchFactor(174.0, 120.0, true);

    REQUIRE(factor > 1.0);
    REQUIRE(std::abs(factor - 174.0 / 120.0) < 1.0e-9);
}

TEST_CASE("A slower loop is compressed to fit a faster project", "[engine][tempodetect]")
{
    const double factor = warpStretchFactor(90.0, 120.0, true);
    REQUIRE(factor < 1.0);
    REQUIRE(std::abs(factor - 0.75) < 1.0e-9);
}

TEST_CASE("A loop already at the project tempo is not stretched", "[engine][tempodetect]")
{
    // Matters beyond tidiness: a factor of exactly 1 is what lets the engine
    // skip the vocoder entirely, and the vocoder is not transparent.
    REQUIRE(warpStretchFactor(120.0, 120.0, true) == 1.0);
}

TEST_CASE("Warping off, or an unknown tempo, means no stretch", "[engine][tempodetect]")
{
    REQUIRE(warpStretchFactor(174.0, 120.0, false) == 1.0); // switched off
    REQUIRE(warpStretchFactor(0.0,   120.0, true)  == 1.0); // never detected
    REQUIRE(warpStretchFactor(-5.0,  120.0, true)  == 1.0); // nonsense
    REQUIRE(warpStretchFactor(174.0, 0.0,   true)  == 1.0); // no project tempo
}

TEST_CASE("An absurd tempo ratio is clamped rather than obeyed", "[engine][tempodetect]")
{
    // A mis-detection or a hand-typed 12 should degrade into something
    // audible, not into a vocoder run whose output length explodes.
    REQUIRE(warpStretchFactor(12.0,  200.0, true) == 0.25);
    REQUIRE(warpStretchFactor(400.0, 60.0,  true) == 4.0);
}
