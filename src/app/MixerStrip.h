#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "LevelMeter.h"

namespace looper
{
/**
    A single channel strip in the mixer view: track name, mute/solo, a vertical
    gain fader, and a level meter. Purely a display/input widget — it owns no
    model or engine state; the owner wires its callbacks to the document and
    engine, and feeds it fresh values via the setters.

    Clicking the strip's background (but not its buttons/fader) selects the
    track, matching the common DAW pattern of "click a channel strip to arm it."
*/
class MixerStrip final : public juce::Component
{
public:
    std::function<void(float)> onGainChange;
    std::function<void(bool)>  onMuteChange;
    std::function<void(bool)>  onSoloChange;
    std::function<void()>      onSelect;

    MixerStrip()
    {
        nameLabel_.setJustificationType(juce::Justification::centred);
        nameLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
        nameLabel_.setInterceptsMouseClicks(false, false); // clicks pass through to select the strip
        addAndMakeVisible(nameLabel_);

        muteButton_.setClickingTogglesState(true);
        muteButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colours::orangered);
        muteButton_.onClick = [this] { if (onMuteChange) onMuteChange(muteButton_.getToggleState()); };
        addAndMakeVisible(muteButton_);

        soloButton_.setClickingTogglesState(true);
        soloButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colours::yellow);
        soloButton_.onClick = [this] { if (onSoloChange) onSoloChange(soloButton_.getToggleState()); };
        addAndMakeVisible(soloButton_);

        gainSlider_.setSliderStyle(juce::Slider::LinearVertical);
        gainSlider_.setRange(-60.0, 6.0, 0.1);
        gainSlider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
        gainSlider_.setTextValueSuffix(" dB");
        gainSlider_.onValueChange = [this] { if (onGainChange) onGainChange((float) gainSlider_.getValue()); };
        addAndMakeVisible(gainSlider_);

        addAndMakeVisible(meter_);
    }

    void setTrackName(const juce::String& name) { nameLabel_.setText(name, juce::dontSendNotification); }
    void setGainDb(float db)   { gainSlider_.setValue(db, juce::dontSendNotification); }
    void setMuted(bool muted)  { muteButton_.setToggleState(muted, juce::dontSendNotification); }
    void setSoloed(bool solo)  { soloButton_.setToggleState(solo, juce::dontSendNotification); }
    void setSelected(bool sel) { if (selected_ != sel) { selected_ = sel; repaint(); } }
    void setLevel(int channel, float linearPeak) { meter_.setLevel(channel, linearPeak); }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(selected_ ? juce::Colours::white.withAlpha(0.10f) : juce::Colours::black.withAlpha(0.15f));
        g.fillRoundedRectangle(area, 4.0f);

        if (selected_)
        {
            g.setColour(juce::Colours::orange.withAlpha(0.8f));
            g.drawRoundedRectangle(area.reduced(1.0f), 4.0f, 1.5f);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onSelect)
            onSelect();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);

        nameLabel_.setBounds(area.removeFromTop(20));
        area.removeFromTop(4);

        auto btnRow = area.removeFromTop(22);
        muteButton_.setBounds(btnRow.removeFromLeft(btnRow.getWidth() / 2).reduced(2));
        soloButton_.setBounds(btnRow.reduced(2));
        area.removeFromTop(6);

        auto meterArea = area.removeFromRight(20);
        meter_.setBounds(meterArea);
        area.removeFromRight(4);
        gainSlider_.setBounds(area);
    }

private:
    juce::Label      nameLabel_;
    juce::TextButton muteButton_ { "M" };
    juce::TextButton soloButton_ { "S" };
    juce::Slider     gainSlider_;
    LevelMeter       meter_;
    bool             selected_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerStrip)
};

} // namespace looper
