#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DockRegion.h"

namespace looper
{
/**
    The center workspace region: starts as a single DockRegion (primary_)
    and can optionally be split into two — side by side (splitLeftRight) or
    stacked (splitTopBottom) — revealing a second region (secondary_) that
    participates in the same docking system as any other region (it's just
    another DockRegion; the owner wires onForeignPanelDropped/persistence
    for it exactly like Left/Bottom/Right). unsplit() moves any panels still
    in secondary_ back into primary_ before hiding it again, so nothing gets
    orphaned off-screen.
*/
class CenterSplitArea final : public juce::Component
{
public:
    DockRegion primary_, secondary_;

    CenterSplitArea()
    {
        addAndMakeVisible(primary_);
        addChildComponent(secondary_); // present but hidden until split
    }

    void splitLeftRight() { split(false); }
    void splitTopBottom() { split(true); }

    bool isSplit() const noexcept { return isSplit_; }
    bool isStackedVertically() const noexcept { return stackedVertically_; }

    /** Moves any panels still in secondary_ back into primary_, then hides
        secondary_ and gives primary_ the whole area again. */
    void unsplit()
    {
        if (! isSplit_)
            return;

        for (const auto& name : secondary_.panelNames())
        {
            auto* content = secondary_.contentFor(name);
            if (content == nullptr)
                continue;
            secondary_.removePanel(name);
            primary_.addPanel(name, *content);
        }

        isSplit_ = false;
        secondary_.setVisible(false);
        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        if (! isSplit_)
        {
            primary_.setBounds(area);
            return;
        }

        splitLayout_.setItemLayout(0, 100, -1.0, -1.0);
        splitLayout_.setItemLayout(1, 8, 8, 8);
        splitLayout_.setItemLayout(2, 100, -1.0, -1.0);

        // Only one resizer bar is ever active — its orientation must match
        // the split (StretchableLayoutResizerBar reads drag delta along a
        // fixed axis, so a single bar can't serve both orientations).
        auto& activeResizer   = stackedVertically_ ? stackedResizer_ : sideBySideResizer_;
        auto& inactiveResizer = stackedVertically_ ? sideBySideResizer_ : stackedResizer_;
        inactiveResizer.setVisible(false);
        activeResizer.setVisible(true);

        juce::Component* items[] = { &primary_, &activeResizer, &secondary_ };
        splitLayout_.layOutComponents(items, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                      stackedVertically_, true);
    }

private:
    void split(bool vertical)
    {
        stackedVertically_ = vertical;
        isSplit_           = true;
        secondary_.setVisible(true);
        addAndMakeVisible(sideBySideResizer_);
        addAndMakeVisible(stackedResizer_);
        resized();
    }

    bool isSplit_           = false;
    bool stackedVertically_ = false;

    juce::StretchableLayoutManager    splitLayout_;
    juce::StretchableLayoutResizerBar sideBySideResizer_ { &splitLayout_, 1, true };  // left/right split
    juce::StretchableLayoutResizerBar stackedResizer_    { &splitLayout_, 1, false }; // top/bottom split
};

} // namespace looper
