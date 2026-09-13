#include <catch2/catch_test_macros.hpp>

#include <app/StemNaming.h>

using namespace looper::app;

TEST_CASE("A stem file name carries its track index", "[app][stems]")
{
    // The index makes the folder sort in track order rather than
    // alphabetically, which is the order the tracks are on screen.
    CHECK(stemFileName(1, "Drums", "wav") == "01 Drums.wav");
    CHECK(stemFileName(2, "Bass", "wav") == "02 Bass.wav");
    CHECK(stemFileName(10, "Vocals", "flac") == "10 Vocals.flac");
}

TEST_CASE("Two tracks with the same name get different files", "[app][stems]")
{
    // The app lets tracks share a name, and two audio tracks added without
    // renaming will. Without the index they would land on the same file and
    // the second would silently overwrite the first.
    CHECK(stemFileName(1, "Audio", "wav") != stemFileName(2, "Audio", "wav"));
}

TEST_CASE("A track name with path separators can't escape the folder", "[app][stems]")
{
    // "Gtr / Lead" is a name someone would actually type. Untreated it would
    // try to write into a subdirectory that doesn't exist, and the export
    // would fail for a reason nobody could guess from the message.
    for (const char* name : { "Gtr / Lead", "Drums\\Kick", "../escape", "a:b*c?d" })
    {
        const auto result = stemFileName(1, name, "wav");
        INFO("name [" << name << "] -> " << result);

        CHECK_FALSE(result.containsChar('/'));
        CHECK_FALSE(result.containsChar('\\'));
        CHECK(result.endsWith(".wav"));
        CHECK(result.startsWith("01 "));
    }
}

TEST_CASE("An empty track name still produces an openable file", "[app][stems]")
{
    // A file called ".wav" is hidden by some browsers and refused by others,
    // so an unnamed track has to get something rather than nothing.
    for (const char* name : { "", "   ", "///", "..." })
    {
        const auto result = stemFileName(3, name, "wav");
        INFO("name [" << name << "] -> " << result);

        CHECK(result.startsWith("03 "));
        CHECK(result.endsWith(".wav"));
        CHECK(result.length() > juce::String("03 .wav").length());
    }
}

TEST_CASE("An index below one is still valid", "[app][stems]")
{
    // Callers pass a 1-based index; a 0 would produce "00 " which sorts oddly
    // but must not produce something unopenable.
    const auto result = stemFileName(0, "Drums", "wav");
    CHECK(result.endsWith("Drums.wav"));
    CHECK_FALSE(result.startsWith("."));
}

TEST_CASE("Stems go in a folder beside the master file", "[app][stems]")
{
    // A folder, not siblings: a project with eight tracks would otherwise
    // scatter nine files into whatever directory was browsed to.
    const juce::File master("/tmp/somewhere/My Mix.wav");
    const auto       folder = stemFolderFor(master);

    CHECK(folder.getParentDirectory() == master.getParentDirectory());
    CHECK(folder.getFileName() == "My Mix stems");

    // And the folder is not the master file, which would be a delightful way
    // to destroy the export that was just written.
    CHECK(folder != master);
}
