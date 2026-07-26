#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Effects.h"

namespace looper
{
/**
    The selected track's insert effects: a filter, a delay and a reverb, in
    that fixed order, each switchable on its own — the same three the mixer's
    master strip offers, but applied to one track before its fader.

    Built the same way as SynthEditor (labelled sections of plain sliders and
    combo boxes, a placeholder when there's nothing to edit) so the two panes
    read as siblings, which they are: both edit the selected track's sound.

    Reports the whole settings triple on any change rather than one callback
    per parameter — cheap, and it matches how MainComponent::syncEngineTracks
    already pushes every field together.
*/
class TrackEffectsPanel final : public juce::Component
{
public:
    std::function<void(const model::FilterSettings&,
                       const model::DelaySettings&,
                       const model::ReverbSettings&)> onSettingsChanged;

    TrackEffectsPanel()
    {
        placeholderLabel_.setText("Select a track to edit its effects", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        // ---- filter ----
        setupHeader(filterHeader_, "Filter");
        setupToggle(filterEnabled_, [this]
        {
            filter_.enabled = filterEnabled_.getToggleState();
            updateEnabledStates();
            notify();
        });
        filterMode_.addItem("Low-pass", 1);
        filterMode_.addItem("High-pass", 2);
        filterMode_.addItem("Band-pass", 3);
        filterMode_.setSelectedId(1, juce::dontSendNotification);
        filterMode_.onChange = [this] { filter_.mode = juce::jmax(0, filterMode_.getSelectedId() - 1); notify(); };
        addAndMakeVisible(filterMode_);

        setupSlider(filterCutoff_, 20.0, 18000.0, 1.0, " Hz",
                    [this] { filter_.cutoff = (float) filterCutoff_.getValue(); notify(); });
        filterCutoff_.setSkewFactorFromMidPoint(1000.0);
        setupSlider(filterReso_, 0.1, 5.0, 0.01, " Q",
                    [this] { filter_.resonance = (float) filterReso_.getValue(); notify(); });
        setupLabel(filterCutoffLabel_, "Cutoff");
        setupLabel(filterResoLabel_, "Reso");

        // ---- delay ----
        setupHeader(delayHeader_, "Delay");
        setupToggle(delayEnabled_, [this]
        {
            delay_.enabled = delayEnabled_.getToggleState();
            updateEnabledStates();
            notify();
        });
        setupSlider(delayTime_, 20.0, 1000.0, 1.0, " ms",
                    [this] { delay_.timeMs = (float) delayTime_.getValue(); notify(); });
        setupSlider(delayFeedback_, 0.0, 95.0, 1.0, " %",
                    [this] { delay_.feedback = (float) (delayFeedback_.getValue() / 100.0); notify(); });
        setupSlider(delayMix_, 0.0, 100.0, 1.0, " %",
                    [this] { delay_.mix = (float) (delayMix_.getValue() / 100.0); notify(); });
        setupLabel(delayTimeLabel_, "Time");
        setupLabel(delayFeedbackLabel_, "Fb");
        setupLabel(delayMixLabel_, "Mix");

        // ---- reverb ----
        setupHeader(reverbHeader_, "Reverb");
        setupToggle(reverbEnabled_, [this]
        {
            reverb_.enabled = reverbEnabled_.getToggleState();
            updateEnabledStates();
            notify();
        });
        setupSlider(reverbRoom_, 0.0, 100.0, 1.0, " %",
                    [this] { reverb_.roomSize = (float) (reverbRoom_.getValue() / 100.0); notify(); });
        setupSlider(reverbDamp_, 0.0, 100.0, 1.0, " %",
                    [this] { reverb_.damping = (float) (reverbDamp_.getValue() / 100.0); notify(); });
        setupSlider(reverbMix_, 0.0, 100.0, 1.0, " %",
                    [this] { reverb_.mix = (float) (reverbMix_.getValue() / 100.0); notify(); });
        setupLabel(reverbRoomLabel_, "Room");
        setupLabel(reverbDampLabel_, "Damp");
        setupLabel(reverbMixLabel_, "Mix");

        setControlsVisible(false);
    }

    /** Reflects a track's inserts without firing onSettingsChanged. */
    void setSettings(const model::FilterSettings& filter,
                     const model::DelaySettings& delay,
                     const model::ReverbSettings& reverb)
    {
        filter_ = filter;
        delay_  = delay;
        reverb_ = reverb;

        filterEnabled_.setToggleState(filter.enabled, juce::dontSendNotification);
        filterMode_.setSelectedId(filter.mode + 1, juce::dontSendNotification);
        filterCutoff_.setValue(filter.cutoff, juce::dontSendNotification);
        filterReso_.setValue(filter.resonance, juce::dontSendNotification);

        delayEnabled_.setToggleState(delay.enabled, juce::dontSendNotification);
        delayTime_.setValue(delay.timeMs, juce::dontSendNotification);
        delayFeedback_.setValue(delay.feedback * 100.0, juce::dontSendNotification);
        delayMix_.setValue(delay.mix * 100.0, juce::dontSendNotification);

        reverbEnabled_.setToggleState(reverb.enabled, juce::dontSendNotification);
        reverbRoom_.setValue(reverb.roomSize * 100.0, juce::dontSendNotification);
        reverbDamp_.setValue(reverb.damping * 100.0, juce::dontSendNotification);
        reverbMix_.setValue(reverb.mix * 100.0, juce::dontSendNotification);

        updateEnabledStates();
        setControlsVisible(true);
    }

    void setNoTrackSelected() { setControlsVisible(false); }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());
        if (! controlsVisible_)
            return;

        auto area = getLocalBounds().reduced(10);

        layoutSectionHeader(area, filterHeader_, filterEnabled_);
        layoutRow(area, filterMode_);
        layoutLabelledRow(area, filterCutoffLabel_, filterCutoff_);
        layoutLabelledRow(area, filterResoLabel_, filterReso_);
        area.removeFromTop(10);

        layoutSectionHeader(area, delayHeader_, delayEnabled_);
        layoutLabelledRow(area, delayTimeLabel_, delayTime_);
        layoutLabelledRow(area, delayFeedbackLabel_, delayFeedback_);
        layoutLabelledRow(area, delayMixLabel_, delayMix_);
        area.removeFromTop(10);

        layoutSectionHeader(area, reverbHeader_, reverbEnabled_);
        layoutLabelledRow(area, reverbRoomLabel_, reverbRoom_);
        layoutLabelledRow(area, reverbDampLabel_, reverbDamp_);
        layoutLabelledRow(area, reverbMixLabel_, reverbMix_);
    }

private:
    static constexpr int kRowHeight    = 24;
    static constexpr int kHeaderHeight = 22;

    void setupHeader(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.85f));
        addAndMakeVisible(label);
    }

    void setupLabel(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
        label.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(label);
    }

    void setupToggle(juce::TextButton& button, std::function<void()> onClick)
    {
        button.setButtonText("On");
        button.setClickingTogglesState(true);
        button.onClick = std::move(onClick);
        addAndMakeVisible(button);
    }

    void setupSlider(juce::Slider& slider, double lo, double hi, double step,
                     const juce::String& suffix, std::function<void()> onChange)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = std::move(onChange);
        addAndMakeVisible(slider);
    }

    void layoutSectionHeader(juce::Rectangle<int>& area, juce::Label& header, juce::TextButton& toggle)
    {
        auto row = area.removeFromTop(kHeaderHeight);
        toggle.setBounds(row.removeFromRight(46).reduced(1));
        header.setBounds(row);
    }

    void layoutRow(juce::Rectangle<int>& area, juce::Component& control)
    {
        control.setBounds(area.removeFromTop(kRowHeight).reduced(0, 2));
    }

    void layoutLabelledRow(juce::Rectangle<int>& area, juce::Label& label, juce::Component& control)
    {
        auto row = area.removeFromTop(kRowHeight);
        label.setBounds(row.removeFromLeft(48));
        control.setBounds(row.reduced(0, 2));
    }

    /** Greys out a section's parameters while it's switched off — the
        controls stay visible so the settings are still readable. */
    void updateEnabledStates()
    {
        juce::Component* filterControls[] = { &filterMode_, &filterCutoff_, &filterReso_ };
        for (auto* c : filterControls)
            c->setEnabled(filter_.enabled);

        juce::Component* delayControls[] = { &delayTime_, &delayFeedback_, &delayMix_ };
        for (auto* c : delayControls)
            c->setEnabled(delay_.enabled);

        juce::Component* reverbControls[] = { &reverbRoom_, &reverbDamp_, &reverbMix_ };
        for (auto* c : reverbControls)
            c->setEnabled(reverb_.enabled);
    }

    void setControlsVisible(bool visible)
    {
        controlsVisible_ = visible;
        placeholderLabel_.setVisible(! visible);

        juce::Component* controls[] = {
            &filterHeader_, &filterEnabled_, &filterMode_, &filterCutoffLabel_, &filterCutoff_,
            &filterResoLabel_, &filterReso_,
            &delayHeader_, &delayEnabled_, &delayTimeLabel_, &delayTime_,
            &delayFeedbackLabel_, &delayFeedback_, &delayMixLabel_, &delayMix_,
            &reverbHeader_, &reverbEnabled_, &reverbRoomLabel_, &reverbRoom_,
            &reverbDampLabel_, &reverbDamp_, &reverbMixLabel_, &reverbMix_
        };
        for (auto* c : controls)
            c->setVisible(visible);

        resized();
    }

    void notify()
    {
        if (onSettingsChanged)
            onSettingsChanged(filter_, delay_, reverb_);
    }

    model::FilterSettings filter_;
    model::DelaySettings  delay_;
    model::ReverbSettings reverb_;
    bool                  controlsVisible_ = false;

    juce::Label      placeholderLabel_;

    juce::Label      filterHeader_, filterCutoffLabel_, filterResoLabel_;
    juce::TextButton filterEnabled_;
    juce::ComboBox   filterMode_;
    juce::Slider     filterCutoff_, filterReso_;

    juce::Label      delayHeader_, delayTimeLabel_, delayFeedbackLabel_, delayMixLabel_;
    juce::TextButton delayEnabled_;
    juce::Slider     delayTime_, delayFeedback_, delayMix_;

    juce::Label      reverbHeader_, reverbRoomLabel_, reverbDampLabel_, reverbMixLabel_;
    juce::TextButton reverbEnabled_;
    juce::Slider     reverbRoom_, reverbDamp_, reverbMix_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackEffectsPanel)
};

} // namespace looper
