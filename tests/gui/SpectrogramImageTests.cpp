#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <app/SpectrogramImage.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

TEST_CASE("The spectrogram's frequency axis is logarithmic, 20 Hz to the top", "[gui][spectrogram]")
{
    const double nyquist = 20480.0; // 20 Hz up ten octaves
    REQUIRE_THAT(spectrogramimage::frequencyAt(0.0, nyquist), WithinAbs(20.0, 1e-9));
    REQUIRE_THAT(spectrogramimage::frequencyAt(1.0, nyquist), WithinAbs(nyquist, 1e-6));
    REQUIRE_THAT(spectrogramimage::frequencyAt(0.5, nyquist), WithinAbs(640.0, 1e-6)); // five octaves up
    REQUIRE_THAT(spectrogramimage::proportionOf(640.0, nyquist), WithinAbs(0.5, 1e-9));
}

TEST_CASE("A spectrogram image puts a tone on the row for its frequency", "[gui][spectrogram]")
{
    constexpr double rate = 48000.0;
    std::vector<float> audio(48000);
    for (size_t n = 0; n < audio.size(); ++n)
        audio[n] = (float) (0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * (double) n / rate));

    engine::SpectrogramBuilder builder(rate, (std::int64_t) audio.size());
    const float*               channels[] { audio.data() };
    builder.append(channels, 1, (int) audio.size());
    const auto data = builder.finish();

    constexpr int rows  = 256;
    const auto    image = spectrogramimage::imageOf(data, rows);
    REQUIRE(image.getWidth() == data.columns);
    REQUIRE(image.getHeight() == rows);

    // The brightest rows in a column in the middle are centred on the one
    // 1 kHz falls on. Rows, not one row: a windowed tone spreads over a few
    // bins at nearly its full level, and near 1 kHz a row is about a bin.
    const int column = data.columns / 2;
    float     best   = 0.0f;
    for (int row = 0; row < rows; ++row)
        best = std::max(best, image.getPixelAt(column, row).getBrightness());

    int first = -1, last = -1;
    for (int row = 0; row < rows; ++row)
        if (image.getPixelAt(column, row).getBrightness() >= best - 1.0e-3f)
        {
            if (first < 0)
                first = row;
            last = row;
        }

    const double expected = (1.0 - spectrogramimage::proportionOf(1000.0, rate * 0.5)) * rows;
    REQUIRE(std::abs((first + last) * 0.5 - expected) <= 1.5);
    REQUIRE(last - first <= 6);

    // Silence is drawn at the bottom of the colour scale.
    REQUIRE(spectrogramimage::colourFor(-200.0f) == spectrogramimage::colourFor(spectrogramimage::kFloorDb));
    REQUIRE(spectrogramimage::colourFor(0.0f).getBrightness() > spectrogramimage::colourFor(-60.0f).getBrightness());
}

TEST_CASE("The spectrogram's linear and mel scales map frequencies both ways", "[gui][spectrogram]")
{
    using spectrogramimage::Scale;
    const double nyquist = 24000.0;

    REQUIRE_THAT(spectrogramimage::frequencyAt(0.5, nyquist, Scale::Linear), WithinAbs(12000.0, 1e-9));
    REQUIRE_THAT(spectrogramimage::proportionOf(6000.0, nyquist, Scale::Linear), WithinAbs(0.25, 1e-12));

    // Mel puts far more of the view below a few kilohertz than linear does,
    // and less than the octave scale does.
    const double mel = spectrogramimage::proportionOf(1000.0, nyquist, Scale::Mel);
    REQUIRE(mel > spectrogramimage::proportionOf(1000.0, nyquist, Scale::Linear));
    REQUIRE(mel < spectrogramimage::proportionOf(1000.0, nyquist, Scale::Logarithmic));

    for (auto scale : { Scale::Linear, Scale::Mel, Scale::Logarithmic })
        for (double hz : { 100.0, 1000.0, 7000.0, 20000.0 })
        {
            INFO((int) scale << " at " << hz);
            const double back = spectrogramimage::frequencyAt(spectrogramimage::proportionOf(hz, nyquist, scale), nyquist, scale);
            REQUIRE_THAT(back, WithinAbs(hz, hz * 1e-9));
        }
}

TEST_CASE("The healing brush covers what was painted, softly at its edges", "[gui][spectrogram]")
{
    spectrogramimage::Brush brush;
    brush.radiusSeconds    = 0.1;
    brush.radiusProportion = 0.05;
    brush.nyquist          = 24000.0;
    brush.scale            = spectrogramimage::Scale::Logarithmic;

    const double oneK = spectrogramimage::proportionOf(1000.0, brush.nyquist, brush.scale);
    brush.dabs.push_back({ 2.0, oneK });
    brush.dabs.push_back({ 2.05, oneK });

    REQUIRE(brush.amountAt(2.0, 1000.0) == 1.0f);         // under a dab
    REQUIRE(brush.amountAt(2.02, 1000.0) == 1.0f);        // between two that overlap
    REQUIRE(brush.amountAt(2.5, 1000.0) == 0.0f);         // well after
    REQUIRE(brush.amountAt(2.0, 8000.0) == 0.0f);         // well above
    const float edge = brush.amountAt(2.0 - 0.1 * 0.85, 1000.0); // the side away from the second dab
    REQUIRE(edge > 0.0f);                                  // the soft edge
    REQUIRE(edge < 1.0f);

    const auto [from, to] = brush.timeSpan();
    REQUIRE_THAT(from, WithinAbs(1.9, 1e-9));
    REQUIRE_THAT(to, WithinAbs(2.15, 1e-9));
}

TEST_CASE("The spectrogram's gain and range set how levels are coloured", "[gui][spectrogram]")
{
    const auto brightness = [](juce::Colour c) { return c.getPerceivedBrightness(); };

    // By default -100 dB is black and 0 dB the brightest.
    REQUIRE(brightness(spectrogramimage::colourFor(-100.0f)) < 0.1f);
    REQUIRE(brightness(spectrogramimage::colourFor(0.0f)) > 0.9f);

    // Gain brings a quiet level up; a narrower range makes the fall to black
    // steeper.
    spectrogramimage::Display loud;
    loud.gainDb = 30.0f;
    REQUIRE(spectrogramimage::colourFor(-30.0f, loud) == spectrogramimage::colourFor(0.0f));

    spectrogramimage::Display narrow;
    narrow.rangeDb = 50.0f;
    REQUIRE(brightness(spectrogramimage::colourFor(-50.0f, narrow)) < 0.1f);
    REQUIRE(spectrogramimage::colourFor(-25.0f, narrow) == spectrogramimage::colourFor(-50.0f));
}

TEST_CASE("The lasso covers what's inside its outline", "[gui][spectrogram]")
{
    spectrogramimage::Brush lasso;
    lasso.radiusSeconds    = 0.01;
    lasso.radiusProportion = 0.01;
    lasso.nyquist          = 24000.0;
    lasso.scale            = spectrogramimage::Scale::Linear;

    // A square from 1 to 2 seconds, 0.25 to 0.5 of the way up (6 - 12 kHz).
    lasso.outline = { { 1.0, 0.25 }, { 2.0, 0.25 }, { 2.0, 0.5 }, { 1.0, 0.5 } };
    REQUIRE(lasso.isLasso());

    REQUIRE(lasso.amountAt(1.5, 9000.0) == 1.0f);
    REQUIRE(lasso.amountAt(0.9, 9000.0) == 0.0f);
    REQUIRE(lasso.amountAt(1.5, 3000.0) == 0.0f);
    REQUIRE(lasso.amountAt(1.5, 13000.0) == 0.0f);
    const float edge = lasso.amountAt(1.001, 9000.0); // a tenth of the ramp in
    REQUIRE(edge > 0.0f);
    REQUIRE(edge < 1.0f);

    const auto [from, to] = lasso.timeSpan();
    REQUIRE_THAT(from, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(to, WithinAbs(2.0, 1e-12));
}

TEST_CASE("The harmonic brush covers the overtones of what it paints", "[gui][spectrogram]")
{
    spectrogramimage::Brush brush;
    brush.radiusSeconds    = 0.05;
    brush.radiusProportion = 0.02;
    brush.nyquist          = 24000.0;
    brush.scale            = spectrogramimage::Scale::Logarithmic;
    brush.dabs.push_back({ 1.0, spectrogramimage::proportionOf(220.0, brush.nyquist, brush.scale) });

    REQUIRE(brush.amountAt(1.0, 220.0) == 1.0f);
    REQUIRE(brush.amountAt(1.0, 660.0) == 0.0f);   // one brush, one pitch

    brush.harmonics = spectrogramimage::Brush::kHarmonics;
    for (int n = 1; n <= spectrogramimage::Brush::kHarmonics; ++n)
    {
        INFO(n);
        REQUIRE(brush.amountAt(1.0, 220.0 * n) == 1.0f);
    }
    REQUIRE(brush.amountAt(1.0, 220.0 * 2.5) == 0.0f);  // between them, nothing
    REQUIRE(brush.amountAt(1.2, 440.0) == 0.0f);        // nor after the stroke
}
