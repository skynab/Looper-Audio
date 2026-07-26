#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace looper
{
/**
    A single tab header: click to activate its panel, drag it onto a different
    DockRegion's header strip to move the panel there. Deliberately not a
    juce::Button subclass — plain paint()/mouse* like this app's other custom
    widgets (MixerStrip, LevelMeter) — so the drag gesture (telling a click
    apart from a drag-past-threshold) is fully under our control.
*/
class DockTabHeader final : public juce::Component
{
public:
    std::function<void()> onClick;
    std::function<void()> onDragStarted;

    void setText(const juce::String& text) { text_ = text; repaint(); }
    void setActive(bool active) { active_ = active; repaint(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(active_ ? juce::Colour(0xff3d3d44) : juce::Colour(0xff2a2a2e));
        g.setColour(juce::Colours::white.withAlpha(active_ ? 0.95f : 0.55f));
        g.drawText(text_, getLocalBounds().reduced(10, 0), juce::Justification::centred);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.drawRect(getLocalBounds());
    }

    void mouseDown(const juce::MouseEvent&) override { dragStarted_ = false; }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragStarted_ && e.getDistanceFromDragStart() > 8)
        {
            dragStarted_ = true;
            if (onDragStarted)
                onDragStarted();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (! dragStarted_ && e.getDistanceFromDragStart() < 4 && onClick)
            onClick();
        dragStarted_ = false;
    }

private:
    juce::String text_;
    bool active_      = false;
    bool dragStarted_ = false;
};

/**
    One dockable region of the workspace: a strip of tab headers (one per
    hosted panel) plus the currently-active panel's content below it. Panels
    can be dragged from one DockRegion's header strip onto another's to move
    them there — the actual re-parenting is coordinated by whoever owns two or
    more DockRegions (see MainComponent::movePanelBetweenRegions), since a
    region only knows about its own panels.

    A DockRegion never owns the panel Components passed to addPanel() (they're
    expected to outlive it, e.g. as members of the owning MainComponent) — it
    only parents/unparents them via addAndMakeVisible/removeChildComponent,
    same as any JUCE re-parenting.
*/
class DockRegion final : public juce::Component,
                         public juce::DragAndDropTarget
{
public:
    // Fired when a panel dragged FROM ELSEWHERE is dropped on this region.
    // `panelName` identifies which panel (matches the name it was added under
    // in whichever region currently hosts it); the owner is expected to look
    // up that panel's Component, remove it from its current region, and
    // addPanel() it here.
    std::function<void(const juce::String& panelName, DockRegion& target)> onForeignPanelDropped;

    void addPanel(const juce::String& name, juce::Component& content)
    {
        auto header = std::make_unique<DockTabHeader>();
        header->setText(name);
        header->onClick       = [this, name] { showPanel(name); };
        header->onDragStarted = [this, name]
        {
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this))
                dnd->startDragging(name, this);
        };
        addAndMakeVisible(*header);
        addAndMakeVisible(content);
        content.setVisible(false);

        panels_.push_back({ name, &content, std::move(header) });
        showPanel(name);
    }

    // Detaches `name` from this region: removes its header, unparents its
    // content, and drops the bookkeeping entry. Does not delete the content
    // component — callers re-home it elsewhere via addPanel(), or it's simply
    // left unparented (and invisible) if nowhere else wants it yet.
    void removePanel(const juce::String& name)
    {
        for (size_t i = 0; i < panels_.size(); ++i)
        {
            if (panels_[i].name != name)
                continue;

            removeChildComponent(panels_[i].header.get());
            removeChildComponent(panels_[i].content);
            panels_.erase(panels_.begin() + (long) i);
            break;
        }

        if (! panels_.empty())
        {
            activeIndex_ = juce::jlimit(0, (int) panels_.size() - 1, activeIndex_);
            showPanel(panels_[(size_t) activeIndex_].name);
        }
        else
        {
            activeIndex_ = 0;
            resized();
        }
    }

    bool hasPanel(const juce::String& name) const
    {
        for (auto& p : panels_)
            if (p.name == name)
                return true;
        return false;
    }

    int numPanels() const { return (int) panels_.size(); }

    /** Every panel name currently hosted here, in tab order. Used when
        merging one region's panels into another (see
        CenterSplitArea::unsplit). */
    std::vector<juce::String> panelNames() const
    {
        std::vector<juce::String> names;
        for (const auto& p : panels_)
            names.push_back(p.name);
        return names;
    }

    /** The content Component for @p name, or nullptr if this region doesn't
        host it — lets a caller move a panel between regions without needing
        its own separate name->Component lookup table. */
    juce::Component* contentFor(const juce::String& name) const
    {
        for (const auto& p : panels_)
            if (p.name == name)
                return p.content;
        return nullptr;
    }

    /** The name of whichever panel is currently visible, or an empty string
        if this region hosts none. Used to persist/restore the workspace
        layout (see MainComponent::saveDockLayout). */
    juce::String activePanelName() const
    {
        return (activeIndex_ >= 0 && activeIndex_ < (int) panels_.size())
                   ? panels_[(size_t) activeIndex_].name
                   : juce::String();
    }

    void showPanel(const juce::String& name)
    {
        for (size_t i = 0; i < panels_.size(); ++i)
        {
            const bool active = (panels_[i].name == name);
            panels_[i].content->setVisible(active);
            panels_[i].header->setActive(active);
            if (active)
                activeIndex_ = (int) i;
        }
        resized();
    }

    void paint(juce::Graphics& g) override
    {
        if (dragHighlight_)
        {
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRect(getLocalBounds());
        }
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.drawRect(getLocalBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto headerRow  = area.removeFromTop(26);
        const int headerWidth = panels_.empty()
                                     ? 0
                                     : juce::jmin(140, headerRow.getWidth() / (int) panels_.size());
        for (auto& p : panels_)
            p.header->setBounds(headerRow.removeFromLeft(headerWidth));

        for (auto& p : panels_)
            if (p.content->isVisible())
                p.content->setBounds(area);
    }

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        // File drags (from a FileBrowserPanel's FileTreeComponent) are for
        // ArrangementView, not for regrouping dock panels — reject them here
        // so they fall through to whichever ArrangementView is underneath.
        if (dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr)
            return false;
        return details.description.isString() && ! hasPanel(details.description.toString());
    }

    void itemDragEnter(const SourceDetails&) override { dragHighlight_ = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { dragHighlight_ = false; repaint(); }

    void itemDropped(const SourceDetails& details) override
    {
        dragHighlight_ = false;
        repaint();
        if (onForeignPanelDropped)
            onForeignPanelDropped(details.description.toString(), *this);
    }

private:
    struct Panel
    {
        juce::String                   name;
        juce::Component*               content;
        std::unique_ptr<DockTabHeader> header;
    };

    std::vector<Panel> panels_;
    int                 activeIndex_   = 0;
    bool                dragHighlight_ = false;
};

} // namespace looper
