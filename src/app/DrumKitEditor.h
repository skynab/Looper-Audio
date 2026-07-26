#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "model/DrumKit.h"

namespace looper
{
/**
    The sounds half of the Drums pane: the kit's pads as a list — mute/solo,
    name, assigned sample, and a "Load..." button each — plus Add/Remove for
    growing the kit, and a detail strip at the bottom with gain/pan/pitch for
    whichever pad is selected.

    Per-pad mix controls live in that shared strip rather than on every row
    (FL's channel rack does much the same, with per-channel settings behind a
    click) so the rows stay readable at the widths a docked side pane
    actually gets.

    A file can be dropped straight onto a row from FileBrowserPanel's tree
    (the same sourceComponent-type check ArrangementView uses to tell a file
    drag from a dock-panel drag), so replacing a sound is "drag a new file
    onto it".

    Two kinds of change come out of here, and they're deliberately separate
    callbacks because the owner treats them differently: onSampleAssigned /
    onPadAdded / onPadRemoved are structural document edits (undoable),
    while onPadMixChanged is a live tweak like a fader — see
    MainComponent::setDrumPadMix.
*/
class DrumKitEditor final : public juce::Component,
                            public juce::DragAndDropTarget
{
public:
    DrumKitEditor()
    {
        addButton_.onClick    = [this] { if (onPadAdded) onPadAdded(); };
        removeButton_.onClick = [this]
        {
            if (onPadRemoved && selectedPad_ >= 0 && selectedPad_ < (int) pads_.size())
                onPadRemoved(selectedPad_);
        };
        addAndMakeVisible(addButton_);
        addAndMakeVisible(removeButton_);

        setupMixSlider(gainSlider_, -24.0, 12.0, 0.1, " dB",
                       [this] { pushMixChange([this](model::DrumPad& p) { p.gainDb = (float) gainSlider_.getValue(); }); });
        setupMixSlider(panSlider_, -100.0, 100.0, 1.0, " pan",
                       [this] { pushMixChange([this](model::DrumPad& p) { p.pan = (float) (panSlider_.getValue() / 100.0); }); });
        setupMixSlider(pitchSlider_, -24.0, 24.0, 1.0, " st",
                       [this] { pushMixChange([this](model::DrumPad& p) { p.pitchSemitones = (float) pitchSlider_.getValue(); }); });

        setupMixLabel(gainLabel_, "Gain");
        setupMixLabel(panLabel_, "Pan");
        setupMixLabel(pitchLabel_, "Pitch");
    }

    std::function<void(int padIndex, const juce::File&)>       onSampleAssigned; // Load... or drag-drop
    std::function<void(int padIndex, const model::DrumPad&)>   onPadMixChanged;  // mute/solo/gain/pan/pitch
    std::function<void()>                                      onPadAdded;
    std::function<void(int padIndex)>                          onPadRemoved;
    std::function<void(int padIndex)>                          onPadSelected;

    /** Rebuilds the row widgets for @p pads. Called on selection changes and
        on any structural kit edit — not on a live mix tweak, which would
        otherwise yank the slider out from under the mouse mid-drag. */
    void setPads(const std::vector<model::DrumPad>& pads)
    {
        pads_ = pads;
        selectedPad_ = pads_.empty() ? -1 : juce::jlimit(0, (int) pads_.size() - 1, juce::jmax(0, selectedPad_));

        muteButtons_.clear();
        soloButtons_.clear();
        loadButtons_.clear();

        for (size_t i = 0; i < pads_.size(); ++i)
        {
            const int index = (int) i;

            auto* mute = muteButtons_.add(new juce::TextButton("M"));
            mute->setClickingTogglesState(true);
            mute->setToggleState(pads_[i].muted, juce::dontSendNotification);
            mute->setColour(juce::TextButton::buttonOnColourId, juce::Colours::orangered);
            mute->onClick = [this, index]
            {
                pushMixChangeFor(index, [this, index](model::DrumPad& p)
                                        { p.muted = muteButtons_[index]->getToggleState(); });
            };
            addAndMakeVisible(mute);

            auto* solo = soloButtons_.add(new juce::TextButton("S"));
            solo->setClickingTogglesState(true);
            solo->setToggleState(pads_[i].solo, juce::dontSendNotification);
            solo->setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
            solo->onClick = [this, index]
            {
                pushMixChangeFor(index, [this, index](model::DrumPad& p)
                                        { p.solo = soloButtons_[index]->getToggleState(); });
            };
            addAndMakeVisible(solo);

            auto* load = loadButtons_.add(new juce::TextButton("Load..."));
            load->onClick = [this, index] { promptLoadSample(index); };
            addAndMakeVisible(load);
        }

        removeButton_.setEnabled(pads_.size() > 1); // never let the kit go empty
        refreshMixControls();
        resized();
        repaint();
    }

    void setSelectedPad(int padIndex)
    {
        const int clamped = pads_.empty() ? -1 : juce::jlimit(0, (int) pads_.size() - 1, padIndex);
        if (clamped == selectedPad_)
            return;
        selectedPad_ = clamped;
        refreshMixControls();
        repaint();
    }

    int selectedPad() const noexcept { return selectedPad_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
        g.setFont(juce::FontOptions(12.0f));

        for (int i = 0; i < (int) pads_.size(); ++i)
        {
            const auto row      = rowBounds(i);
            const bool selected = (i == selectedPad_);

            g.setColour(dragHighlightRow_ == i ? juce::Colours::white.withAlpha(0.14f)
                        : selected             ? juce::Colours::white.withAlpha(0.10f)
                                               : juce::Colours::white.withAlpha(0.04f));
            g.fillRect(row);

            if (selected)
            {
                g.setColour(juce::Colours::orange.withAlpha(0.7f));
                g.drawRect(row, 1);
            }

            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawText(pads_[(size_t) i].label, row.getX() + kButtonsWidth + 4, row.getY(), 60, row.getHeight(),
                       juce::Justification::centredLeft);

            const bool hasSample  = ! pads_[(size_t) i].samplePath.empty();
            const auto sampleName = hasSample ? juce::File(pads_[(size_t) i].samplePath).getFileName()
                                              : juce::String("(no sample)");
            const int  nameX      = row.getX() + kButtonsWidth + 68;
            const int  nameWidth  = juce::jmax(10, row.getRight() - kLoadWidth - 4 - nameX);
            g.setColour(juce::Colours::white.withAlpha(hasSample ? 0.85f : 0.35f));
            g.drawFittedText(sampleName, nameX, row.getY(), nameWidth, row.getHeight(),
                             juce::Justification::centredLeft, 1);
        }

        // Divider above the selected pad's mix strip.
        if (selectedPad_ >= 0)
        {
            const int y = getHeight() - kMixStripHeight;
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillRect(0, y, getWidth(), 1);
            g.setColour(juce::Colours::white.withAlpha(0.75f));
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText(juce::String(pads_[(size_t) selectedPad_].label) + " settings",
                       6, y + 3, getWidth() - 12, 18, juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();

        auto toolbar = area.removeFromTop(kToolbarHeight);
        addButton_.setBounds(toolbar.removeFromLeft(70).reduced(3));
        removeButton_.setBounds(toolbar.removeFromLeft(80).reduced(3));

        auto mixStrip = area.removeFromBottom(kMixStripHeight);
        mixStrip.removeFromTop(22); // the "<pad> settings" caption painted above
        layoutMixRow(mixStrip, gainLabel_, gainSlider_);
        layoutMixRow(mixStrip, panLabel_, panSlider_);
        layoutMixRow(mixStrip, pitchLabel_, pitchSlider_);

        for (int i = 0; i < (int) pads_.size(); ++i)
        {
            auto row = rowBounds(i);
            muteButtons_[i]->setBounds(row.removeFromLeft(kButtonWidth).reduced(2));
            soloButtons_[i]->setBounds(row.removeFromLeft(kButtonWidth).reduced(2));
            loadButtons_[i]->setBounds(row.removeFromRight(kLoadWidth).reduced(3));
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int row = padIndexForY((float) e.position.y);
        if (row < 0)
            return;
        setSelectedPad(row);
        if (onPadSelected)
            onPadSelected(row);
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
    static constexpr int kRowHeight      = 28;
    static constexpr int kToolbarHeight  = 28;
    static constexpr int kMixStripHeight = 22 + 3 * 24;
    static constexpr int kButtonWidth    = 26;
    static constexpr int kButtonsWidth   = 2 * kButtonWidth;
    static constexpr int kLoadWidth      = 60;

    juce::Rectangle<int> rowBounds(int index) const
    {
        return { 0, kToolbarHeight + index * kRowHeight, getWidth(), kRowHeight };
    }

    int padIndexForY(float y) const
    {
        const int idx = (int) ((y - (float) kToolbarHeight) / (float) kRowHeight);
        return (y >= (float) kToolbarHeight && idx >= 0 && idx < (int) pads_.size()) ? idx : -1;
    }

    void setupMixSlider(juce::Slider& slider, double lo, double hi, double step,
                        const juce::String& suffix, std::function<void()> onChange)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = std::move(onChange);
        addAndMakeVisible(slider);
    }

    void setupMixLabel(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
        label.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(label);
    }

    void layoutMixRow(juce::Rectangle<int>& area, juce::Label& label, juce::Slider& slider)
    {
        auto row = area.removeFromTop(24);
        label.setBounds(row.removeFromLeft(42).reduced(4, 0));
        slider.setBounds(row.reduced(2, 1));
    }

    /** Mirrors the selected pad's values into the mix strip, and shows/hides
        it — there's nothing to configure when the kit has no pads. */
    void refreshMixControls()
    {
        const bool valid = selectedPad_ >= 0 && selectedPad_ < (int) pads_.size();

        juce::Component* mixControls[] = { &gainLabel_, &gainSlider_, &panLabel_, &panSlider_,
                                           &pitchLabel_, &pitchSlider_ };
        for (auto* c : mixControls)
            c->setVisible(valid);

        if (! valid)
            return;

        const auto& pad = pads_[(size_t) selectedPad_];
        gainSlider_.setValue(pad.gainDb, juce::dontSendNotification);
        panSlider_.setValue(pad.pan * 100.0, juce::dontSendNotification);
        pitchSlider_.setValue(pad.pitchSemitones, juce::dontSendNotification);
    }

    /** Applies @p mutate to the selected pad and reports it. */
    template <typename MutateFn>
    void pushMixChange(MutateFn&& mutate)
    {
        pushMixChangeFor(selectedPad_, std::forward<MutateFn>(mutate));
    }

    template <typename MutateFn>
    void pushMixChangeFor(int padIndex, MutateFn&& mutate)
    {
        if (padIndex < 0 || padIndex >= (int) pads_.size())
            return;

        auto& pad = pads_[(size_t) padIndex];
        mutate(pad);
        if (onPadMixChanged)
            onPadMixChanged(padIndex, pad);
        repaint(); // mute state is part of the row's appearance
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
    juce::OwnedArray<juce::TextButton> muteButtons_, soloButtons_, loadButtons_;
    juce::TextButton                   addButton_    { "Add Pad" };
    juce::TextButton                   removeButton_ { "Remove Pad" };
    juce::Label                        gainLabel_, panLabel_, pitchLabel_;
    juce::Slider                       gainSlider_, panSlider_, pitchSlider_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int                                dragHighlightRow_ = -1;
    int                                selectedPad_      = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumKitEditor)
};

} // namespace looper
