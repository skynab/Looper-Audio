#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "model/SynthSettings.h"
#include "model/Track.h"

#include "TrackColours.h"

namespace looper
{
/**
    Editor for the currently selected Instrument track's timbre
    (model::SynthSettings): oscillator waveform, amp envelope, an optional
    per-voice filter, and an output trim — organized into labelled sections
    the way Reason's Objekt panel groups Oscillator/Filter/Amp controls,
    built from this app's existing plain slider/combo-box widgets rather than
    a new rotary-knob look, to stay visually consistent with the rest of the
    app (see the Mixer master panel's filter/delay/reverb controls).

    Shows a centred placeholder instead of the controls when no Instrument
    track is selected (see setNoTrackSelected) — Drum and Audio tracks have
    no synth to edit.
*/
class SynthEditor final : public juce::Component
{
public:
    // Fires with the whole updated settings on any control change — cheap
    // enough to always send the full struct rather than one callback per
    // field (mirrors how MainComponent::syncEngineTracks already pushes every
    // field together).
    std::function<void(const model::SynthSettings&)> onSettingsChanged;

    SynthEditor()
    {
        placeholderLabel_.setText("Select an Instrument track to edit its synth", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        setupSectionHeader(oscHeader_, "Oscillator");
        waveformBox_.addItem("Sine", 1);
        waveformBox_.addItem("Saw", 2);
        waveformBox_.addItem("Square", 3);
        waveformBox_.addItem("Triangle", 4);
        waveformBox_.setSelectedId(1, juce::dontSendNotification);
        waveformBox_.onChange = [this] { settings_.waveform = waveformBox_.getSelectedId() - 1; notify(); };
        addAndMakeVisible(waveformBox_);

        setupSectionHeader(ampHeader_, "Amp Envelope");
        setupSlider(attackSlider_, "Attack", 0.0, 2000.0, 1.0, " ms",
                    [this] { settings_.attackMs = (float) attackSlider_.getValue(); notify(); });
        setupSlider(decaySlider_, "Decay", 0.0, 2000.0, 1.0, " ms",
                    [this] { settings_.decayMs = (float) decaySlider_.getValue(); notify(); });
        setupSlider(sustainSlider_, "Sustain", 0.0, 100.0, 1.0, " %",
                    [this] { settings_.sustain = (float) (sustainSlider_.getValue() / 100.0); notify(); });
        setupSlider(releaseSlider_, "Release", 0.0, 4000.0, 1.0, " ms",
                    [this] { settings_.releaseMs = (float) releaseSlider_.getValue(); notify(); });

        setupSectionHeader(filterHeader_, "Filter");
        filterEnabledButton_.setButtonText("On");
        filterEnabledButton_.setClickingTogglesState(true);
        filterEnabledButton_.onClick = [this]
        {
            settings_.filterEnabled = filterEnabledButton_.getToggleState();
            updateFilterControlsEnabled();
            notify();
        };
        addAndMakeVisible(filterEnabledButton_);

        filterModeBox_.addItem("Low-pass", 1);
        filterModeBox_.addItem("High-pass", 2);
        filterModeBox_.addItem("Band-pass", 3);
        filterModeBox_.setSelectedId(1, juce::dontSendNotification);
        filterModeBox_.onChange = [this] { settings_.filterMode = juce::jmax(0, filterModeBox_.getSelectedId() - 1); notify(); };
        addAndMakeVisible(filterModeBox_);

        setupSlider(filterCutoffSlider_, "Cutoff", 20.0, 18000.0, 1.0, " Hz",
                    [this] { settings_.filterCutoff = (float) filterCutoffSlider_.getValue(); notify(); });
        filterCutoffSlider_.setSkewFactorFromMidPoint(1000.0);
        setupSlider(filterResonanceSlider_, "Resonance", 0.1, 5.0, 0.01, " Q",
                    [this] { settings_.filterResonance = (float) filterResonanceSlider_.getValue(); notify(); });

        setupSectionHeader(outputHeader_, "Output");
        setupSlider(gainSlider_, "Gain", -24.0, 24.0, 0.1, " dB",
                    [this] { settings_.gainDb = (float) gainSlider_.getValue(); notify(); });

        updateFilterControlsEnabled();
        setControlsVisible(false);
    }

    /** Reflects @p settings without firing onSettingsChanged, and reveals the
        controls (hiding the placeholder). Called whenever the selected track
        changes, or its synth settings change from elsewhere (undo/redo). */
    void setSettings(const model::SynthSettings& settings)
    {
        settings_ = settings;

        waveformBox_.setSelectedId(settings.waveform + 1, juce::dontSendNotification);
        attackSlider_.setValue(settings.attackMs, juce::dontSendNotification);
        decaySlider_.setValue(settings.decayMs, juce::dontSendNotification);
        sustainSlider_.setValue(settings.sustain * 100.0, juce::dontSendNotification);
        releaseSlider_.setValue(settings.releaseMs, juce::dontSendNotification);
        filterEnabledButton_.setToggleState(settings.filterEnabled, juce::dontSendNotification);
        filterModeBox_.setSelectedId(settings.filterMode + 1, juce::dontSendNotification);
        filterCutoffSlider_.setValue(settings.filterCutoff, juce::dontSendNotification);
        filterResonanceSlider_.setValue(settings.filterResonance, juce::dontSendNotification);
        gainSlider_.setValue(settings.gainDb, juce::dontSendNotification);

        updateFilterControlsEnabled();
        setControlsVisible(true);
    }

    /** Shows the placeholder instead of the controls — the selected track
        isn't an Instrument track (or none is selected). */
    void setNoTrackSelected()
    {
        setControlsVisible(false);
    }

    /** Which track this is, so the header says so — this tab's title never
        changes per track, so without this there was no on-screen way to
        tell which track's patch was actually open after switching tracks
        while parked here. */
    void setTrackInfo(const juce::String& name, juce::uint32 colour)
    {
        trackName_   = name;
        trackColour_ = colour;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (controlsVisible_)
            paintTrackHeader(g, headerBounds(), trackName_, trackColour_, model::TrackType::Instrument);
    }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());

        if (! controlsVisible_)
            return;

        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);
        area = area.reduced(10);

        layoutHeader(oscHeader_, area);
        layoutRow(area, waveformBox_);
        area.removeFromTop(10);

        layoutHeader(ampHeader_, area);
        layoutRow(area, attackSlider_);
        layoutRow(area, decaySlider_);
        layoutRow(area, sustainSlider_);
        layoutRow(area, releaseSlider_);
        area.removeFromTop(10);

        layoutHeader(filterHeader_, area);
        auto filterToggleRow = area.removeFromTop(kRowHeight);
        filterEnabledButton_.setBounds(filterToggleRow.removeFromLeft(60).reduced(2));
        filterModeBox_.setBounds(filterToggleRow.reduced(2));
        layoutRow(area, filterCutoffSlider_);
        layoutRow(area, filterResonanceSlider_);
        area.removeFromTop(10);

        layoutHeader(outputHeader_, area);
        layoutRow(area, gainSlider_);
    }

private:
    static constexpr int kRowHeight    = 26;
    static constexpr int kHeaderHeight = 22;

    void setupSectionHeader(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.85f));
        addAndMakeVisible(label);
    }

    void setupSlider(juce::Slider& slider, const juce::String& suffix, double lo, double hi, double step,
                     const juce::String& unitSuffix, std::function<void()> onChange)
    {
        slider.setRange(lo, hi, step);
        slider.setTextValueSuffix(unitSuffix);
        slider.setName(suffix);
        slider.onValueChange = std::move(onChange);
        addAndMakeVisible(slider);
    }

    void layoutHeader(juce::Label& label, juce::Rectangle<int>& area)
    {
        label.setBounds(area.removeFromTop(kHeaderHeight));
    }

    /** Same top strip resized() carves out before the rest of the layout —
        kept in one place so paint() and resized() can't drift apart. */
    juce::Rectangle<int> headerBounds() const
    {
        return getLocalBounds().removeFromTop(kTrackHeaderHeight);
    }

    void layoutRow(juce::Rectangle<int>& area, juce::Component& control)
    {
        control.setBounds(area.removeFromTop(kRowHeight).reduced(0, 2));
        area.removeFromTop(2);
    }

    void updateFilterControlsEnabled()
    {
        const bool on = filterEnabledButton_.getToggleState();
        filterModeBox_.setEnabled(on);
        filterCutoffSlider_.setEnabled(on);
        filterResonanceSlider_.setEnabled(on);
    }

    void setControlsVisible(bool visible)
    {
        controlsVisible_ = visible;
        placeholderLabel_.setVisible(! visible);
        juce::Component* controls[] = { &oscHeader_, &waveformBox_, &ampHeader_,
                                        &attackSlider_, &decaySlider_, &sustainSlider_, &releaseSlider_, &filterHeader_,
                                        &filterEnabledButton_, &filterModeBox_, &filterCutoffSlider_, &filterResonanceSlider_,
                                        &outputHeader_, &gainSlider_ };
        for (auto* c : controls)
            c->setVisible(visible);
        resized();
    }

    void notify()
    {
        if (onSettingsChanged)
            onSettingsChanged(settings_);
    }

    model::SynthSettings settings_;
    bool                  controlsVisible_ = false;
    juce::String          trackName_;
    juce::uint32          trackColour_ = 0;

    juce::Label      placeholderLabel_;

    juce::Label      oscHeader_;
    juce::ComboBox   waveformBox_;

    juce::Label      ampHeader_;
    juce::Slider      attackSlider_, decaySlider_, sustainSlider_, releaseSlider_;

    juce::Label       filterHeader_;
    juce::TextButton  filterEnabledButton_;
    juce::ComboBox    filterModeBox_;
    juce::Slider      filterCutoffSlider_, filterResonanceSlider_;

    juce::Label       outputHeader_;
    juce::Slider      gainSlider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthEditor)
};

} // namespace looper
