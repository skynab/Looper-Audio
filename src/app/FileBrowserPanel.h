#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

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
    Left-side file-management pane: browse the filesystem (filtered to audio
    files) and drag one out onto the arrangement to import it — see
    ArrangementView's juce::DragAndDropTarget handling, which identifies a
    drag as coming from a FileTreeComponent and reads the dragged file back
    off it via getSelectedFile(). Double-click previews a file through the
    engine's existing global preview path (the same one "File > Import
    Audio..." uses).

    "Places" has two always-present buttons (Home, Recordings) plus a
    user-editable bookmark list: the "+" button adds a folder (via a picker),
    right-click on a bookmark removes it. Bookmarks are just held here — the
    owner (MainComponent) persists them via setBookmarks()/bookmarks() and
    onBookmarksChanged, the same app-settings file it uses for the dockable
    workspace layout.

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
        addAndMakeVisible(fileTree_);

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
    void showDirectory(const juce::File& dir) { directoryList_.setDirectory(dir, true, true); }

    // User-editable bookmarks (see class doc) — the owner persists these,
    // this class only holds and displays them.
    void setBookmarks(const std::vector<juce::File>& bookmarks)
    {
        bookmarks_ = bookmarks;
        rebuildBookmarkButtons();
    }
    const std::vector<juce::File>& bookmarks() const { return bookmarks_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);
        auto row  = area.removeFromTop(24);
        addBookmarkButton_.setBounds(row.removeFromRight(24));
        row.removeFromRight(4);
        homeButton_.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 0));
        recordingsButton_.setBounds(row.reduced(2, 0));

        for (auto* button : bookmarkButtons_)
        {
            area.removeFromTop(2);
            button->setBounds(area.removeFromTop(20));
        }

        area.removeFromTop(4);
        fileTree_.setBounds(area);
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

    // juce::FileBrowserListener
    void selectionChanged() override {}
    void fileClicked(const juce::File&, const juce::MouseEvent&) override {}
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

    juce::TextButton homeButton_        { "Home" };
    juce::TextButton recordingsButton_  { "Recordings" };
    juce::TextButton addBookmarkButton_ { "+" };
    juce::File       recordingsDirectory_;

    std::vector<juce::File>            bookmarks_;
    juce::OwnedArray<BookmarkButton>    bookmarkButtons_;
    std::unique_ptr<juce::FileChooser>  chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileBrowserPanel)
};

} // namespace looper
