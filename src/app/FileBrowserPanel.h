#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "FileGrid.h"
#include "Icons.h"

namespace looper
{
/**
    A small TextButton that also reports right-clicks (used for "remove this
    bookmark" — left-click still navigates there via the normal onClick).
*/
class BookmarkButton final : public juce::TextButton
{
public:
    std::function<void()> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick)
                onRightClick();
            return;
        }
        TextButton::mouseDown(e);
    }
};

/**
    Left-side file-management pane: a folder tree (top) for navigation and
    drag-into-arrangement (unchanged from before — see ArrangementView's
    juce::DragAndDropTarget handling, which identifies a drag as coming from
    a FileTreeComponent and reads the dragged file back off it via
    getSelectedFile()), plus a sortable, color-coded detail grid (bottom,
    FileGrid) showing whichever folder was last selected — Name/Type/Size/
    Modified/Duration, the "grid of information about the files" ask.
    Double-click either one to preview through the engine's existing global
    preview path (the same one "File > Import Audio..." uses).

    "Places" has an optional Project Root button (see setProjectRootFolder,
    shown only once a project root is set), two always-present buttons (Home,
    Recordings), and a user-editable bookmark list: the "+" button adds a
    folder (via a picker), right-click on a bookmark removes it. Bookmarks
    are just held here — the owner (MainComponent) persists them via
    setBookmarks()/bookmarks() and onBookmarksChanged, the same app-settings
    file it uses for the dockable workspace layout.

    Folder management (right-click the tree or a grid row): New Folder,
    Rename, Delete — Delete asks for confirmation first (see
    confirmAndDelete) since it's a real, irreversible filesystem operation.

    Docks like any other panel (see DockRegion) — it's just Component content
    handed to DockRegion::addPanel(), so it can be dragged between regions
    the same way Arrange/Edit/Mixer can.
*/
class FileBrowserPanel final : public juce::Component,
                               private juce::FileBrowserListener
{
public:
    std::function<void(const juce::File&)> onFilePreview;      // double-click
    std::function<void()>                  onBookmarksChanged; // added or removed one
    std::function<void()>                  onFavoritesChanged; // starred or unstarred a file in the grid

    FileBrowserPanel()
        : audioFilter_("*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3;*.m4a;*.mp4", "*", "Audio files"),
          directoryList_(&audioFilter_, fileThread_),
          fileTree_(directoryList_)
    {
        fileThread_.startThread();
        recordingsDirectory_ = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        directoryList_.setDirectory(recordingsDirectory_, true, true);

        // A plain marker distinguishing "dragging a file" from a DockRegion
        // tab-header drag (which uses the panel's own name as its
        // description) — ArrangementView/DockRegion tell drags apart by
        // sourceComponent type, not this string, but JUCE requires some
        // non-empty description to start a drag at all.
        fileTree_.setDragAndDropDescription("audiofile");
        fileTree_.addListener(this);
        // Right-click is otherwise the only way to discover New Folder,
        // Rename, and Delete exist at all.
        fileTree_.setTooltip("Right-click for New Folder, Rename, Delete");
        addAndMakeVisible(fileTree_);

        fileGrid_.onFilePreview      = [this](const juce::File& file) { if (onFilePreview) onFilePreview(file); };
        fileGrid_.onRightClick       = [this](const juce::File& file) { showContextMenuFor(file); };
        fileGrid_.onFavoritesChanged = [this] { if (onFavoritesChanged) onFavoritesChanged(); };
        addAndMakeVisible(fileGrid_);
        fileGrid_.setDirectory(recordingsDirectory_);

        projectRootButton_.onClick = [this] { showDirectory(projectRootFolder_); };
        addAndMakeVisible(projectRootButton_);
        projectRootButton_.setVisible(false); // addAndMakeVisible forces true; no root is set yet

        homeButton_.onClick = [this]
        {
            showDirectory(juce::File::getSpecialLocation(juce::File::userHomeDirectory));
        };
        recordingsButton_.onClick = [this] { showDirectory(recordingsDirectory_); };
        addAndMakeVisible(homeButton_);
        addAndMakeVisible(recordingsButton_);

        addBookmarkButton_.setTooltip("Bookmark the folder currently being browsed");
        addBookmarkButton_.onClick = [this] { promptAddBookmark(); };
        addAndMakeVisible(addBookmarkButton_);

        {
            auto sidebarOn  = icons::fromSvg(icons::kSidebarOn);
            auto sidebarOff = icons::fromSvg(icons::kSidebarOff);
            toggleTreeButton_.setImages(sidebarOff.get(), nullptr, nullptr, nullptr, sidebarOn.get());
        }
        toggleTreeButton_.setClickingTogglesState(true);
        toggleTreeButton_.setToggleState(true, juce::dontSendNotification); // tree starts visible
        toggleTreeButton_.setTooltip("Show/hide the folder tree");
        toggleTreeButton_.onClick = [this]
        {
            treeVisible_ = toggleTreeButton_.getToggleState();
            fileTree_.setVisible(treeVisible_);
            resized();
        };
        addAndMakeVisible(toggleTreeButton_);

        {
            auto folderAddOn  = icons::fromSvg(icons::kFolderAddOn);
            auto folderAddOff = icons::fromSvg(icons::kFolderAddOff);
            newFolderButton_.setImages(folderAddOff.get(), folderAddOn.get());
        }
        newFolderButton_.setTooltip("New folder in the current directory");
        newFolderButton_.onClick = [this] { promptNewFolder(directoryList_.getDirectory()); };
        addAndMakeVisible(newFolderButton_);
    }

    ~FileBrowserPanel() override
    {
        fileTree_.removeListener(this);
        fileThread_.stopThread(2000);
    }

    // "Places" bookmark target for the Recordings button — defaults to the
    // user's home directory until the owner points it at the actual
    // recordings folder (MainComponent::recordingsDirectory()).
    void setRecordingsDirectory(const juce::File& dir) { recordingsDirectory_ = dir; }

    void showDirectory(const juce::File& dir)
    {
        directoryList_.setDirectory(dir, true, true);
        fileGrid_.setDirectory(dir);
    }

    // Project-specific "Places" entry (see model::Song::projectRootFolder) —
    // hidden until the project actually has one set.
    void setProjectRootFolder(const juce::File& dir)
    {
        projectRootFolder_ = dir;
        const bool has = dir != juce::File{} && dir.isDirectory();
        projectRootButton_.setVisible(has);
        if (has)
            projectRootButton_.setButtonText(dir.getFileName());
        resized();
    }

    // User-editable bookmarks (see class doc) — the owner persists these,
    // this class only holds and displays them.
    void setBookmarks(const std::vector<juce::File>& bookmarks)
    {
        bookmarks_ = bookmarks;
        rebuildBookmarkButtons();
    }
    const std::vector<juce::File>& bookmarks() const { return bookmarks_; }

    // Starred files (see FileGrid) — the owner persists these too.
    void setFavorites(const std::vector<juce::File>& favorites) { fileGrid_.setFavorites(favorites); }
    std::vector<juce::File> favorites() const { return fileGrid_.favorites(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);

        auto placesRow = area.removeFromTop(24);
        addBookmarkButton_.setBounds(placesRow.removeFromRight(24));
        placesRow.removeFromRight(4);
        toggleTreeButton_.setBounds(placesRow.removeFromLeft(24));
        placesRow.removeFromLeft(4);
        newFolderButton_.setBounds(placesRow.removeFromLeft(24));
        placesRow.removeFromLeft(4);
        if (projectRootButton_.isVisible())
        {
            projectRootButton_.setBounds(placesRow.removeFromLeft(placesRow.getWidth() / 3).reduced(2, 0));
            homeButton_.setBounds(placesRow.removeFromLeft(placesRow.getWidth() / 2).reduced(2, 0));
        }
        else
        {
            homeButton_.setBounds(placesRow.removeFromLeft(placesRow.getWidth() / 2).reduced(2, 0));
        }
        recordingsButton_.setBounds(placesRow.reduced(2, 0));

        for (auto* button : bookmarkButtons_)
        {
            area.removeFromTop(2);
            button->setBounds(area.removeFromTop(20));
        }
        area.removeFromTop(4);

        // Tree (top ~55%) and detail grid (bottom ~45%), a fixed split rather
        // than a draggable one — keeps this addition contained; see
        // docs/PLAN.md for the cut. Hiding the tree (toggleTreeButton_) gives
        // the grid the full remaining height instead.
        if (treeVisible_)
        {
            const int treeHeight = (int) (area.getHeight() * 0.55f);
            fileTree_.setBounds(area.removeFromTop(treeHeight));
            area.removeFromTop(4);
        }
        fileGrid_.setBounds(area);
    }

private:
    void promptAddBookmark()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Add a bookmark folder", directoryList_.getDirectory());
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

        chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
        {
            const auto dir = fc.getResult();
            if (dir == juce::File{} || ! dir.isDirectory())
                return;
            if (std::find(bookmarks_.begin(), bookmarks_.end(), dir) != bookmarks_.end())
                return; // already bookmarked

            bookmarks_.push_back(dir);
            rebuildBookmarkButtons();
            if (onBookmarksChanged)
                onBookmarksChanged();
        });
    }

    void removeBookmark(const juce::File& dir)
    {
        bookmarks_.erase(std::remove(bookmarks_.begin(), bookmarks_.end(), dir), bookmarks_.end());
        rebuildBookmarkButtons();
        if (onBookmarksChanged)
            onBookmarksChanged();
    }

    void rebuildBookmarkButtons()
    {
        bookmarkButtons_.clear();
        for (const auto& dir : bookmarks_)
        {
            auto* button = bookmarkButtons_.add(new BookmarkButton());
            button->setButtonText(dir.getFileName());
            button->setTooltip(dir.getFullPathName() + " (right-click to remove)");
            button->onClick      = [this, dir] { showDirectory(dir); };
            button->onRightClick = [this, dir] { removeBookmark(dir); };
            addAndMakeVisible(button);
        }
        resized();
    }

    // Re-scans whatever the tree/grid currently show — call after any
    // filesystem change made through the context menu below.
    void refreshCurrentView()
    {
        directoryList_.refresh();
        fileGrid_.refresh();
    }

    void showContextMenuFor(const juce::File& file)
    {
        juce::PopupMenu menu;
        menu.addItem(1, "New Folder...");
        menu.addItem(2, "Rename...");
        menu.addItem(3, "Delete");

        menu.showMenuAsync(juce::PopupMenu::Options(), [this, file](int result)
        {
            const auto parentDir = file.isDirectory() ? file : file.getParentDirectory();
            if (result == 1)      promptNewFolder(parentDir);
            else if (result == 2) promptRename(file);
            else if (result == 3) confirmAndDelete(file);
        });
    }

    // New Folder and Rename both need to read the text editor's contents
    // *after* the dialog closes, so they own the AlertWindow themselves
    // (deleteWhenDismissed = false) rather than letting JUCE auto-delete it
    // before the callback runs — auto-delete happens before the callback,
    // which would otherwise leave the read dangling.
    void promptNewFolder(const juce::File& parentDir)
    {
        textPromptDialog_ = std::make_unique<juce::AlertWindow>("New Folder", "Folder name:",
                                                                juce::MessageBoxIconType::NoIcon);
        textPromptDialog_->addTextEditor("name", "New Folder");
        textPromptDialog_->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
        textPromptDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        textPromptDialog_->enterModalState(true, juce::ModalCallbackFunction::create([this, parentDir](int result)
        {
            if (result == 1 && textPromptDialog_ != nullptr)
            {
                const auto name = textPromptDialog_->getTextEditorContents("name").trim();
                if (name.isNotEmpty())
                    parentDir.getChildFile(name).createDirectory();
            }
            textPromptDialog_.reset();
            refreshCurrentView();
        }), false);
    }

    void promptRename(const juce::File& file)
    {
        textPromptDialog_ = std::make_unique<juce::AlertWindow>("Rename", "New name:",
                                                                juce::MessageBoxIconType::NoIcon);
        textPromptDialog_->addTextEditor("name", file.getFileName());
        textPromptDialog_->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        textPromptDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        textPromptDialog_->enterModalState(true, juce::ModalCallbackFunction::create([this, file](int result)
        {
            if (result == 1 && textPromptDialog_ != nullptr)
            {
                const auto newName = textPromptDialog_->getTextEditorContents("name").trim();
                if (newName.isNotEmpty() && newName != file.getFileName())
                    file.moveFileTo(file.getSiblingFile(newName));
            }
            textPromptDialog_.reset();
            refreshCurrentView();
        }), false);
    }

    void confirmAndDelete(const juce::File& file)
    {
        const auto opts = juce::MessageBoxOptions()
                              .withIconType(juce::MessageBoxIconType::WarningIcon)
                              .withTitle(file.isDirectory() ? "Delete Folder" : "Delete File")
                              .withMessage("Permanently delete \"" + file.getFileName()
                                          + "\"? This cannot be undone.")
                              .withButton("Delete")
                              .withButton("Cancel");

        juce::AlertWindow::showAsync(opts, [this, file](int result)
        {
            if (result == 0) // "Delete", the first button added
            {
                file.deleteRecursively();
                refreshCurrentView();
            }
        });
    }

    // juce::FileBrowserListener
    void selectionChanged() override {}
    void fileClicked(const juce::File& file, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showContextMenuFor(file);
            return;
        }
        if (file.isDirectory())
            fileGrid_.setDirectory(file);
    }
    void fileDoubleClicked(const juce::File& file) override
    {
        if (onFilePreview)
            onFilePreview(file);
    }
    void browserRootChanged(const juce::File&) override {}

    juce::WildcardFileFilter    audioFilter_;
    juce::TimeSliceThread       fileThread_ { "LooperFileBrowser" };
    juce::DirectoryContentsList directoryList_;
    juce::FileTreeComponent     fileTree_;
    FileGrid                    fileGrid_;

    juce::TextButton homeButton_        { "Home" };
    juce::TextButton recordingsButton_  { "Recordings" };
    juce::TextButton projectRootButton_ { "Project" };
    juce::TextButton addBookmarkButton_ { "+" };
    juce::DrawableButton toggleTreeButton_ { "ToggleTree", juce::DrawableButton::ImageFitted };
    juce::DrawableButton newFolderButton_  { "NewFolder", juce::DrawableButton::ImageFitted };
    bool             treeVisible_ = true;
    juce::File       recordingsDirectory_;
    juce::File       projectRootFolder_;

    std::vector<juce::File>            bookmarks_;
    juce::OwnedArray<BookmarkButton>    bookmarkButtons_;
    std::unique_ptr<juce::FileChooser>  chooser_;
    std::unique_ptr<juce::AlertWindow>  textPromptDialog_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileBrowserPanel)
};

} // namespace looper
