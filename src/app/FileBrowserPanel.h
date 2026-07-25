#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace looper
{
/**
    Left-side file-management pane: browse the filesystem (filtered to audio
    files) and drag one out onto the arrangement to import it — see
    ArrangementView's juce::DragAndDropTarget handling, which identifies a
    drag as coming from a FileTreeComponent and reads the dragged file back
    off it via getSelectedFile(). Double-click previews a file through the
    engine's existing global preview path (the same one "File > Import
    Audio..." uses).

    Docks like any other panel (see DockRegion) — it's just Component content
    handed to DockRegion::addPanel(), so it can be dragged between regions
    the same way Arrange/Edit/Mixer can.
*/
class FileBrowserPanel final : public juce::Component,
                               private juce::FileBrowserListener
{
public:
    std::function<void(const juce::File&)> onFilePreview; // double-click

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

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);
        auto row  = area.removeFromTop(24);
        homeButton_.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2, 0));
        recordingsButton_.setBounds(row.reduced(2, 0));
        area.removeFromTop(4);
        fileTree_.setBounds(area);
    }

private:
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

    juce::TextButton homeButton_       { "Home" };
    juce::TextButton recordingsButton_ { "Recordings" };
    juce::File       recordingsDirectory_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileBrowserPanel)
};

} // namespace looper
