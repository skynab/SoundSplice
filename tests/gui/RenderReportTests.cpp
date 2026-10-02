#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/RenderReport.h>

#include <cmath>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

TEST_CASE("A render report measures the file and lists its clips", "[gui][renderreport]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    // Ten seconds: silent for the first two, then a steady sine.
    constexpr double rate = 48000.0;
    juce::AudioBuffer<float> audio(2, (int) (rate * 10.0));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float v = i < (int) (rate * 2.0) ? 0.0f : 0.25f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate);
        audio.setSample(0, i, v);
        audio.setSample(1, i, v);
    }

    auto report = app::renderreport::analyse(audio, rate);
    REQUIRE_THAT(report.seconds, WithinAbs(10.0, 1e-9));
    REQUIRE(report.shortTerm.size() == 10);           // a point a second
    REQUIRE_FALSE(std::isfinite(report.shortTerm[1].second)); // not yet three seconds of anything
    REQUIRE(std::isfinite(report.shortTerm.back().second));
    REQUIRE_THAT(report.amplitude.peakDb, WithinAbs(20.0 * std::log10(0.25), 0.1));
    REQUIRE(std::isfinite(report.loudness.integratedLufs));

    report.fileName = "Show <final> & \"done\".wav";
    report.format   = "WAV";
    report.project  = "Show";
    report.clips    = { { "Voice", "take one.wav", 2.0, 8.0 } };
    const auto page = app::renderreport::html(report);

    REQUIRE(page.startsWith("<!doctype html>"));
    REQUIRE(page.contains("Show &lt;final&gt; &amp; &quot;done&quot;.wav")); // escaped
    REQUIRE_FALSE(page.contains("<final>"));
    REQUIRE(page.contains("<polyline points=\""));
    REQUIRE(page.contains("take one.wav"));
    REQUIRE(page.contains("0:02.0"));
    REQUIRE(page.contains("LUFS"));

    REQUIRE(app::renderreport::fileFor(juce::File::getCurrentWorkingDirectory().getChildFile("Mix.wav")).getFileName()
            == "Mix report.html");
}
