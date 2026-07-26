#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/Pattern.h"
#include "model/DrumKit.h"

#include "DrumKitEditor.h"
#include "DrumStepGrid.h"

namespace looper
{
/**
    The Drums pane: the kit's sounds on the left (DrumKitEditor — load/replace
    samples, add pads, per-pad mute/solo/gain/pan/pitch) and the rhythm on the
    right (DrumStepGrid — one row per pad, click to toggle steps), with a
    draggable divider between them.

    It owns no state of its own beyond what's needed to keep the two halves
    agreeing on the selected pad; everything else is forwarded straight
    through to whichever half needs it, and edits are reported to the owner
    via the callbacks below (which are simply the two children's callbacks
    re-exposed, so MainComponent wires the document/engine in one place).

    Shows a placeholder instead of both halves when the selected track isn't
    a Drum track — the same is-it-this-track-type gating SynthEditor uses for
    Instrument tracks.
*/
class DrumsPane final : public juce::Component
{
public:
    DrumsPane()
    {
        placeholderLabel_.setText("Select a Drum track to edit its kit", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        addChildComponent(kitEditor_);
        addChildComponent(stepGrid_);
        addChildComponent(divider_);

        // Selecting a pad on either side selects it on the other, so the
        // mix strip always describes the row you last touched in the grid.
        kitEditor_.onPadSelected = [this](int padIndex) { stepGrid_.setSelectedPad(padIndex); };
        stepGrid_.onPadSelected  = [this](int padIndex)
        {
            kitEditor_.setSelectedPad(padIndex);
            stepGrid_.setSelectedPad(padIndex);
        };

        layout_.setItemLayout(0, 220, 520, 320); // kit editor: min/max/preferred
        layout_.setItemLayout(1, 8, 8, 8);       // divider
        layout_.setItemLayout(2, 200, -1.0, -1.0); // step grid: takes the rest
    }

    // Straight pass-throughs to the two halves — see DrumKitEditor/DrumStepGrid.
    std::function<void(int padIndex, const juce::File&)>     onSampleAssigned;
    std::function<void(int padIndex, const model::DrumPad&)> onPadMixChanged;
    std::function<void()>                                    onPadAdded;
    std::function<void(int padIndex)>                        onPadRemoved;
    std::function<void(const engine::Pattern&)>              onPatternChanged;
    std::function<void(int noteNumber)>                      onNotePreview;

    /** Shows the kit and its pattern. Both halves get the same pad list, so
        the grid's rows and the editor's rows always describe the same kit. */
    void setKit(const std::vector<model::DrumPad>& pads, const engine::Pattern& pattern)
    {
        kitEditor_.setPads(pads);
        stepGrid_.setPads(pads);
        stepGrid_.setPattern(pattern);
        stepGrid_.setSelectedPad(kitEditor_.selectedPad());
        setContentVisible(true);
    }

    /** Refreshes only what a live mix tweak affects — the grid's dimming of
        muted pads — without rebuilding the kit editor's row widgets underneath
        the mouse mid-drag (see DrumKitEditor::setPads). */
    void refreshPadsForMixChange(const std::vector<model::DrumPad>& pads)
    {
        stepGrid_.setPads(pads);
    }

    void setPattern(const engine::Pattern& pattern) { stepGrid_.setPattern(pattern); }

    void setPlayheadBeats(double patternLocalBeats, bool visible)
    {
        if (contentVisible_)
            stepGrid_.setPlayheadBeats(patternLocalBeats, visible);
    }

    /** Shows the placeholder instead of the kit — the selected track isn't a
        Drum track (or none is selected). */
    void setNoDrumTrackSelected() { setContentVisible(false); }

    /** Wires the two halves' callbacks through to this pane's own. Called once
        by the owner after construction. */
    void connectCallbacks()
    {
        kitEditor_.onSampleAssigned = [this](int padIndex, const juce::File& file)
        {
            if (onSampleAssigned) onSampleAssigned(padIndex, file);
        };
        kitEditor_.onPadMixChanged = [this](int padIndex, const model::DrumPad& pad)
        {
            if (onPadMixChanged) onPadMixChanged(padIndex, pad);
        };
        kitEditor_.onPadAdded   = [this] { if (onPadAdded) onPadAdded(); };
        kitEditor_.onPadRemoved = [this](int padIndex) { if (onPadRemoved) onPadRemoved(padIndex); };

        stepGrid_.onChange = [this](const engine::Pattern& pattern)
        {
            if (onPatternChanged) onPatternChanged(pattern);
        };
        stepGrid_.onNotePreview = [this](int noteNumber)
        {
            if (onNotePreview) onNotePreview(noteNumber);
        };
    }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds();
        juce::Component* items[] = { &kitEditor_, &divider_, &stepGrid_ };
        layout_.layOutComponents(items, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                 false, // side by side
                                 true); // full height
    }

private:
    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholderLabel_.setVisible(! visible);
        kitEditor_.setVisible(visible);
        stepGrid_.setVisible(visible);
        divider_.setVisible(visible);
        resized();
    }

    DrumKitEditor kitEditor_;
    DrumStepGrid  stepGrid_;
    bool          contentVisible_ = false;

    juce::Label                       placeholderLabel_;
    juce::StretchableLayoutManager    layout_;
    juce::StretchableLayoutResizerBar divider_ { &layout_, 1, true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumsPane)
};

} // namespace looper
