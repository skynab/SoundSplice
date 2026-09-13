#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "FileTypeColors.h"
#include "AudioFileTypes.h"
#include "Icons.h"

namespace looper
{
/**
    A sortable, color-coded detail view of one folder's files — Name / Type /
    Size / Modified / Duration — the "grid of information" companion to the
    folder tree in FileBrowserPanel. Shows files only; the tree handles
    folder navigation. Duration is probed lazily (a file header read, not a
    full decode — the same lightweight technique
    AudioEngine::probeDurationSeconds already uses) and only for files
    classified as audio, and cached per path so re-sorting never re-probes.

    A drag source: rows can be dragged onto the timeline or the audio editor.
    It was left out originally on the grounds that the directory tree above
    already covered dragging — but the tree is the *navigator* and the grid is
    where the files actually are, so in practice dragging a file appeared not
    to work at all. The description carries the path (see
    audiofiles::dragDescriptionFor), so a target identifies the drag without
    having to know what kind of component it came from.
*/
class FileGrid final : public juce::Component,
                       private juce::TableListBoxModel
{
public:
    std::function<void(const juce::File&)> onFilePreview;      // double-click
    std::function<void(const juce::File&)> onRightClick;       // right-click a row
    std::function<void()>                  onFavoritesChanged; // starred or unstarred one

    FileGrid()
        : starOn_(icons::fromSvg(icons::kStarOn)), starOff_(icons::fromSvg(icons::kStarOutlineOff))
    {
        formatManager_.registerBasicFormats();

        auto& header = table_.getHeader();
        header.addColumn({}, kFavColumnId, 26, 26, 26, juce::TableHeaderComponent::notResizableOrSortable);
        header.addColumn("Name", 1, 170, 60, -1);
        header.addColumn("Type", 2, 64, 50, 100);
        header.addColumn("Size", 3, 72, 50, 120);
        header.addColumn("Modified", 4, 130, 90, 200);
        header.addColumn("Duration", 5, 70, 50, 100);
        header.setSortColumnId(1, true);

        table_.setModel(this);
        table_.setMultipleSelectionEnabled(false);
        // Right-click is otherwise the only way to discover New Folder,
        // Rename, and Delete exist at all — nothing else in this row hints
        // at it.
        table_.setTooltip("Double-click to preview - right-click for New Folder, Rename, Delete");
        addAndMakeVisible(table_);
    }

    // User-editable favorites (see class doc) — the owner persists these,
    // this class only holds, displays, and toggles them.
    void setFavorites(const std::vector<juce::File>& favorites)
    {
        favorites_.clear();
        for (const auto& f : favorites)
            favorites_.insert(f.getFullPathName());
        table_.repaint();
    }

    std::vector<juce::File> favorites() const
    {
        std::vector<juce::File> result;
        for (const auto& path : favorites_)
            result.push_back(juce::File(path));
        return result;
    }

    /** Shows @p dir's files (not its subfolders — the tree handles those). */
    void setDirectory(const juce::File& dir)
    {
        directory_ = dir;
        refresh();
    }

    void refresh()
    {
        entries_.clear();
        if (directory_.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator(directory_, false, kWildcard, juce::File::findFiles))
                entries_.push_back(entry.getFile());
        }
        sortEntries();
        table_.updateContent();
        table_.repaint();
    }

    void resized() override { table_.setBounds(getLocalBounds()); }

private:
    static constexpr int kFavColumnId = 6;
    static constexpr const char* kWildcard =
        "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3;*.m4a;*.mp4;*.mid;*.midi;*.looper";

    bool isFavorite(const juce::File& file) const { return favorites_.count(file.getFullPathName()) > 0; }

    void toggleFavorite(const juce::File& file)
    {
        const auto path = file.getFullPathName();
        if (! favorites_.erase(path))
            favorites_.insert(path);
        table_.repaint();
        if (onFavoritesChanged)
            onFavoritesChanged();
    }

    void sortEntries()
    {
        const int  sortColumn = table_.getHeader().getSortColumnId();
        const bool forwards   = table_.getHeader().isSortedForwards();

        std::stable_sort(entries_.begin(), entries_.end(), [&](const juce::File& a, const juce::File& b)
        {
            bool less = false;
            switch (sortColumn)
            {
                case 2:  less = labelForFileKind(classifyFile(a)) < labelForFileKind(classifyFile(b)); break;
                case 3:  less = a.getSize() < b.getSize(); break;
                case 4:  less = a.getLastModificationTime() < b.getLastModificationTime(); break;
                case 5:  less = durationSecondsFor(a) < durationSecondsFor(b); break;
                default: less = a.getFileName().compareIgnoreCase(b.getFileName()) < 0; break;
            }
            return forwards ? less : ! less;
        });
    }

    double durationSecondsFor(const juce::File& file)
    {
        if (classifyFile(file) != FileKind::Audio)
            return 0.0;

        const auto path = file.getFullPathName();
        auto       it    = durationCache_.find(path);
        if (it != durationCache_.end())
            return it->second;

        double seconds = 0.0;
        if (std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file)); reader != nullptr
            && reader->sampleRate > 0.0)
            seconds = (double) reader->lengthInSamples / reader->sampleRate;

        durationCache_[path] = seconds;
        return seconds;
    }

    juce::String durationTextFor(const juce::File& file)
    {
        if (classifyFile(file) != FileKind::Audio)
            return {};
        const double seconds = durationSecondsFor(file);
        if (seconds <= 0.0)
            return {};
        return juce::String::formatted("%d:%02d", (int) seconds / 60, (int) seconds % 60);
    }

    // juce::TableListBoxModel
    int getNumRows() override { return (int) entries_.size(); }

    /** Returning a description is what makes TableListBox start a drag at
        all — without it a press-and-move on a row is just a click. */
    juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override
    {
        if (selectedRows.isEmpty())
            return {};

        const int row = selectedRows[0];
        if (row < 0 || row >= (int) entries_.size())
            return {};

        const auto& file = entries_[(size_t) row];

        // Directories aren't draggable: dropping one on a track has no
        // meaning, and offering the gesture would just fail silently.
        if (file.isDirectory())
            return {};

        return audiofiles::dragDescriptionFor(file);
    }

    void paintRowBackground(juce::Graphics& g, int rowNumber, int, int, bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int) entries_.size())
            return;
        if (rowIsSelected)
        {
            g.fillAll(juce::Colours::white.withAlpha(0.16f));
            return;
        }
        const auto kind = classifyFile(entries_[(size_t) rowNumber]);
        g.fillAll(colourForFileKind(kind).withAlpha((rowNumber % 2 == 1) ? 0.10f : 0.05f));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override
    {
        if (rowNumber < 0 || rowNumber >= (int) entries_.size())
            return;
        const auto& file = entries_[(size_t) rowNumber];
        const auto  kind = classifyFile(file);

        if (columnId == kFavColumnId)
        {
            auto* star = isFavorite(file) ? starOn_.get() : starOff_.get();
            if (star != nullptr)
                star->drawWithin(g, { 2.0f, 2.0f, (float) width - 4.0f, (float) height - 4.0f },
                                 juce::RectanglePlacement::centred, 1.0f);
            return;
        }

        juce::String text;
        switch (columnId)
        {
            case 1: text = file.getFileName(); break;
            case 2: text = labelForFileKind(kind); break;
            case 3: text = file.getSize() > 0 ? juce::File::descriptionOfSizeInBytes(file.getSize()) : juce::String(); break;
            case 4: text = file.getLastModificationTime().toString(true, true, false, true); break;
            case 5: text = durationTextFor(file); break;
            default: break;
        }

        g.setColour(columnId == 2 ? colourForFileKind(kind) : juce::Colours::white.withAlpha(0.85f));
        g.drawFittedText(text, 4, 0, width - 8, height, juce::Justification::centredLeft, 1);
    }

    void cellDoubleClicked(int rowNumber, int, const juce::MouseEvent&) override
    {
        if (rowNumber >= 0 && rowNumber < (int) entries_.size() && onFilePreview)
            onFilePreview(entries_[(size_t) rowNumber]);
    }

    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& e) override
    {
        if (rowNumber < 0 || rowNumber >= (int) entries_.size())
            return;

        if (e.mods.isPopupMenu())
        {
            if (onRightClick)
                onRightClick(entries_[(size_t) rowNumber]);
            return;
        }

        if (columnId == kFavColumnId)
            toggleFavorite(entries_[(size_t) rowNumber]);
    }

    void sortOrderChanged(int, bool) override
    {
        sortEntries();
        table_.updateContent();
        table_.repaint();
    }

    juce::TableListBox           table_;
    juce::AudioFormatManager     formatManager_;
    juce::File                   directory_;
    std::vector<juce::File>      entries_;
    std::map<juce::String, double> durationCache_;
    std::set<juce::String>       favorites_; // full paths of starred files
    std::unique_ptr<juce::Drawable> starOn_, starOff_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileGrid)
};

} // namespace looper
