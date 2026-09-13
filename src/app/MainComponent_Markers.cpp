#include "MainComponentInternal.h"

#include "model/Markers.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Markers: adding, jumping between, renaming and deleting them, and moving
// them in and out as Audacity label files. The rules live in model/Markers.h.

namespace soundsplice
{
/** Adds a point marker at the playhead, named "Marker N". */
void MainComponent::addMarkerAtPlayhead()
{
    const double beat = playheadBeat();
    std::string  name;

    history_.edit("Add marker", [beat, &name](model::Song& s)
    {
        name = model::nextMarkerName(s);
        model::addMarker(s, beat, 0.0, name);
    });

    arrangementView_.setSong(history_.current());
    showStatus("Added " + juce::String::fromUTF8(name.c_str()) + " - double-click it on the ruler to rename it");
}

/** Marks the audio editor's selection as a range marker, at the place on the
    timeline where that stretch of the clip plays. */
void MainComponent::addMarkerFromAudioSelection()
{
    const auto  range = audioEditor_.selection();
    if (selectedAudioClip() == nullptr || range.isEmpty())
    {
        showError("Select part of a clip in the audio editor first");
        return;
    }

    const double start = songBeatForClipSeconds(range.startSeconds);
    const double end   = songBeatForClipSeconds(range.endSeconds);
    std::string  name;

    history_.edit("Add marker", [start, end, &name](model::Song& s)
    {
        name = model::nextMarkerName(s);
        model::addMarker(s, start, end - start, name);
    });

    arrangementView_.setSong(history_.current());
    showStatus("Added " + juce::String::fromUTF8(name.c_str()));
}

/** Moves the playhead to the next marker after it, or the previous one
    before it. */
void MainComponent::jumpToMarker(bool forward)
{
    const auto& song   = history_.current();
    const auto  target = forward ? model::nextMarkerStart(song, playheadBeat())
                                 : model::previousMarkerStart(song, playheadBeat());
    if (! target.has_value())
    {
        showStatus(forward ? "No marker after the playhead" : "No marker before the playhead");
        return;
    }

    seekToBeat(*target);
}

/** Asks for a new name for a marker. */
void MainComponent::renameMarkerPrompt(int markerId)
{
    const auto* marker = model::findMarker(history_.current(), markerId);
    if (marker == nullptr)
        return;

    auto* window = new juce::AlertWindow("Rename Marker", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", juce::String::fromUTF8(marker->name.c_str()), "Name:");
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, markerId](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const auto name = window->getTextEditorContents("name").trim().toStdString();
            self->history_.edit("Rename marker", [markerId, name](model::Song& s)
            {
                model::renameMarker(s, markerId, name);
            });
            self->arrangementView_.setSong(self->history_.current());
        }));
}

void MainComponent::deleteMarker(int markerId)
{
    history_.edit("Delete marker", [markerId](model::Song& s) { model::removeMarker(s, markerId); });
    arrangementView_.setSong(history_.current());
}

/** Where a marker dragged along the ruler was dropped. */
void MainComponent::moveMarkerTo(int markerId, double startBeats)
{
    history_.edit("Move marker", [markerId, startBeats](model::Song& s)
    {
        model::moveMarker(s, markerId, startBeats);
    });
    arrangementView_.setSong(history_.current());
}

/** Removes every marker, as one undoable step. */
void MainComponent::deleteAllMarkers()
{
    const auto count = history_.current().markers.size();
    if (count == 0)
        return;

    history_.edit("Delete all markers", [](model::Song& s) { s.markers.clear(); });
    arrangementView_.setSong(history_.current());
    showStatus("Deleted " + juce::String((int) count) + (count == 1 ? " marker - undo to bring it back"
                                                                    : " markers - undo to bring them back"));
}

/** The right-click menu on a marker in the ruler. */
void MainComponent::showMarkerMenu(int markerId)
{
    const auto* marker = model::findMarker(history_.current(), markerId);
    if (marker == nullptr)
        return;

    const double start = marker->startBeats;

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String::fromUTF8(marker->name.c_str()));
    menu.addItem(1, "Go to Marker");
    menu.addItem(2, "Rename...");
    menu.addItem(3, "Delete");

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [self = juce::Component::SafePointer<MainComponent>(this), markerId, start](int result)
        {
            if (self == nullptr)
                return;

            switch (result)
            {
                case 1: self->seekToBeat(start); break;
                case 2: self->renameMarkerPrompt(markerId); break;
                case 3: self->deleteMarker(markerId); break;
                default: break;
            }
        });
}

/** Writes the markers to an Audacity label file. */
void MainComponent::exportMarkersDialog()
{
    if (history_.current().markers.empty())
    {
        showError("There are no markers to export");
        return;
    }

    const auto folder = projectFile_ != juce::File{}
                            ? projectFile_.getParentDirectory()
                            : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    const auto name   = (projectFile_ != juce::File{} ? projectFile_.getFileNameWithoutExtension()
                                                      : juce::String("Untitled"))
                      + " markers.txt";

    chooser_ = std::make_unique<juce::FileChooser>("Export markers", folder.getChildFile(name), "*.txt");
    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        const auto text = model::exportMarkersAsLabels(history_.current());
        if (! file.replaceWithText(juce::String::fromUTF8(text.c_str())))
        {
            showError("Could not write " + file.getFileName());
            return;
        }

        showStatus("Exported " + juce::String((int) history_.current().markers.size()) + " marker(s) to "
                   + file.getFileName());
    });
}

/** Adds markers from an Audacity label file, alongside any already here. */
void MainComponent::importMarkersDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import markers", juce::File{}, "*.txt");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        const auto markers = model::markersFromLabels(file.loadFileAsString().toStdString(),
                                                      history_.current().bpm);
        if (markers.empty())
        {
            showError("No labels found in " + file.getFileName());
            return;
        }

        history_.edit("Import markers", [&markers](model::Song& s)
        {
            for (const auto& marker : markers)
                model::addMarker(s, marker.startBeats, marker.lengthBeats, marker.name);
        });

        arrangementView_.setSong(history_.current());
        showStatus("Imported " + juce::String((int) markers.size()) + " marker(s) from " + file.getFileName());
    });
}

} // namespace soundsplice
