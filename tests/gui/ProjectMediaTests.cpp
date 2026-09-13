#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include <set>

#include <app/ProjectMedia.h>
#include <model/History.h>
#include <model/Serialization.h>

using namespace soundsplice;
namespace media = soundsplice::app::media;

namespace
{
    /** A fresh folder under the temp directory, removed afterwards. */
    struct TempFolder
    {
        juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SoundSpliceProjectMediaTests")
                              .getNonexistentChildFile("run", "", false);

        TempFolder() { root.createDirectory(); }
        ~TempFolder() { root.deleteRecursively(); }
    };

    juce::File writeFile(const juce::File& file, const juce::String& contents)
    {
        file.getParentDirectory().createDirectory();
        file.replaceWithText(contents);
        return file;
    }

    model::Song songUsing(const juce::File& arranged, const juce::File& inSession)
    {
        model::Song song;
        auto& track = model::addTrack(song, model::TrackType::Audio, "Take");

        model::Clip clip;
        clip.type      = model::ClipType::Audio;
        clip.audioFile = media::pathOf(arranged);
        track.clips.push_back(clip);

        model::SessionSlot slot;
        slot.hasClip        = true;
        slot.clip           = clip;
        slot.clip.audioFile = media::pathOf(inSession);
        track.sessionSlots.push_back(slot);

        return song;
    }
}

TEST_CASE("A project's audio folder sits beside its file", "[gui][projectmedia]")
{
    TempFolder temp;
    const auto project = temp.root.getChildFile("My Song.soundsplice");
    REQUIRE(media::audioFolderFor(project) == temp.root.getChildFile("My Song Audio"));
}

TEST_CASE("Audio inside a project's folder is stored relative, anything else in full", "[gui][projectmedia]")
{
    TempFolder temp;
    const auto project = temp.root.getChildFile("song.soundsplice");
    const auto inside  = temp.root.getChildFile("song Audio").getChildFile("take 1.wav");
    const auto outside = temp.root.getParentDirectory().getChildFile("my library.wav");

    REQUIRE(media::storedPath(inside, project) == "song Audio/take 1.wav");
    REQUIRE(media::storedPath(outside, project) == media::pathOf(outside));

    REQUIRE(media::resolvedPath("song Audio/take 1.wav", project) == inside);
    REQUIRE(media::resolvedPath(media::pathOf(outside), project) == outside);
}

TEST_CASE("A project moved with its audio folder still finds its audio", "[gui][projectmedia]")
{
    TempFolder temp;
    const auto here  = temp.root.getChildFile("here");
    const auto there = temp.root.getChildFile("there");

    const auto project = here.getChildFile("song.soundsplice");
    const auto song    = songUsing(here.getChildFile("song Audio/a.wav"), here.getChildFile("song Audio/b.wav"));

    // Written, then read back as if the folder had been moved.
    model::Song loaded;
    std::string error;
    REQUIRE(model::deserialize(model::serialize(media::withStoredPaths(song, project)), loaded, &error));
    REQUIRE(loaded.tracks[0].clips[0].audioFile == "song Audio/a.wav");

    const auto moved = media::withResolvedPaths(loaded, there.getChildFile("song.soundsplice"));
    REQUIRE(media::fileFromPath(moved.tracks[0].clips[0].audioFile) == there.getChildFile("song Audio/a.wav"));
    REQUIRE(media::fileFromPath(moved.tracks[0].sessionSlots[0].clip.audioFile)
            == there.getChildFile("song Audio/b.wav"));
}

TEST_CASE("Only audio in the app's own folders is collected into a project", "[gui][projectmedia]")
{
    TempFolder temp;
    const auto recordings   = temp.root.getChildFile("Recordings");
    const auto projectAudio = temp.root.getChildFile("song Audio");
    const std::vector<juce::File> owned { recordings };

    REQUIRE(media::shouldCollect(recordings.getChildFile("take.wav"), projectAudio, owned));
    REQUIRE_FALSE(media::shouldCollect(projectAudio.getChildFile("take.wav"), projectAudio, owned));
    REQUIRE_FALSE(media::shouldCollect(temp.root.getChildFile("library/beat.wav"), projectAudio, owned));
}

TEST_CASE("Collecting copies a file once, and never over a different one", "[gui][projectmedia]")
{
    TempFolder temp;
    const auto folder = temp.root.getChildFile("song Audio");
    const auto first  = writeFile(temp.root.getChildFile("a/take.wav"), "first take");

    const auto copy = media::collectInto(first, folder);
    REQUIRE(copy == folder.getChildFile("take.wav"));
    REQUIRE(copy.loadFileAsString() == "first take");

    // Saving again finds the identical copy rather than making another.
    REQUIRE(media::collectInto(first, folder) == copy);
    REQUIRE(folder.getNumberOfChildFiles(juce::File::findFiles) == 1);

    // A different recording with the same name gets a name of its own.
    const auto second     = writeFile(temp.root.getChildFile("b/take.wav"), "second take");
    const auto secondCopy = media::collectInto(second, folder);
    REQUIRE(secondCopy != copy);
    REQUIRE(secondCopy.getParentDirectory() == folder);
    REQUIRE(secondCopy.loadFileAsString() == "second take");
    REQUIRE(copy.loadFileAsString() == "first take");

    // What can't be copied stays where it is.
    const auto missing = temp.root.getChildFile("gone.wav");
    REQUIRE(media::collectInto(missing, folder) == missing);
}

TEST_CASE("Unused audio is found, and used audio and other files are left alone", "[gui][projectmedia]")
{
    TempFolder temp;
    const auto folder = temp.root.getChildFile("song Audio");
    const auto used   = writeFile(folder.getChildFile("used.wav"), "x");
    const auto unused = writeFile(folder.getChildFile("unused.wav"), "x");
    writeFile(folder.getChildFile("notes.txt"), "not audio");
    writeFile(folder.getChildFile("nested/deep.wav"), "not directly in the folder");

    juce::Array<juce::File> referenced;
    referenced.add(used);

    const auto found = media::unusedAudioFiles(folder, referenced);
    REQUIRE(found.size() == 1);
    REQUIRE(found[0] == unused);
}

TEST_CASE("The history visits every state the user can still get back to", "[gui][projectmedia]")
{
    model::History<model::Song> history;
    history.edit("one", [](model::Song& s) { s.bpm = 101.0; });
    history.edit("two", [](model::Song& s) { s.bpm = 102.0; });
    history.edit("three", [](model::Song& s) { s.bpm = 103.0; });
    history.undo();

    std::set<double> seen;
    history.forEachState([&seen](const model::Song& s) { seen.insert(s.bpm); });

    REQUIRE(seen == std::set<double> { 120.0, 101.0, 102.0, 103.0 });
}
