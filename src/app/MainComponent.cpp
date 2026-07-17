#include "MainComponent.h"

#include <cmath>

namespace looper
{
using Cmd = engine::EngineCommand::Type;

MainComponent::MainComponent()
    : deviceSelector(engine_.deviceManager(),
                     0, 0,   // min/max audio inputs
                     0, 2,   // min/max audio outputs
                     false,  // show MIDI inputs
                     false,  // show MIDI outputs
                     false,  // stereo pair channels
                     false)  // hide advanced options
{
    // ---- transport ----
    playButton.onClick = [this] { post(Cmd::SetPlaying, 1.0); };
    stopButton.onClick = [this] { post(Cmd::SetPlaying, 0.0); post(Cmd::Seek, 0.0); };
    loopButton.onClick = [this]
    {
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        updateLoopRegion();
    };
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(loopButton);

    // ---- test source ----
    sourceButton.onClick = [this] { post(Cmd::SetSourceEnabled, sourceButton.getToggleState() ? 1.0 : 0.0); };
    addAndMakeVisible(sourceButton);

    // ---- sliders ----
    tempoSlider.setRange(40.0, 240.0, 0.1);
    tempoSlider.setValue(120.0, juce::dontSendNotification);
    tempoSlider.setTextValueSuffix(" bpm");
    tempoSlider.onValueChange = [this]
    {
        uiTempoMap_.setTempo(tempoSlider.getValue());
        post(Cmd::SetTempo, tempoSlider.getValue());
        updateLoopRegion();
    };

    freqSlider.setRange(50.0, 2000.0, 1.0);
    freqSlider.setSkewFactorFromMidPoint(440.0);
    freqSlider.setValue(440.0, juce::dontSendNotification);
    freqSlider.setTextValueSuffix(" Hz");
    freqSlider.onValueChange = [this] { post(Cmd::SetSourceFrequency, freqSlider.getValue()); };

    gainSlider.setRange(-60.0, 0.0, 0.1);
    gainSlider.setValue(-12.0, juce::dontSendNotification);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.onValueChange = [this] { post(Cmd::SetSourceGainDb, gainSlider.getValue()); };

    masterSlider.setRange(-60.0, 6.0, 0.1);
    masterSlider.setValue(0.0, juce::dontSendNotification);
    masterSlider.setTextValueSuffix(" dB");
    masterSlider.onValueChange = [this] { post(Cmd::SetMasterGainDb, masterSlider.getValue()); };

    for (auto* s : { &tempoSlider, &freqSlider, &gainSlider, &masterSlider })
        addAndMakeVisible(s);

    tempoLabel.attachToComponent(&tempoSlider, true);
    freqLabel.attachToComponent(&freqSlider, true);
    gainLabel.attachToComponent(&gainSlider, true);
    masterLabel.attachToComponent(&masterSlider, true);

    positionLabel.setFont(juce::Font(juce::FontOptions(20.0f)));
    positionLabel.setText("Bar 1  Beat 1   |   0.00 s   |   STOPPED", juce::dontSendNotification);
    addAndMakeVisible(positionLabel);

    addAndMakeVisible(deviceSelector);

    engine_.deviceManager().addChangeListener(this);
    logAudioDeviceStatus();

    setSize(600, 540);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine_.deviceManager().removeChangeListener(this);
}

void MainComponent::post(engine::EngineCommand::Type type, double a, double b)
{
    engine_.postCommand({ type, a, b });
}

void MainComponent::updateLoopRegion()
{
    const double sampleRate = engine_.sampleRate();
    if (sampleRate <= 0.0)
        return;

    uiTempoMap_.setSampleRate(sampleRate);
    const auto barSamples = (int64_t) std::llround(uiTempoMap_.quartersPerBar() * uiTempoMap_.samplesPerBeat());
    post(Cmd::SetLoopRegion, 0.0, (double) (barSamples * 4)); // 4-bar loop
}

void MainComponent::timerCallback()
{
    const double sampleRate = engine_.sampleRate();
    uiTempoMap_.setSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0);

    const int64_t playhead = engine_.playheadSamples();
    const auto    bb       = uiTempoMap_.barsBeatsFromSamples(playhead);
    const double  seconds  = sampleRate > 0.0 ? (double) playhead / sampleRate : 0.0;

    positionLabel.setText(juce::String::formatted("Bar %d  Beat %d   |   %.2f s   |   %s",
                                                  bb.bar, bb.beat, seconds,
                                                  engine_.isPlaying() ? "PLAYING" : "STOPPED"),
                          juce::dontSendNotification);

    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = engine_.masterPeak(ch);
        meterLevel_[ch] = juce::jmax(peak, meterLevel_[ch] * 0.85f); // fast attack, slow decay
    }

    if (! meterArea_.isEmpty())
        repaint(meterArea_);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    logAudioDeviceStatus();
    updateLoopRegion();
}

void MainComponent::logAudioDeviceStatus()
{
    if (auto* device = engine_.deviceManager().getCurrentAudioDevice())
        juce::Logger::writeToLog("Audio device: " + device->getName()
            + " | " + juce::String(device->getCurrentSampleRate(), 0) + " Hz"
            + " | buffer " + juce::String(device->getCurrentBufferSizeSamples()) + " samples");
    else
        juce::Logger::writeToLog("Audio device: none open");
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    if (meterArea_.isEmpty())
        return;

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRect(meterArea_);

    const int gap  = 4;
    const int barW = (meterArea_.getWidth() - gap) / 2;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto bar = juce::Rectangle<int>(meterArea_.getX() + ch * (barW + gap),
                                        meterArea_.getY(), barW, meterArea_.getHeight());

        const float db   = juce::Decibels::gainToDecibels(meterLevel_[ch], -60.0f);
        const float norm = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);

        auto filled = bar.removeFromBottom(juce::roundToInt((float) bar.getHeight() * norm));
        g.setColour(norm > 0.9f ? juce::Colours::red
                                : (norm > 0.7f ? juce::Colours::yellow : juce::Colours::limegreen));
        g.fillRect(filled);
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(12);

    auto row1 = area.removeFromTop(30);
    playButton.setBounds(row1.removeFromLeft(80));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(80));
    row1.removeFromLeft(14);
    loopButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(14);
    sourceButton.setBounds(row1);
    area.removeFromTop(10);

    positionLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(10);

    auto sliderRow = [&](juce::Slider& s)
    {
        s.setBounds(area.removeFromTop(26).withTrimmedLeft(70));
        area.removeFromTop(4);
    };
    sliderRow(tempoSlider);
    sliderRow(freqSlider);
    sliderRow(gainSlider);
    sliderRow(masterSlider);
    area.removeFromTop(10);

    meterArea_ = area.removeFromTop(56);
    area.removeFromTop(10);

    deviceSelector.setBounds(area);
}

} // namespace looper
