#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "model/DrumKit.h"

namespace looper
{
/**
    One row per drum pad: its label, its assigned sample's name (or
    "(no sample)"), and a "Load..." button — plus accepting a file dragged
    straight from FileBrowserPanel's tree (same sourceComponent-type check
    ArrangementView already uses to tell a file drag apart from a dock-panel
    drag), so replacing a pad's sound is just "drag a new file onto its
    row". Shown alongside the piano roll only when the selected track is a
    Drum track (see MainComponent::refreshPianoRollForSelected).
*/
class DrumKitEditor final : public juce::Component,
                            public juce::DragAndDropTarget
{
public:
    DrumKitEditor() = default;

    std::function<void(int padIndex, const juce::File&)> onSampleAssigned; // Load... or drag-drop

    void setPads(const std::vector<model::DrumPad>& pads)
    {
        pads_ = pads;
        loadButtons_.clear();
        for (size_t i = 0; i < pads_.size(); ++i)
        {
            auto* button = loadButtons_.add(new juce::TextButton("Load..."));
            const int index = (int) i;
            button->onClick = [this, index] { promptLoadSample(index); };
            addAndMakeVisible(button);
        }
        resized();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
        g.setFont(juce::FontOptions(12.0f));

        for (int i = 0; i < (int) pads_.size(); ++i)
        {
            const auto row = rowBounds(i);
            g.setColour(dragHighlightRow_ == i ? juce::Colours::white.withAlpha(0.14f)
                                              : juce::Colours::white.withAlpha(0.04f));
            g.fillRect(row);

            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawText(pads_[(size_t) i].label, row.getX() + 6, row.getY(), 64, row.getHeight(),
                       juce::Justification::centredLeft);

            const bool hasSample  = ! pads_[(size_t) i].samplePath.empty();
            const auto sampleName = hasSample ? juce::File(pads_[(size_t) i].samplePath).getFileName()
                                              : juce::String("(no sample)");
            g.setColour(juce::Colours::white.withAlpha(hasSample ? 0.85f : 0.35f));
            g.drawFittedText(sampleName, row.getX() + 72, row.getY(), row.getWidth() - 72 - 72,
                             row.getHeight(), juce::Justification::centredLeft, 1);
        }
    }

    void resized() override
    {
        for (int i = 0; i < loadButtons_.size(); ++i)
            loadButtons_[i]->setBounds(rowBounds(i).removeFromRight(64).reduced(3));
    }

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr;
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        dragHighlightRow_ = padIndexForY((float) details.localPosition.y);
        repaint();
    }

    void itemDragMove(const SourceDetails& details) override
    {
        const int row = padIndexForY((float) details.localPosition.y);
        if (row != dragHighlightRow_)
        {
            dragHighlightRow_ = row;
            repaint();
        }
    }

    void itemDragExit(const SourceDetails&) override
    {
        dragHighlightRow_ = -1;
        repaint();
    }

    void itemDropped(const SourceDetails& details) override
    {
        const int padIndex = dragHighlightRow_;
        dragHighlightRow_   = -1;
        repaint();

        auto* fileTree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get());
        if (fileTree == nullptr || fileTree->getNumSelectedFiles() == 0 || padIndex < 0)
            return;

        const auto file = fileTree->getSelectedFile(0);
        if (file != juce::File{} && onSampleAssigned)
            onSampleAssigned(padIndex, file);
    }

private:
    static constexpr int kRowHeight = 26;

    juce::Rectangle<int> rowBounds(int index) const { return { 0, index * kRowHeight, getWidth(), kRowHeight }; }

    int padIndexForY(float y) const
    {
        const int idx = (int) (y / (float) kRowHeight);
        return (idx >= 0 && idx < (int) pads_.size()) ? idx : -1;
    }

    void promptLoadSample(int index)
    {
        chooser_ = std::make_unique<juce::FileChooser>("Load a drum sample", juce::File{},
                                                        "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3;*.m4a");
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

        chooser_->launchAsync(flags, [this, index](const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file != juce::File{} && onSampleAssigned)
                onSampleAssigned(index, file);
        });
    }

    std::vector<model::DrumPad>        pads_;
    juce::OwnedArray<juce::TextButton> loadButtons_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int                                 dragHighlightRow_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumKitEditor)
};

} // namespace looper
