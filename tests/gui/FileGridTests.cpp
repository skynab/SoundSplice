#include <catch2/catch_test_macros.hpp>

#include <app/FileGrid.h>
#include <engine/AudioExport.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    void writeSilence(const juce::File& file, int channels, double rate, int bits)
    {
        juce::AudioBuffer<float> audio(channels, (int) rate);
        audio.clear();
        engine::ExportOptions options;
        options.sampleRate    = rate;
        options.bitsPerSample = bits;
        REQUIRE(engine::writeAudioFile(file, audio, options));
    }
}

TEST_CASE("The file list shows what each file's header holds", "[gui][files]")
{
    JuceFixture fixture;
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("soundsplice-grid-test");
    folder.deleteRecursively();
    folder.createDirectory();
    const auto mono = folder.getChildFile("mono.wav");
    writeSilence(mono, 1, 44100.0, 16);

    FileGrid grid;
    REQUIRE(grid.sampleRateForTesting(mono) == 44100.0);
    REQUIRE(grid.channelsForTesting(mono) == 1);
    REQUIRE(grid.bitsForTesting(mono) == 16);
    folder.deleteRecursively();
}

TEST_CASE("Favorites lists every starred file, wherever it is", "[gui][files]")
{
    JuceFixture fixture;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("soundsplice-fav-test");
    root.deleteRecursively();
    const auto a = root.getChildFile("one").getChildFile("a.wav");
    const auto b = root.getChildFile("two").getChildFile("b.wav");
    a.getParentDirectory().createDirectory();
    b.getParentDirectory().createDirectory();
    writeSilence(a, 2, 48000.0, 24);
    writeSilence(b, 2, 48000.0, 24);

    FileGrid grid;
    grid.setDirectory(a.getParentDirectory());
    REQUIRE(grid.rowCountForTesting() == 1);

    grid.toggleFavoriteForTesting(a);
    grid.toggleFavoriteForTesting(b);
    grid.showFavorites();
    REQUIRE(grid.showingFavorites());
    REQUIRE(grid.rowCountForTesting() == 2); // from two folders

    grid.toggleFavoriteForTesting(a); // unstarred: it leaves the list
    REQUIRE(grid.rowCountForTesting() == 1);

    grid.setDirectory(a.getParentDirectory());
    REQUIRE_FALSE(grid.showingFavorites());
    root.deleteRecursively();
}
