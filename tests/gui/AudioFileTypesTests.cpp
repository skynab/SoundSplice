#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/AudioFileTypes.h>

using namespace looper;

TEST_CASE("Every importable extension is recognised", "[app][audiofiles]")
{
    for (const char* name : { "take.wav", "take.aiff", "take.aif",
                              "take.flac", "take.ogg", "take.mp3" })
    {
        INFO(name);
        REQUIRE(audiofiles::isImportableAudioFile(juce::String(name)));
    }
}

TEST_CASE("Extensions are matched case-insensitively", "[app][audiofiles]")
{
    // Files exported by other tools routinely arrive as .WAV or .Mp3, and a
    // case-sensitive check would refuse a drag with no explanation.
    for (const char* name : { "TAKE.WAV", "Take.Mp3", "take.AIFF" })
    {
        INFO(name);
        REQUIRE(audiofiles::isImportableAudioFile(juce::String(name)));
    }
}

TEST_CASE("Non-audio files are refused", "[app][audiofiles]")
{
    for (const char* name : { "song.looper", "notes.txt", "cover.png", "take", "take.wav.txt" })
    {
        INFO(name);
        REQUIRE_FALSE(audiofiles::isImportableAudioFile(juce::String(name)));
    }
}

TEST_CASE("A drag is interesting if any file in it is audio", "[app][audiofiles]")
{
    // Selecting a folder's worth of files in Finder routinely picks up a
    // stray README. Refusing the whole drag because of it would be worse
    // than importing what's importable.
    juce::StringArray mixed { "/tmp/notes.txt", "/tmp/take.wav" };
    REQUIRE(audiofiles::containsImportableAudio(mixed));

    juce::StringArray none { "/tmp/notes.txt", "/tmp/cover.png" };
    REQUIRE_FALSE(audiofiles::containsImportableAudio(none));

    REQUIRE_FALSE(audiofiles::containsImportableAudio({}));
}

TEST_CASE("The chooser's wildcards cover exactly what drops accept", "[app][audiofiles]")
{
    // The bug this guards against is the two lists drifting: a format the
    // file chooser offers but a drop silently ignores reads as "drag and
    // drop is broken", not as "that format isn't supported".
    const juce::StringArray wildcards =
        juce::StringArray::fromTokens(juce::String(audiofiles::wildcards()), ";", "");

    REQUIRE_FALSE(wildcards.isEmpty());
    for (const auto& wildcard : wildcards)
    {
        // "*.wav" -> "file.wav"
        const auto name = juce::String("file") + wildcard.fromFirstOccurrenceOf("*", false, false);
        INFO(wildcard << " -> " << name);
        REQUIRE(audiofiles::isImportableAudioFile(name));
    }
}

TEST_CASE("importableFilesIn skips files that aren't on disk", "[app][audiofiles]")
{
    // A path in a drag can name something that no longer exists — a stale
    // alias, or a file moved between the drag starting and the drop. Opening
    // it would fail later and more confusingly than skipping it here.
    juce::StringArray paths { "/definitely/not/here/take.wav" };
    REQUIRE(audiofiles::importableFilesIn(paths).isEmpty());
}

TEST_CASE("importableFilesIn keeps real files, in order", "[app][audiofiles]")
{
    // Order matters: a multi-file drop lays clips out in sequence, so the
    // result has to follow the order the drag reported.
    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("looper-audiofiles-test");
    directory.deleteRecursively();
    directory.createDirectory();

    const auto first  = directory.getChildFile("a.wav");
    const auto second = directory.getChildFile("b.mp3");
    const auto other  = directory.getChildFile("notes.txt");
    for (const auto& file : { first, second, other })
        file.replaceWithText("x");

    juce::StringArray paths { other.getFullPathName(),
                              first.getFullPathName(),
                              second.getFullPathName() };

    const auto found = audiofiles::importableFilesIn(paths);
    REQUIRE(found.size() == 2);
    REQUIRE(found[0] == first);
    REQUIRE(found[1] == second);

    directory.deleteRecursively();
}

// --- drop targets ----------------------------------------------------------

#include <app/ArrangementView.h>
#include <app/AudioEditorPane.h>
#include <app/MasteringPane.h>

namespace
{
    struct JuceFixture { juce::ScopedJuceInitialiser_GUI juce; };

    model::Song songWithOneTrack()
    {
        model::Song song;
        model::addTrack(song, model::TrackType::Instrument, "Synth");
        return song;
    }
}

TEST_CASE("The timeline accepts audio dragged from the OS", "[gui][filedrop]")
{
    // The bug this exists for: ArrangementView implemented only
    // juce::DragAndDropTarget, which handles drags starting *inside* the
    // app. A drag from Finder needs juce::FileDragAndDropTarget, so dropping
    // a file from the desktop did nothing whatsoever.
    JuceFixture fixture;

    ArrangementView view;
    view.setVisible(true);
    view.setSize(900, 400);
    view.setSong(songWithOneTrack());

    auto& target = static_cast<juce::FileDragAndDropTarget&>(view);
    REQUIRE(target.isInterestedInFileDrag({ "/tmp/take.wav" }));
    REQUIRE_FALSE(target.isInterestedInFileDrag({ "/tmp/notes.txt" }));
}

TEST_CASE("The mastering pane accepts audio dragged from the OS", "[gui][filedrop]")
{
    JuceFixture fixture;

    MasteringPane pane;
    pane.setVisible(true);
    pane.setSize(600, 500);

    auto& target = static_cast<juce::FileDragAndDropTarget&>(pane);
    REQUIRE(target.isInterestedInFileDrag({ "/tmp/take.wav" }));
    REQUIRE_FALSE(target.isInterestedInFileDrag({ "/tmp/cover.png" }));
}

TEST_CASE("Dropping several files on the mastering pane reports them all", "[gui][filedrop]")
{
    JuceFixture fixture;

    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("looper-filedrop-test");
    directory.deleteRecursively();
    directory.createDirectory();

    const auto first  = directory.getChildFile("one.wav");
    const auto second = directory.getChildFile("two.wav");
    const auto other  = directory.getChildFile("notes.txt");
    for (const auto& file : { first, second, other })
        file.replaceWithText("x");

    MasteringPane pane;
    pane.setVisible(true);
    pane.setSize(600, 500);

    juce::Array<juce::File> reported;
    pane.onFilesDropped = [&](const juce::Array<juce::File>& files) { reported = files; };

    static_cast<juce::FileDragAndDropTarget&>(pane).filesDropped(
        { other.getFullPathName(), first.getFullPathName(), second.getFullPathName() }, 10, 10);

    // The non-audio file is skipped rather than failing the whole drop.
    REQUIRE(reported.size() == 2);
    REQUIRE(reported[0] == first);
    REQUIRE(reported[1] == second);

    directory.deleteRecursively();
}

TEST_CASE("A multi-file drop on the timeline spaces the clips out", "[gui][filedrop]")
{
    // Dropping three takes at one beat would stack them, and only the first
    // would be audible — which looks like the other two failed to import.
    JuceFixture fixture;

    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("looper-filedrop-timeline");
    directory.deleteRecursively();
    directory.createDirectory();

    const auto first  = directory.getChildFile("one.wav");
    const auto second = directory.getChildFile("two.wav");
    for (const auto& file : { first, second })
        file.replaceWithText("x");

    ArrangementView view;
    view.setVisible(true);
    view.setSize(900, 400);
    view.setSong(songWithOneTrack());

    std::vector<double> beats;
    view.onFileDropped = [&](const juce::File&, double beat, int) { beats.push_back(beat); };

    static_cast<juce::FileDragAndDropTarget&>(view).filesDropped(
        { first.getFullPathName(), second.getFullPathName() }, 300, 60);

    REQUIRE(beats.size() == 2);
    REQUIRE(beats[1] > beats[0]);

    directory.deleteRecursively();
}

TEST_CASE("The audio editor accepts audio dragged from the OS", "[gui][filedrop]")
{
    JuceFixture fixture;

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setSize(800, 300);

    auto& target = static_cast<juce::FileDragAndDropTarget&>(pane);
    REQUIRE(target.isInterestedInFileDrag({ "/tmp/take.wav" }));
    REQUIRE_FALSE(target.isInterestedInFileDrag({ "/tmp/notes.txt" }));
}

TEST_CASE("Dropping files on the audio editor reports the audio ones", "[gui][filedrop]")
{
    JuceFixture fixture;

    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("looper-audioeditor-drop");
    directory.deleteRecursively();
    directory.createDirectory();

    const auto wav   = directory.getChildFile("take.wav");
    const auto notes = directory.getChildFile("notes.txt");
    for (const auto& file : { wav, notes })
        file.replaceWithText("x");

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setSize(800, 300);

    juce::Array<juce::File> reported;
    pane.onFilesDropped = [&](const juce::Array<juce::File>& files) { reported = files; };

    static_cast<juce::FileDragAndDropTarget&>(pane).filesDropped(
        { notes.getFullPathName(), wav.getFullPathName() }, 10, 10);

    REQUIRE(reported.size() == 1);
    REQUIRE(reported[0] == wav);

    directory.deleteRecursively();
}

// --- drags from the app's own Files pane -----------------------------------

TEST_CASE("A drag description round-trips to its file", "[gui][filedrop]")
{
    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("looper-dragdesc");
    directory.deleteRecursively();
    directory.createDirectory();

    const auto file = directory.getChildFile("take one.wav"); // a space, deliberately
    file.replaceWithText("x");

    const auto description = audiofiles::dragDescriptionFor(file);
    REQUIRE(audiofiles::fileFromDragDescription(description) == file);

    directory.deleteRecursively();
}

TEST_CASE("A foreign drag description is not mistaken for a file", "[gui][filedrop]")
{
    // The dock uses description strings for its own panel-regrouping drags,
    // so a target must not treat every drag as a file drop.
    REQUIRE(audiofiles::fileFromDragDescription(juce::var("Mixer")) == juce::File{});
    REQUIRE(audiofiles::fileFromDragDescription(juce::var()) == juce::File{});
    REQUIRE(audiofiles::fileFromDragDescription(juce::var("looper:file:/definitely/not/here.wav"))
            == juce::File{});
}

TEST_CASE("The timeline accepts a drag from the Files pane", "[gui][filedrop]")
{
    // The bug this covers: ArrangementView only recognised drags whose source
    // component was a FileTreeComponent, so dragging from the file *grid* —
    // where the files actually are — was silently ignored.
    JuceFixture fixture;

    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("looper-gridsource");
    directory.deleteRecursively();
    directory.createDirectory();
    const auto file = directory.getChildFile("take.wav");
    file.replaceWithText("x");

    ArrangementView view;
    view.setVisible(true);
    view.setSize(900, 400);
    view.setSong(songWithOneTrack());

    // A source component that is *not* a FileTreeComponent, which is exactly
    // the case that used to be refused.
    juce::Component someOtherSource;
    const juce::DragAndDropTarget::SourceDetails details(
        audiofiles::dragDescriptionFor(file), &someOtherSource, { 300, 60 });

    auto& target = static_cast<juce::DragAndDropTarget&>(view);
    REQUIRE(target.isInterestedInDragSource(details));

    juce::File dropped;
    view.onFileDropped = [&](const juce::File& f, double, int) { dropped = f; };
    target.itemDropped(details);

    REQUIRE(dropped == file);

    directory.deleteRecursively();
}
