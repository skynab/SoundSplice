#include <catch2/catch_test_macros.hpp>

#include <engine/PianoNote.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    std::vector<float> render(PianoNote& note, double seconds)
    {
        const int n = (int) (seconds * kSampleRate);
        std::vector<float> out((size_t) n);
        for (int i = 0; i < n; ++i)
            out[(size_t) i] = note.process();
        return out;
    }

    /** Peak level in a window, in dB. */
    double peakDbAt(const std::vector<float>& signal, double atSeconds, double windowSeconds)
    {
        const auto from  = (size_t) (atSeconds * kSampleRate);
        const auto count = (size_t) (windowSeconds * kSampleRate);

        double peak = 0.0;
        for (size_t i = from; i < from + count && i < signal.size(); ++i)
            peak = std::max(peak, (double) std::abs(signal[i]));

        return 20.0 * std::log10(std::max(peak, 1.0e-9));
    }

    /** Decay rate in dB per second between two times. */
    double decayRate(const std::vector<float>& signal, double fromSeconds, double toSeconds)
    {
        const double a = peakDbAt(signal, fromSeconds, 0.1);
        const double b = peakDbAt(signal, toSeconds,   0.1);
        return (a - b) / (toSeconds - fromSeconds);
    }

    PianoNote makeNote(int strings, float coupling = 1.0f, double frequency = 220.0)
    {
        PianoNote note;
        note.prepare(kSampleRate);
        note.setStringCount(strings);
        note.setFrequency(frequency);
        note.setDecaySeconds(12.0);
        note.setBrightness(0.85f);
        note.setStrikePosition(0.125f);
        note.setCoupling(coupling);
        return note;
    }
}

TEST_CASE("A trichord decays fast and then slowly", "[engine][piano]")
{
    // *The* piano property, and the one this whole class exists for: a fast
    // initial decay and a long quiet aftersound. In phase the strings drive
    // the bridge hard and lose energy quickly; the detuning pulls them out of
    // phase, their forces cancel at the bridge, and what is left rings on.
    //
    // Both parts contribute — see "The bridge deepens the double decay" for
    // the measurement that separates them. This asserts the shape exists.
    auto note = makeNote(3);
    note.strike(0.9f);

    const auto signal = render(note, 6.0);

    const double early = decayRate(signal, 0.1, 1.0);
    const double late  = decayRate(signal, 3.0, 5.0);

    INFO("early " << early << " dB/s, late " << late << " dB/s");
    REQUIRE(early > 0.0);
    REQUIRE(late > 0.0);
    REQUIRE(early > late * 1.5);
}

TEST_CASE("A single string decays at one rate", "[engine][piano]")
{
    // The control. One string has nothing to be out of phase *with*, so there
    // is no second mode and no aftersound — if this also showed a double decay
    // the test above would be measuring something else entirely.
    auto note = makeNote(1);
    note.strike(0.9f);

    const auto signal = render(note, 6.0);

    const double early = decayRate(signal, 0.1, 1.0);
    const double late  = decayRate(signal, 3.0, 5.0);

    INFO("early " << early << " dB/s, late " << late << " dB/s");
    REQUIRE(std::abs(early - late) < early * 0.5);
}

TEST_CASE("The bridge deepens the double decay", "[engine][piano]")
{
    // The first version of this asserted that an *uncoupled* trichord shows no
    // double decay at all, and that was wrong: three detuned strings beat
    // against each other, so their summed peak already falls faster at first
    // than any one of them does. Detuning alone buys part of the effect.
    //
    // What the bridge adds is the mechanism on top of that — in phase the
    // strings drive it hard and genuinely lose energy, rather than merely
    // cancelling in the sum. So the honest measurement is comparative: coupled
    // must fall away faster, relative to its own aftersound, than uncoupled.
    auto ratioFor = [](float coupling)
    {
        auto note = makeNote(3, coupling);
        note.strike(0.9f);
        const auto signal = render(note, 6.0);

        const double early = decayRate(signal, 0.1, 1.0);
        const double late  = decayRate(signal, 3.0, 5.0);
        return late > 0.0 ? early / late : 0.0;
    };

    const double uncoupled = ratioFor(0.0f);
    const double coupled   = ratioFor(1.0f);

    INFO("uncoupled ratio " << uncoupled << ", coupled ratio " << coupled);
    REQUIRE(uncoupled > 0.0);
    REQUIRE(coupled > uncoupled * 1.15);
}

TEST_CASE("A unison beats", "[engine][piano]")
{
    // Detuned strings drift in and out of phase, and the envelope wavers
    // rather than falling smoothly. A perfectly tuned unison would not — and
    // would have no way to reach the out-of-phase mode at all.
    auto note = makeNote(3);
    note.setDetuneCents(3.0f);
    note.strike(0.9f);

    const auto signal = render(note, 4.0);

    // Sample the envelope across the sustain and look for it going back up,
    // which a single decaying exponential never does.
    double previous = peakDbAt(signal, 0.5, 0.05);
    bool   rose     = false;

    for (double at = 0.55; at < 3.0; at += 0.05)
    {
        const double now = peakDbAt(signal, at, 0.05);
        if (now > previous + 0.2)
            rose = true;
        previous = now;
    }

    REQUIRE(rose);
}

TEST_CASE("A damper stops the note", "[engine][piano]")
{
    // Unlike a guitar string, this is the *normal* end of a piano note.
    auto note = makeNote(3);
    note.strike(0.9f);

    render(note, 0.5);
    REQUIRE(note.isRinging());

    note.damp(1.0f);
    const auto after = render(note, 0.4);

    const double level = peakDbAt(after, 0.25, 0.1);
    INFO("level after damping: " << level << " dB");
    REQUIRE(level < -60.0);
}

TEST_CASE("An undamped note keeps ringing", "[engine][piano]")
{
    // The control for the damper, and what the sustain pedal will rely on.
    auto note = makeNote(3);
    note.strike(0.9f);

    const auto signal = render(note, 1.0);
    REQUIRE(peakDbAt(signal, 0.85, 0.1) > -60.0);
    REQUIRE(note.isRinging());
}

TEST_CASE("Adding strings does not shift the pitch", "[engine][piano]")
{
    // The detuning is spread symmetrically about the nominal pitch, so a
    // trichord is centred on the note rather than sharp of it.
    for (int strings : { 1, 2, 3 })
    {
        auto note = makeNote(strings);
        note.strike(0.9f);
        const auto signal = render(note, 1.0);

        // Correlate against the nominal pitch; a shifted unison would beat
        // against this and lose magnitude.
        double real = 0.0, imaginary = 0.0;
        const int from = (int) (0.05 * kSampleRate), count = (int) (0.5 * kSampleRate);
        for (int i = 0; i < count; ++i)
        {
            const double angle = 2.0 * M_PI * 220.0 * (double) i / kSampleRate;
            real      += signal[(size_t) (from + i)] * std::cos(angle);
            imaginary += signal[(size_t) (from + i)] * std::sin(angle);
        }

        INFO(strings << " strings");
        REQUIRE(std::hypot(real, imaginary) / (double) count > 1.0e-4);
    }
}

TEST_CASE("A coupled unison cannot grow", "[engine][piano]")
{
    // Three strings feeding back through a shared bridge is a loop, and the
    // existing "no string may grow" guarantee has to survive it.
    auto note = makeNote(3);
    note.setDecaySeconds(30.0); // the least damped setting available
    note.setBrightness(1.0f);
    note.setCoupling(1.0f);
    note.strike(1.0f);

    const auto signal = render(note, 20.0);

    const double early = peakDbAt(signal, 0.1, 0.5);
    const double late  = peakDbAt(signal, 19.0, 0.5);

    REQUIRE(std::isfinite(late));
    REQUIRE(late < early);
}

TEST_CASE("The bass is single-strung and the treble triple", "[engine][piano]")
{
    REQUIRE(pianoStringCount(21) == 1);  // A0
    REQUIRE(pianoStringCount(28) == 1);
    REQUIRE(pianoStringCount(36) == 2);  // C2
    REQUIRE(pianoStringCount(60) == 3);  // middle C
    REQUIRE(pianoStringCount(108) == 3); // C8
}

TEST_CASE("Inharmonicity is U-shaped across the keyboard", "[engine][piano]")
{
    // High in the bass, least through the middle, climbing again at the top —
    // the measured shape on a real instrument, and not something a single
    // constant or a simple slope reproduces.
    const float bass   = pianoStiffness(21);
    const float middle = pianoStiffness(60);
    const float treble = pianoStiffness(105);

    INFO("bass " << bass << " middle " << middle << " treble " << treble);
    REQUIRE(middle < bass);
    REQUIRE(middle < treble);
    REQUIRE(middle >= 0.0f);
    REQUIRE(bass <= 1.0f);
    REQUIRE(treble <= 1.0f);
}
