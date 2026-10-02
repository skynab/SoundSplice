#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/TranscriptPane.h>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

namespace
{
    std::vector<model::textedit::TrackWord> words()
    {
        // "So um we went [2 s] to the shop."
        return { { 1, 0, "So", 0.5, 0.8 },  { 1, 1, "um,", 1.0, 1.4 }, { 1, 2, "we", 1.5, 1.7 }, { 1, 3, "went", 1.7, 2.0 },
                 { 1, 4, "to", 4.0, 4.1 }, { 1, 5, "the", 4.1, 4.3 }, { 1, 6, "shop.", 4.3, 4.8 } };
    }
}

TEST_CASE("The Transcript pane cuts what's selected, and finds fillers and pauses for review", "[gui][transcript]")
{
    juce::ScopedJuceInitialiser_GUI juce;

    TranscriptPane pane;
    pane.setSize(500, 300);
    pane.setWords(words(), "nothing");
    REQUIRE(pane.tokenCount() == 8); // seven words and the pause between "went" and "to"

    std::vector<std::pair<double, double>> cut;
    double seekedTo = -1.0;
    pane.onDelete = [&](const auto& ranges) { cut = ranges; };
    pane.onSeek   = [&](double s) { seekedTo = s; };

    // A click puts the playhead on the word.
    pane.selectTokenForTesting(2, false, false);
    REQUIRE(seekedTo == 1.5);

    // "we went" selected and deleted: from "we" to just after "went" - the
    // following pause is a token of its own, so only the usual breath.
    pane.selectTokenForTesting(3, true, false);
    REQUIRE(pane.selectedCount() == 2);
    pane.deleteForTesting();
    REQUIRE(cut.size() == 1);
    REQUIRE_THAT(cut[0].first, WithinAbs(1.5, 1e-9));
    REQUIRE_THAT(cut[0].second, WithinAbs(2.0, 1e-9));

    // Fillers: "um," alone, cut with the breath before "we".
    pane.findFillersForTesting();
    REQUIRE(pane.selectedCount() == 1);
    pane.deleteForTesting();
    REQUIRE(cut.size() == 1);
    REQUIRE_THAT(cut[0].first, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(cut[0].second, WithinAbs(1.5, 1e-9)); // up to "we", not into it

    // Long pauses: shortened, not closed up.
    pane.findPausesForTesting();
    REQUIRE(pane.selectedCount() == 1);
    pane.deleteForTesting();
    REQUIRE(cut.size() == 1);
    REQUIRE_THAT(cut[0].first, WithinAbs(2.0 + 0.5 * TranscriptPane::kKeepSeconds, 1e-9));
    REQUIRE_THAT(cut[0].second, WithinAbs(4.0 - 0.5 * TranscriptPane::kKeepSeconds, 1e-9));

    // Review: Ctrl-click lets one go before Delete.
    pane.findFillersForTesting();
    pane.selectTokenForTesting(1, false, true);
    REQUIRE(pane.selectedCount() == 0);

    // New words clear the selection.
    pane.selectTokenForTesting(0, false, false);
    pane.setWords(words(), "nothing");
    REQUIRE(pane.selectedCount() == 0);
}
