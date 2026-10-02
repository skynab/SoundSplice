#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ExportAudioDialog.h>
#include <app/ExportNaming.h>
#include <engine/ExportLoudness.h>

#include <cmath>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** Five seconds of a 1 kHz sine with sharp bursts on top, at @p level. */
    juce::AudioBuffer<float> programme(float level, bool bursts)
    {
        constexpr double rate = 48000.0;
        juce::AudioBuffer<float> audio(2, (int) (rate * 5.0));
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            float v = level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate);
            if (bursts && (i % 12000) < 200)
                v *= 4.0f; // peaks well above the body: what a limiter is for
            audio.setSample(0, i, v);
            audio.setSample(1, i, v);
        }
        return audio;
    }
}

TEST_CASE("Export loudness reaches the target and holds the true peak", "[gui][exportloudness]")
{
    JuceFixture fixture;

    SECTION("A quiet, even programme just gets louder")
    {
        auto audio  = programme(0.02f, false);
        const auto result = engine::normalizeForExport(audio, 48000.0, -16.0, -1.0);
        REQUIRE(result.measured);
        REQUIRE_FALSE(result.limited);
        REQUIRE_THAT(result.integratedLufs, WithinAbs(-16.0, 0.1));
        REQUIRE(result.truePeakDb <= -1.0 + 0.05);
    }

    SECTION("Peaks that would cross the ceiling are limited, and the true peak fits")
    {
        auto audio = programme(0.05f, true);
        const auto result = engine::normalizeForExport(audio, 48000.0, -10.0, -1.0);
        REQUIRE(result.measured);
        REQUIRE(result.limited);
        REQUIRE(result.truePeakDb <= -1.0 + 0.05);
        REQUIRE_THAT(result.integratedLufs, WithinAbs(-10.0, 1.0)); // limiting costs a little
        // Measured again from the file's samples, it says the same.
        const auto check = engine::exportloudness::measure(audio, 48000.0);
        REQUIRE_THAT(check.truePeakDb, WithinAbs(result.truePeakDb, 0.01));
    }

    SECTION("Silence is left alone")
    {
        juce::AudioBuffer<float> silence(2, 48000);
        silence.clear();
        const auto result = engine::normalizeForExport(silence, 48000.0, -16.0);
        REQUIRE_FALSE(result.measured);
        REQUIRE(silence.getMagnitude(0, silence.getNumSamples()) == 0.0f);
    }
}

TEST_CASE("Export file names come from a pattern", "[gui][exportnaming]")
{
    JuceFixture fixture;
    namespace naming = app::exportnaming;

    auto fields = naming::regionFields("Episode 12", "Intro", 0, 3);
    REQUIRE(naming::expand("$project - $region", fields) == "Episode 12 - Intro");
    REQUIRE(naming::expand("$index $region", fields) == "01 Intro");
    REQUIRE(naming::expand("$unknown $region", fields) == "$unknown Intro"); // a typo shows
    REQUIRE(naming::expand("$region/$region", fields).contains("/") == false); // made a legal name
    REQUIRE(naming::expand("", fields) == "01");

    // Unnamed regions, and more than 99 of them.
    REQUIRE(naming::expand("$region", naming::regionFields("", "  ", 4, 120)) == "Region 5");
    REQUIRE(naming::expand("$index", naming::regionFields("", "x", 4, 120)) == "005");
    REQUIRE(naming::expand("$project", naming::regionFields("", "x", 0, 1)) == "Untitled");

    REQUIRE(naming::distinct({ "A", "B", "A", "a" }) == juce::StringArray { "A", "B", "A (2)", "a (3)" });
}

TEST_CASE("The export dialog offers only the ranges there are", "[gui][exportnaming]")
{
    JuceFixture fixture;

    {
        juce::AlertWindow window("Export", {}, juce::MessageBoxIconType::NoIcon);
        app::ExportAudioDialog::buildControls(window, 48000.0, false, 0);
        REQUIRE(window.getComboBoxComponent("range")->getNumItems() == 1);
        REQUIRE(app::ExportAudioDialog::readRange(window) == app::ExportRange::Project);
        REQUIRE_FALSE(window.getTextEditor("names")->isEnabled());
        REQUIRE(app::ExportAudioDialog::readOptions(window).loudnessLufs == 0.0);
    }
    {
        juce::AlertWindow window("Export", {}, juce::MessageBoxIconType::NoIcon);
        app::ExportAudioDialog::buildControls(window, 48000.0, true, 4);
        auto* range = window.getComboBoxComponent("range");
        REQUIRE(range->getNumItems() == 3);
        range->setSelectedId(1 + (int) app::ExportRange::MarkerRanges, juce::sendNotificationSync);
        REQUIRE(app::ExportAudioDialog::readRange(window) == app::ExportRange::MarkerRanges);
        REQUIRE(window.getTextEditor("names")->isEnabled());
        REQUIRE(app::ExportAudioDialog::readNamePattern(window) == "$project - $region");

        window.getComboBoxComponent("loudness")->setSelectedItemIndex(2, juce::sendNotificationSync);
        REQUIRE(app::ExportAudioDialog::readOptions(window).loudnessLufs == -16.0);
    }
}
