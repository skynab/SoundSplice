#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/DeliveryPane.h>
#include <app/ExportAudioDialog.h>

using namespace soundsplice;

TEST_CASE("The Delivery pane says whether the mix passes", "[gui][delivery]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    DeliveryPane pane;
    pane.setSize(700, 300);
    int checked = -1;
    pane.onCheck = [&](int spec) { checked = spec; };

    pane.setBusy();
    REQUIRE(pane.summaryForTesting().contains("Rendering"));

    pane.showResults(2, { { "Integrated loudness", "-16.2 LUFS", "-17.0 to -15.0 LUFS", true, "" },
                          { "True peak", "-0.4 dBTP", "at most -1.0 dBTP", false, "Export with this spec's loudness target" } });
    REQUIRE(pane.specIndex() == 2);
    REQUIRE(pane.summaryForTesting() == "1 thing fails Apple Podcasts");

    pane.showResults(2, { { "Integrated loudness", "-16.0 LUFS", "-17.0 to -15.0 LUFS", true, "" } });
    REQUIRE(pane.summaryForTesting() == "Passes Apple Podcasts");
}

TEST_CASE("Export Audio keeps a delivery spec's own loudness target and ceiling", "[gui][delivery]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    juce::AlertWindow window("Export", {}, juce::MessageBoxIconType::NoIcon);
    app::ExportAudioDialog::buildControls(window, 48000.0);

    app::ExportChoice acx;
    acx.options.format            = engine::ExportFormat::Mp3;
    acx.options.sampleRate        = 44100.0;
    acx.options.loudnessLufs      = -19.0; // not in the list
    acx.options.truePeakCeilingDb = -3.5;
    app::ExportAudioDialog::applyChoice(window, acx, 48000.0);

    auto read = app::ExportAudioDialog::readChoice(window);
    REQUIRE(read.options.loudnessLufs == -19.0);
    REQUIRE(read.options.truePeakCeilingDb == -3.5);
    REQUIRE(read.options.format == engine::ExportFormat::Mp3);

    // Choosing a listed target goes back to the usual ceiling.
    window.getComboBoxComponent("loudness")->setSelectedItemIndex(2, juce::sendNotificationSync);
    read = app::ExportAudioDialog::readChoice(window);
    REQUIRE(read.options.loudnessLufs == -16.0);
    REQUIRE(read.options.truePeakCeilingDb == -1.0);
}
