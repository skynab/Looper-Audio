#include "MainComponent.h"

#include <rt/RealtimeGuard.h>

#include <cmath>

namespace looper
{
MainComponent::MainComponent()
    : deviceSelector(deviceManager,
                     0, 0,     // min/max audio input channels
                     0, 2,     // min/max audio output channels
                     false,    // show MIDI input selector
                     false,    // show MIDI output selector
                     false,    // treat channels as stereo pairs
                     false)    // hide advanced options
{
    addAndMakeVisible(toneButton);
    toneButton.onClick = [this]
    {
        playing_.store(toneButton.getToggleState(), std::memory_order_relaxed);
    };

    frequencySlider.setRange(50.0, 2000.0, 1.0);
    frequencySlider.setSkewFactorFromMidPoint(440.0);
    frequencySlider.setValue(440.0);
    frequencySlider.setTextValueSuffix(" Hz");
    frequencySlider.onValueChange = [this]
    {
        targetFrequency_.store((float) frequencySlider.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible(frequencySlider);
    frequencyLabel.attachToComponent(&frequencySlider, true);

    gainSlider.setRange(-60.0, 0.0, 0.1);
    gainSlider.setValue(-12.0);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.onValueChange = [this]
    {
        targetGainDb_.store((float) gainSlider.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible(gainSlider);
    gainLabel.attachToComponent(&gainSlider, true);

    addAndMakeVisible(deviceSelector);

    setSize(560, 440);

    // Log audio-device changes from the message thread.
    deviceManager.addChangeListener(this);

    // Request 0 inputs, 2 outputs. Triggers device setup + prepareToPlay().
    setAudioChannels(0, 2);
}

MainComponent::~MainComponent()
{
    deviceManager.removeChangeListener(this);
    shutdownAudio();
}

void MainComponent::prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate)
{
    sampleRate_ = sampleRate;
    phase_ = 0.0;
    gain_.reset(sampleRate, 0.02); // 20 ms ramp to avoid clicks
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    rt::markCurrentThreadAsAudioThread();

    auto* buffer         = bufferToFill.buffer;
    const auto startSample = bufferToFill.startSample;
    const auto numSamples  = bufferToFill.numSamples;
    const auto numChannels = buffer->getNumChannels();

    const bool playing = playing_.load(std::memory_order_relaxed);
    const float target = playing
        ? juce::Decibels::decibelsToGain(targetGainDb_.load(std::memory_order_relaxed))
        : 0.0f;
    gain_.setTargetValue(target);

    if (sampleRate_ <= 0.0)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const double frequency = (double) targetFrequency_.load(std::memory_order_relaxed);
    const double increment = juce::MathConstants<double>::twoPi * frequency / sampleRate_;

    for (int i = 0; i < numSamples; ++i)
    {
        const float value = std::sin((float) phase_) * gain_.getNextValue();

        phase_ += increment;
        if (phase_ >= juce::MathConstants<double>::twoPi)
            phase_ -= juce::MathConstants<double>::twoPi;

        for (int ch = 0; ch < numChannels; ++ch)
            buffer->setSample(ch, startSample + i, value);
    }
}

void MainComponent::releaseResources()
{
    // Nothing streamed yet.
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(12);

    toneButton.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);

    frequencySlider.setBounds(area.removeFromTop(28).withTrimmedLeft(48));
    area.removeFromTop(8);

    gainSlider.setBounds(area.removeFromTop(28).withTrimmedLeft(48));
    area.removeFromTop(12);

    deviceSelector.setBounds(area);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    logAudioDeviceStatus();
}

void MainComponent::logAudioDeviceStatus()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        juce::Logger::writeToLog("Audio device opened: " + device->getName()
            + " | " + juce::String(device->getCurrentSampleRate(), 0) + " Hz"
            + " | buffer " + juce::String(device->getCurrentBufferSizeSamples()) + " samples"
            + " | out channels "
            + juce::String(device->getActiveOutputChannels().countNumberOfSetBits()));
    else
        juce::Logger::writeToLog("Audio device: none open");
}

} // namespace looper
