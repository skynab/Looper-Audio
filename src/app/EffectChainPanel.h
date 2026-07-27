#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/PluginHost.h"
#include "model/Effects.h"

namespace looper
{
/**
    The selected track's effect chain: an ordered list of slots, each a
    built-in (filter, delay, reverb) or a hosted plugin, with add, remove,
    reorder and bypass — and the selected slot's parameters below.

    Replaces the fixed filter/delay/reverb panel that came before it. That one
    could only ever edit one of each, which stopped being true the moment a
    chain could hold two filters or a plugin.

    Owns no document state: it draws from a snapshot and reports intent through
    the callbacks, like ArrangementView and SessionView.
*/
class EffectChainPanel final : public juce::Component
{
public:
    std::function<void(int slotIndex)>                              onSlotSelected;
    std::function<void(int slotIndex, bool enabled)>                onSlotBypassToggled;
    std::function<void(int slotIndex)>                              onSlotRemoved;
    std::function<void(int slotIndex, int delta)>                   onSlotMoved;   // -1 up, +1 down
    std::function<void(model::EffectKind kind)>                     onBuiltInAdded;
    std::function<void(const engine::PluginEntry&)>                 onPluginAdded;
    std::function<void(int slotIndex)>                              onPluginEditorRequested;
    std::function<void()>                                           onScanRequested;
    std::function<void(const model::EffectSlot& slot, int slotIndex)> onSlotParamsChanged;

    EffectChainPanel()
    {
        placeholder_.setText("Select a track to edit its effects", juce::dontSendNotification);
        placeholder_.setJustificationType(juce::Justification::centred);
        placeholder_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholder_);

        addButton_.setButtonText("+ Add");
        addButton_.onClick = [this] { showAddMenu(); };
        addAndMakeVisible(addButton_);

        removeButton_.setButtonText("Remove");
        removeButton_.onClick = [this]
        {
            if (onSlotRemoved && isValidSlot(selected_))
                onSlotRemoved(selected_);
        };
        addAndMakeVisible(removeButton_);

        upButton_.setButtonText("Up");
        upButton_.onClick = [this] { if (onSlotMoved && isValidSlot(selected_)) onSlotMoved(selected_, -1); };
        addAndMakeVisible(upButton_);

        downButton_.setButtonText("Down");
        downButton_.onClick = [this] { if (onSlotMoved && isValidSlot(selected_)) onSlotMoved(selected_, +1); };
        addAndMakeVisible(downButton_);

        editorButton_.setButtonText("Open Plugin Editor");
        editorButton_.onClick = [this]
        {
            if (onPluginEditorRequested && isValidSlot(selected_))
                onPluginEditorRequested(selected_);
        };
        addChildComponent(editorButton_);

        setupSlider(cutoff_, 20.0, 18000.0, 1.0, " Hz", [this] { pushParams(); });
        cutoff_.setSkewFactorFromMidPoint(1000.0);
        setupSlider(resonance_, 0.1, 5.0, 0.01, " Q", [this] { pushParams(); });
        setupSlider(timeMs_, 20.0, 1000.0, 1.0, " ms", [this] { pushParams(); });
        setupSlider(feedback_, 0.0, 95.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(mix_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(roomSize_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });
        setupSlider(damping_, 0.0, 100.0, 1.0, " %", [this] { pushParams(); });

        filterMode_.addItem("Low-pass", 1);
        filterMode_.addItem("High-pass", 2);
        filterMode_.addItem("Band-pass", 3);
        filterMode_.setSelectedId(1, juce::dontSendNotification);
        filterMode_.onChange = [this] { pushParams(); };
        addAndMakeVisible(filterMode_);

        setContentVisible(false);
    }

    /** The scanned plugins offered by the Add menu. */
    void setAvailablePlugins(std::vector<engine::PluginEntry> plugins) { plugins_ = std::move(plugins); }

    void setChain(const std::vector<model::EffectSlot>& chain)
    {
        chain_ = chain;
        selected_ = chain_.empty() ? -1 : juce::jlimit(0, (int) chain_.size() - 1, juce::jmax(0, selected_));
        refreshParamControls();
        setContentVisible(true);
    }

    void setNoTrackSelected() { setContentVisible(false); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int slot = slotAtY(e.position.y);
        if (slot < 0)
            return;

        // The left-hand strip of a row is its bypass button; the rest selects.
        if (e.position.x < (float) kBypassWidth)
        {
            if (onSlotBypassToggled)
                onSlotBypassToggled(slot, ! chain_[(size_t) slot].enabled);
            return;
        }

        selected_ = slot;
        refreshParamControls();
        repaint();
        resized();
        if (onSlotSelected)
            onSlotSelected(slot);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));
        if (! contentVisible_)
            return;

        g.setFont(juce::FontOptions(12.0f));
        for (int i = 0; i < (int) chain_.size(); ++i)
        {
            const auto  row  = rowBounds(i);
            const auto& slot = chain_[(size_t) i];

            g.setColour(i == selected_ ? juce::Colours::white.withAlpha(0.10f)
                                       : juce::Colours::white.withAlpha(0.04f));
            g.fillRect(row);

            if (i == selected_)
            {
                g.setColour(juce::Colours::orange.withAlpha(0.7f));
                g.drawRect(row, 1);
            }

            // Bypass indicator: filled when active, hollow when bypassed —
            // the same meaning for a plugin as for a built-in.
            const auto dot = juce::Rectangle<int>(row.getX() + 8, row.getCentreY() - 5, 10, 10).toFloat();
            g.setColour(slot.enabled ? juce::Colours::limegreen : juce::Colours::white.withAlpha(0.25f));
            slot.enabled ? g.fillEllipse(dot) : g.drawEllipse(dot, 1.2f);

            g.setColour(juce::Colours::white.withAlpha(slot.enabled ? 0.9f : 0.45f));
            g.drawText(slotLabel(slot), row.getX() + kBypassWidth, row.getY(),
                       row.getWidth() - kBypassWidth - 6, row.getHeight(),
                       juce::Justification::centredLeft);
        }

        if (chain_.empty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText("No effects — use + Add", listArea(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        placeholder_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds().reduced(6);

        auto toolbar = area.removeFromTop(kToolbarHeight);
        addButton_.setBounds(toolbar.removeFromLeft(64).reduced(2));
        removeButton_.setBounds(toolbar.removeFromLeft(70).reduced(2));
        upButton_.setBounds(toolbar.removeFromLeft(44).reduced(2));
        downButton_.setBounds(toolbar.removeFromLeft(56).reduced(2));

        area.removeFromTop((int) chain_.size() * kRowHeight + 6);

        // Whatever the selected slot needs: a plugin gets an editor button, a
        // built-in gets its own parameters.
        if (! isValidSlot(selected_))
            return;

        const auto kind = chain_[(size_t) selected_].kind;
        if (kind == model::EffectKind::Plugin)
        {
            editorButton_.setBounds(area.removeFromTop(kRowHeight).reduced(2));
            return;
        }

        auto row = [&area](juce::Component& c) { c.setBounds(area.removeFromTop(kRowHeight).reduced(2)); };
        if (kind == model::EffectKind::Filter) { row(filterMode_); row(cutoff_); row(resonance_); }
        else if (kind == model::EffectKind::Delay) { row(timeMs_); row(feedback_); row(mix_); }
        else if (kind == model::EffectKind::Reverb) { row(roomSize_); row(damping_); row(mix_); }
    }

private:
    static constexpr int kRowHeight     = 26;
    static constexpr int kToolbarHeight = 26;
    static constexpr int kBypassWidth   = 26;

    juce::Rectangle<int> listArea() const
    {
        auto area = getLocalBounds().reduced(6);
        area.removeFromTop(kToolbarHeight);
        return area;
    }

    juce::Rectangle<int> rowBounds(int index) const
    {
        auto area = listArea();
        return { area.getX(), area.getY() + index * kRowHeight, area.getWidth(), kRowHeight };
    }

    int slotAtY(float y) const
    {
        const auto area = listArea();
        const int  idx  = (int) ((y - (float) area.getY()) / (float) kRowHeight);
        return (y >= (float) area.getY() && idx >= 0 && idx < (int) chain_.size()) ? idx : -1;
    }

    bool isValidSlot(int index) const { return index >= 0 && index < (int) chain_.size(); }

    static juce::String slotLabel(const model::EffectSlot& slot)
    {
        switch (slot.kind)
        {
            case model::EffectKind::Filter: return "Filter";
            case model::EffectKind::Delay:  return "Delay";
            case model::EffectKind::Reverb: return "Reverb";
            case model::EffectKind::Plugin:
                // A plugin the machine no longer has still names itself, which
                // is the whole reason the document stores the name.
                return slot.plugin.name.empty() ? juce::String("(missing plugin)")
                                                : juce::String(slot.plugin.name);
        }
        return {};
    }

    void showAddMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Filter");
        menu.addItem(2, "Delay");
        menu.addItem(3, "Reverb");
        menu.addSeparator();

        if (plugins_.empty())
        {
            menu.addItem(4, "Scan for plugins...");
        }
        else
        {
            juce::PopupMenu pluginMenu;
            for (int i = 0; i < (int) plugins_.size(); ++i)
                pluginMenu.addItem(100 + i, plugins_[(size_t) i].name + "  (" + plugins_[(size_t) i].format + ")");
            pluginMenu.addSeparator();
            pluginMenu.addItem(4, "Rescan...");
            menu.addSubMenu("Plugins", pluginMenu);
        }

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_),
                           [this](int result)
        {
            if (result == 0)
                return;
            if (result == 1 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Filter);
            else if (result == 2 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Delay);
            else if (result == 3 && onBuiltInAdded) onBuiltInAdded(model::EffectKind::Reverb);
            else if (result == 4 && onScanRequested) onScanRequested();
            else if (result >= 100)
            {
                const size_t index = (size_t) (result - 100);
                if (index < plugins_.size() && onPluginAdded)
                    onPluginAdded(plugins_[index]);
            }
        });
    }

    void setupSlider(juce::Slider& slider, double lo, double hi, double step,
                     const juce::String& suffix, std::function<void()> onChange)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = std::move(onChange);
        addChildComponent(slider);
    }

    /** Mirrors the selected slot into the parameter controls, and shows only
        the ones that slot's kind actually uses. */
    void refreshParamControls()
    {
        juce::Component* all[] = { &filterMode_, &cutoff_, &resonance_, &timeMs_,
                                   &feedback_, &mix_, &roomSize_, &damping_, &editorButton_ };
        for (auto* c : all)
            c->setVisible(false);

        if (! isValidSlot(selected_))
            return;

        const auto& slot = chain_[(size_t) selected_];
        updating_ = true;

        switch (slot.kind)
        {
            case model::EffectKind::Filter:
                filterMode_.setSelectedId(slot.filter.mode + 1, juce::dontSendNotification);
                cutoff_.setValue(slot.filter.cutoff, juce::dontSendNotification);
                resonance_.setValue(slot.filter.resonance, juce::dontSendNotification);
                filterMode_.setVisible(true); cutoff_.setVisible(true); resonance_.setVisible(true);
                break;

            case model::EffectKind::Delay:
                timeMs_.setValue(slot.delay.timeMs, juce::dontSendNotification);
                feedback_.setValue(slot.delay.feedback * 100.0, juce::dontSendNotification);
                mix_.setValue(slot.delay.mix * 100.0, juce::dontSendNotification);
                timeMs_.setVisible(true); feedback_.setVisible(true); mix_.setVisible(true);
                break;

            case model::EffectKind::Reverb:
                roomSize_.setValue(slot.reverb.roomSize * 100.0, juce::dontSendNotification);
                damping_.setValue(slot.reverb.damping * 100.0, juce::dontSendNotification);
                mix_.setValue(slot.reverb.mix * 100.0, juce::dontSendNotification);
                roomSize_.setVisible(true); damping_.setVisible(true); mix_.setVisible(true);
                break;

            case model::EffectKind::Plugin:
                editorButton_.setVisible(true);
                break;
        }

        updating_ = false;
        resized();
    }

    /** Reads the controls back into the selected slot and reports it. Guarded
        against the setValue calls in refreshParamControls, which would
        otherwise echo straight back as a user edit. */
    void pushParams()
    {
        if (updating_ || ! isValidSlot(selected_) || ! onSlotParamsChanged)
            return;

        auto slot = chain_[(size_t) selected_];
        switch (slot.kind)
        {
            case model::EffectKind::Filter:
                slot.filter.mode      = juce::jmax(0, filterMode_.getSelectedId() - 1);
                slot.filter.cutoff    = (float) cutoff_.getValue();
                slot.filter.resonance = (float) resonance_.getValue();
                break;
            case model::EffectKind::Delay:
                slot.delay.timeMs   = (float) timeMs_.getValue();
                slot.delay.feedback = (float) (feedback_.getValue() / 100.0);
                slot.delay.mix      = (float) (mix_.getValue() / 100.0);
                break;
            case model::EffectKind::Reverb:
                slot.reverb.roomSize = (float) (roomSize_.getValue() / 100.0);
                slot.reverb.damping  = (float) (damping_.getValue() / 100.0);
                slot.reverb.mix      = (float) (mix_.getValue() / 100.0);
                break;
            case model::EffectKind::Plugin:
                return; // a plugin's parameters live in its own editor
        }

        chain_[(size_t) selected_] = slot;
        onSlotParamsChanged(slot, selected_);
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholder_.setVisible(! visible);

        juce::Component* toolbar[] = { &addButton_, &removeButton_, &upButton_, &downButton_ };
        for (auto* c : toolbar)
            c->setVisible(visible);

        if (! visible)
        {
            juce::Component* params[] = { &filterMode_, &cutoff_, &resonance_, &timeMs_,
                                          &feedback_, &mix_, &roomSize_, &damping_, &editorButton_ };
            for (auto* c : params)
                c->setVisible(false);
        }
        resized();
        repaint();
    }

    std::vector<model::EffectSlot>   chain_;
    std::vector<engine::PluginEntry> plugins_;
    int                              selected_       = 0;
    bool                             contentVisible_ = false;
    bool                             updating_       = false;

    juce::Label      placeholder_;
    juce::TextButton addButton_, removeButton_, upButton_, downButton_, editorButton_;
    juce::ComboBox   filterMode_;
    juce::Slider     cutoff_, resonance_, timeMs_, feedback_, mix_, roomSize_, damping_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EffectChainPanel)
};

} // namespace looper
