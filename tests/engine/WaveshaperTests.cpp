#include <catch2/catch_test_macros.hpp>

#include <engine/Waveshaper.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    /** Magnitude at one frequency, by direct correlation. A whole FFT would
        be more than this needs: every claim here is about a single known bin. */
    double magnitudeAt(const std::vector<float>& signal, double hz, double sampleRate)
    {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < signal.size(); ++n)
        {
            const double phase = 2.0 * kPi * hz * (double) n / sampleRate;
            re += signal[n] * std::cos(phase);
            im -= signal[n] * std::sin(phase);
        }
        return std::sqrt(re * re + im * im) / (double) signal.size();
    }

    std::vector<float> drive(Waveshaper& shaper, double toneHz, double sampleRate,
                             int samples, float amplitude, bool antiAliased)
    {
        std::vector<float> out;
        out.reserve((size_t) samples);
        for (int n = 0; n < samples; ++n)
        {
            const float x = amplitude * (float) std::sin(2.0 * kPi * toneHz * n / sampleRate);
            out.push_back(antiAliased ? shaper.processSample(x) : shaper.processSampleNaive(x));
        }
        return out;
    }
}

TEST_CASE("Anti-aliased shaping puts less energy in the aliases", "[engine][drive]")
{
    // Measuring aliasing needs a bin that nothing legitimate occupies. If the
    // sample rate is an exact multiple of the tone, every folded alias lands
    // straight back on a harmonic and the two are indistinguishable — so the
    // tones here are chosen so that fs/tone is not an integer, and the bin is
    // checked against the harmonic grid before it is used.
    constexpr double sampleRate = 48000.0;

    auto cleanAliasBin = [sampleRate](double tone)
    {
        auto onHarmonic = [sampleRate](double hz, double t)
        {
            for (int k = 1; k * t < sampleRate / 2; ++k)
                if (std::abs(k * t - hz) < 1.0)
                    return true;
            return false;
        };

        for (int k = 3; k < 60; k += 2)
        {
            const double harmonic = k * tone;
            if (harmonic <= sampleRate / 2)
                continue;

            double folded = std::fmod(harmonic, sampleRate);
            if (folded > sampleRate / 2)
                folded = sampleRate - folded;

            if (folded > 200.0 && folded < sampleRate / 2 - 200.0 && ! onHarmonic(folded, tone))
                return folded;
        }
        return -1.0;
    };

    for (double tone : { 1700.0, 3300.0, 5000.0 })
    {
        for (float driveAmount : { 4.0f, 12.0f })
        {
            const double aliasBin = cleanAliasBin(tone);
            REQUIRE(aliasBin > 0.0);

            Waveshaper naive, adaa;
            naive.setDrive(driveAmount);
            adaa.setDrive(driveAmount);
            naive.reset();
            adaa.reset();

            const double naiveAlias = magnitudeAt(drive(naive, tone, sampleRate, 16384, 0.9f, false),
                                                  aliasBin, sampleRate);
            const double adaaAlias  = magnitudeAt(drive(adaa, tone, sampleRate, 16384, 0.9f, true),
                                                  aliasBin, sampleRate);

            INFO("tone " << tone << " drive " << driveAmount << " alias bin " << aliasBin
                         << ": naive " << naiveAlias << " adaa " << adaaAlias);

            // Measured across these cases the ratio runs 0.44-0.60, i.e. about
            // 4.5-7dB. The bound is set above the worst of them with margin,
            // so this fails if ADAA is bypassed but doesn't chase the exact
            // figure.
            REQUIRE(adaaAlias < naiveAlias * 0.7);
        }
    }
}

TEST_CASE("Shaping still produces the harmonics it is supposed to", "[engine][drive]")
{
    // Suppressing aliases is worthless if it also suppresses the distortion.
    // A symmetric shaper generates odd harmonics: the third must be well up.
    constexpr double sampleRate = 48000.0;
    constexpr double tone       = 500.0;

    Waveshaper shaper;
    shaper.setDrive(10.0f);
    shaper.reset();

    const auto out = drive(shaper, tone, sampleRate, 8192, 0.9f, true);

    const double fundamental = magnitudeAt(out, tone, sampleRate);
    const double third       = magnitudeAt(out, tone * 3.0, sampleRate);

    REQUIRE(fundamental > 0.1);
    REQUIRE(third > fundamental * 0.05); // audibly present, not a rounding error
}

TEST_CASE("A held signal doesn't turn into noise", "[engine][drive]")
{
    // The degenerate case: consecutive samples nearly equal makes the ADAA
    // quotient 0/0. A sustained note is exactly that, so getting it wrong
    // would make held notes — the thing a guitar does most — break up.
    Waveshaper shaper;
    shaper.setDrive(4.0f);
    shaper.reset();

    for (int n = 0; n < 64; ++n) // settle
        shaper.processSample(0.5f);

    const float expected = std::tanh(0.5f * 4.0f);
    for (int n = 0; n < 256; ++n)
    {
        const float y = shaper.processSample(0.5f);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y - expected) < 1.0e-4f);
    }
}

TEST_CASE("A very slow ramp stays finite and monotonic", "[engine][drive]")
{
    // Every sample of this sits in the fallback region.
    Waveshaper shaper;
    shaper.setDrive(2.0f);
    shaper.reset();

    // ADAA carries the previous input as state, and reset() clears it to
    // zero. The very first sample is therefore averaged over the jump from
    // silence to wherever the signal starts — one sample of startup
    // transient, inherent to the method rather than a defect. Prime it.
    shaper.processSample(-0.5f);

    float previous = -2.0f;
    for (int n = 0; n < 4000; ++n)
    {
        const float y = shaper.processSample(-0.5f + (float) n * 1.0e-6f);
        REQUIRE(std::isfinite(y));
        REQUIRE(y >= previous - 1.0e-5f);
        previous = y;
    }
}

TEST_CASE("Large inputs don't overflow the antiderivative", "[engine][drive]")
{
    // log(cosh(x)) via cosh() overflows to infinity around |x| = 710 and the
    // quotient becomes NaN. A drive pedal is where large inputs turn up.
    Waveshaper shaper;
    shaper.setDrive(1000.0f);
    shaper.reset();

    for (float x : { -50.0f, -1.0f, 0.0f, 1.0f, 50.0f, 900.0f, -900.0f })
    {
        const float y = shaper.processSample(x);
        INFO("input " << x);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) <= 1.001f); // tanh is bounded, and so is its average
    }
}

TEST_CASE("Hard clipping is bounded and flat above the threshold", "[engine][drive]")
{
    Waveshaper shaper;
    shaper.setKind(Waveshaper::Kind::Hard);
    shaper.setDrive(1.0f);
    shaper.reset();

    for (int n = 0; n < 64; ++n)
        shaper.processSample(5.0f);

    REQUIRE(std::abs(shaper.processSample(5.0f) - 1.0f) < 1.0e-5f);
    for (int n = 0; n < 64; ++n)
        shaper.processSample(-5.0f);
    REQUIRE(std::abs(shaper.processSample(-5.0f) + 1.0f) < 1.0e-5f);
}

namespace
{
// Steady-state magnitude of the cabinet at one frequency. Built fresh each
// call so no state carries between measurements.
double cabinetResponseAt(double hz, double sampleRate = 48000.0)
{
    CabinetSim cab;
    cab.prepare(sampleRate);

    std::vector<float> out;
    const int samples = 16384;
    out.reserve((size_t) samples);
    for (int n = 0; n < samples; ++n)
        out.push_back(cab.processSample((float) std::sin(2.0 * kPi * hz * n / sampleRate)));

    // Skip the settling transient before measuring.
    const std::vector<float> steady(out.begin() + 4096, out.end());
    return magnitudeAt(steady, hz, sampleRate);
}
}

TEST_CASE("The cabinet takes the fizz off the top", "[engine][drive]")
{
    // The whole reason it ships with the drive: distortion is full of energy
    // above a speaker's top corner, and that energy is what reads as fizz.
    const double atOneK   = cabinetResponseAt(1000.0);
    const double atTenK   = cabinetResponseAt(10000.0);
    const double atThirty = cabinetResponseAt(30.0);

    INFO("1k " << atOneK << "  10k " << atTenK << "  30Hz " << atThirty);

    // 35dB, not the 12dB the previous three-real-pole cabinet managed. That
    // 12dB was the whole reason fizz survived the cab: quieter is not gone,
    // and a real speaker simply does not radiate up there.
    REQUIRE(atTenK < atOneK * 0.0178);
    REQUIRE(atThirty < atOneK * 0.5); // and so is the mud below the cab
    REQUIRE(atOneK > 0.1);            // while the guitar's own range survives
}

TEST_CASE("The cabinet has a cone-breakup peak and a notch above it", "[engine][drive]")
{
    // The midrange structure that makes a speaker identifiable as one. A pair
    // of corner frequencies cannot express this, which is why the cab used to
    // sound like an EQ curve rather than a cabinet.
    const double atOneK     = cabinetResponseAt(1000.0);
    const double atPresence = cabinetResponseAt(2000.0);
    const double atNotch    = cabinetResponseAt(3500.0);

    INFO("1k " << atOneK << "  2k " << atPresence << "  3.5k " << atNotch);

    // The peak is a genuine boost over the midband, not just a shallower
    // rolloff - this is the band that makes a guitar cut through a mix.
    REQUIRE(atPresence > atOneK * 1.3);

    // And the notch immediately above it is a real dip, not merely the start
    // of the top-end rolloff: it has to be well below the peak *and* below
    // the flat midband on the other side of it.
    REQUIRE(atNotch < atPresence * 0.5);
    REQUIRE(atNotch < atOneK);
}

TEST_CASE("The cabinet resonates at the bottom rather than just rolling off", "[engine][drive]")
{
    // A cab's low end is a cone/box resonance around 100Hz with the cabinet
    // ceasing to move air below it - a bump then a cliff, not a slope. The
    // bump is where a palm-muted chug's weight actually comes from.
    const double atResonance = cabinetResponseAt(105.0);
    const double atMid       = cabinetResponseAt(1000.0);
    const double atSubsonic  = cabinetResponseAt(35.0);

    INFO("105Hz " << atResonance << "  1k " << atMid << "  35Hz " << atSubsonic);

    REQUIRE(atResonance > atMid);            // a bump, not a rolloff
    REQUIRE(atSubsonic < atResonance * 0.5); // and gone underneath it
}

TEST_CASE("The cabinet settles rather than running away", "[engine][drive]")
{
    CabinetSim cab;
    cab.prepare(48000.0);

    for (int n = 0; n < 100000; ++n)
    {
        const float y = cab.processSample(n % 2 == 0 ? 1.0f : -1.0f);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 10.0f);
    }
}

TEST_CASE("Asymmetry produces even harmonics where the symmetric shaper produces none",
          "[engine][drive]")
{
    // A symmetric curve gives odd harmonics only, however hard it is driven -
    // that is a property of the curve, not of the drive setting. Every real
    // tube stage is asymmetric, and the 2nd harmonic that asymmetry generates
    // is most of what "warmth" means.
    constexpr double sampleRate = 48000.0;
    constexpr int    samples    = 16384;

    // Deliberately on an exact bin (75 of 16384), so the fundamental's own
    // spectral leakage does not land in the 2nd-harmonic bin and get read as
    // a harmonic that isn't there. At a round 220Hz it does, at about 0.0014
    // relative - small, but the same order as a genuinely weak 2nd.
    constexpr double tone = 48000.0 * 75.0 / 16384.0;

    auto secondHarmonicOf = [](float asymmetry)
    {
        Waveshaper shaper;
        shaper.setDrive(6.0f);
        shaper.setAsymmetry(asymmetry);
        shaper.reset();

        std::vector<float> out;
        out.reserve((size_t) samples);
        for (int n = 0; n < samples; ++n)
            out.push_back(shaper.processSample(
                0.9f * (float) std::sin(2.0 * kPi * tone * n / sampleRate)));

        // Measured against the fundamental, so this is about the *shape* of
        // the curve rather than the level it happens to run at.
        return magnitudeAt(out, 2.0 * tone, sampleRate)
             / magnitudeAt(out, tone, sampleRate);
    };

    const double symmetric = secondHarmonicOf(0.0f);
    const double asymmetricRatio = secondHarmonicOf(0.4f);

    INFO("2nd/1st symmetric " << symmetric << "  asymmetric " << asymmetricRatio);

    REQUIRE(symmetric < 1.0e-5);       // none at all, as the maths says
    REQUIRE(asymmetricRatio > 0.05);   // and a clearly audible one with bias
}

TEST_CASE("Zero asymmetry is bit-identical to the symmetric shaper", "[engine][drive]")
{
    // The default has to be exactly the old behaviour, not merely close: every
    // existing preset and every existing test runs through this path, so a
    // silent drift in all of them would be indistinguishable from a retune.
    Waveshaper plain, biased;
    plain.setDrive(9.0f);
    biased.setDrive(9.0f);
    biased.setAsymmetry(0.0f);
    plain.reset();
    biased.reset();

    for (int n = 0; n < 4096; ++n)
    {
        const float x = 0.8f * (float) std::sin(2.0 * kPi * 330.0 * n / 48000.0);
        REQUIRE(plain.processSample(x) == biased.processSample(x));
    }
}

TEST_CASE("Asymmetry does not leave DC on the bus", "[engine][drive]")
{
    // Biasing the curve is the point; leaving the bias in the output is not.
    // DC eats headroom on every stage downstream and moves nothing audible.
    Waveshaper shaper;
    shaper.setDrive(6.0f);
    shaper.setAsymmetry(0.6f);
    shaper.reset();

    // Measured over the settled tail. The note starts at full amplitude on
    // sample 0, and the level-dependent part of the offset has to settle
    // through the blocker's ~40ms corner - averaging across that step would
    // be measuring the step, not the residual.
    const int samples = 48000;
    double    sum     = 0.0;
    int       counted = 0;

    for (int n = 0; n < samples; ++n)
    {
        const float y = shaper.processSample(
            0.9f * (float) std::sin(2.0 * kPi * 220.0 * n / 48000.0));

        if (n >= samples / 4)
        {
            sum += y;
            ++counted;
        }
    }

    REQUIRE(std::abs(sum / counted) < 0.005);

    // And with no input at all it must sit at exactly zero, since that is the
    // one case where the pedestal is the whole output.
    shaper.reset();
    for (int n = 0; n < 64; ++n)
        REQUIRE(std::abs(shaper.processSample(0.0f)) < 1.0e-6f);
}
