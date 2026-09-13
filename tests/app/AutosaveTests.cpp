#include <catch2/catch_test_macros.hpp>

#include <app/Autosave.h>
#include <model/Serialization.h>

using namespace soundsplice;

TEST_CASE("An autosave unwraps to the project and the file it came from", "[app][autosave]")
{
    const std::string project = "SOUNDSPLICE 1\nsome project text\n";

    app::AutosaveContents contents;
    REQUIRE(app::unwrapAutosave(app::wrapAutosave(project, "C:/Music/My Song.soundsplice"), contents));
    REQUIRE(contents.originalPath == "C:/Music/My Song.soundsplice");
    REQUIRE(contents.projectText == project);

    // An untitled project has no file to point back at.
    REQUIRE(app::unwrapAutosave(app::wrapAutosave(project, ""), contents));
    REQUIRE(contents.originalPath.empty());
    REQUIRE(contents.projectText == project);
}

TEST_CASE("A recovered autosave loads as the same song", "[app][autosave]")
{
    model::Song song;
    auto& track = model::addTrack(song, model::TrackType::Audio, "Vocal");
    track.gainDb = -3.5f;
    song.bpm     = 97.0;

    app::AutosaveContents contents;
    REQUIRE(app::unwrapAutosave(app::wrapAutosave(model::serialize(song), "/projects/vocal.soundsplice"),
                                contents));

    model::Song restored;
    std::string error;
    REQUIRE(model::deserialize(contents.projectText, restored, &error));
    REQUIRE(restored == song);
}

TEST_CASE("Only an autosave unwraps as one", "[app][autosave]")
{
    app::AutosaveContents contents;

    // A plain project file is not an autosave...
    model::Song song;
    REQUIRE_FALSE(app::unwrapAutosave(model::serialize(song), contents));

    // ...and an autosave is not a project file, so Open can't load one by accident.
    model::Song fromAutosave;
    std::string error;
    REQUIRE_FALSE(model::deserialize(app::wrapAutosave(model::serialize(song), ""), fromAutosave, &error));

    // Cut off before any project text: nothing to recover.
    REQUIRE_FALSE(app::unwrapAutosave("", contents));
    REQUIRE_FALSE(app::unwrapAutosave(std::string(app::kAutosaveHeader), contents));
    REQUIRE_FALSE(app::unwrapAutosave(std::string(app::kAutosaveHeader) + "\nPROJECT /a.soundsplice\n", contents));
    REQUIRE_FALSE(app::unwrapAutosave(std::string(app::kAutosaveHeader) + "\nnot the project line\nSOUNDSPLICE 1\n",
                                      contents));
}

TEST_CASE("An autosave written with Windows line endings still unwraps", "[app][autosave]")
{
    app::AutosaveContents contents;
    const std::string text = std::string(app::kAutosaveHeader) + "\r\nPROJECT D:/song.soundsplice\r\nSOUNDSPLICE 1\r\n";

    REQUIRE(app::unwrapAutosave(text, contents));
    REQUIRE(contents.originalPath == "D:/song.soundsplice");
    REQUIRE(contents.projectText == "SOUNDSPLICE 1\r\n");
}

TEST_CASE("An autosave is due only for new unsaved changes, and not too often", "[app][autosave]")
{
    constexpr double kInterval = 30.0;

    // Unsaved, never autosaved, interval passed: write.
    REQUIRE(app::autosaveDue(5, 2, 0, 31.0, kInterval));

    // What's on screen is what's on disk: nothing to protect.
    REQUIRE_FALSE(app::autosaveDue(5, 5, 0, 31.0, kInterval));

    // The last autosave already holds exactly this state.
    REQUIRE_FALSE(app::autosaveDue(5, 2, 5, 300.0, kInterval));

    // A newer change, but too soon after the last write.
    REQUIRE_FALSE(app::autosaveDue(6, 2, 5, 10.0, kInterval));
    REQUIRE(app::autosaveDue(6, 2, 5, 30.0, kInterval));
}
