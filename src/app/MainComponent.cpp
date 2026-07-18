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
    loadButton.onClick  = [this] { chooseFile(); };
    clearButton.onClick = [this] { pianoRoll_.clear(); };

    for (auto* b : { &playButton, &stopButton, &loadButton, &clearButton })
        addAndMakeVisible(b);
    addAndMakeVisible(loopButton);

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

    masterSlider.setRange(-60.0, 6.0, 0.1);
    masterSlider.setValue(0.0, juce::dontSendNotification);
    masterSlider.setTextValueSuffix(" dB");
    masterSlider.onValueChange = [this] { post(Cmd::SetMasterGainDb, masterSlider.getValue()); };

    addAndMakeVisible(tempoSlider);
    addAndMakeVisible(masterSlider);
    tempoLabel.attachToComponent(&tempoSlider, true);
    masterLabel.attachToComponent(&masterSlider, true);

    positionLabel.setFont(juce::Font(juce::FontOptions(20.0f)));
    positionLabel.setText("Bar 1  Beat 1   |   0.00 s   |   STOPPED", juce::dontSendNotification);
    addAndMakeVisible(positionLabel);

    clipLabel.setText("No clip loaded", juce::dontSendNotification);
    addAndMakeVisible(clipLabel);

    // ---- pattern / piano roll ----
    pianoRoll_.onChange = [this](const engine::Pattern& p) { engine_.setPattern(p); };
    addAndMakeVisible(pianoRoll_);
    engine_.setPattern(pianoRoll_.pattern()); // seed the engine with the demo pattern

    addAndMakeVisible(meter_);
    addAndMakeVisible(keyboard_);
    addAndMakeVisible(deviceSelector);

    engine_.deviceManager().addChangeListener(this);
    logAudioDeviceStatus();

    setSize(680, 780);
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

void MainComponent::chooseFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load an audio file", juce::File{},
                                                   "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3");

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        if (engine_.loadAudioFile(file))
            clipLabel.setText(engine_.loadedClipName()
                                  + juce::String::formatted("   (%.2f s)", engine_.loadedClipSeconds()),
                              juce::dontSendNotification);
        else
            clipLabel.setText("Could not load: " + file.getFileName(), juce::dontSendNotification);
    });
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
    engine_.pump(); // free retired clips/patterns on the message thread

    const double sampleRate = engine_.sampleRate();
    uiTempoMap_.setSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0);

    const int64_t playhead = engine_.playheadSamples();
    const auto    bb       = uiTempoMap_.barsBeatsFromSamples(playhead);
    const double  seconds  = sampleRate > 0.0 ? (double) playhead / sampleRate : 0.0;

    positionLabel.setText(juce::String::formatted("Bar %d  Beat %d   |   %.2f s   |   %s",
                                                  bb.bar, bb.beat, seconds,
                                                  engine_.isPlaying() ? "PLAYING" : "STOPPED"),
                          juce::dontSendNotification);

    meter_.setLevel(0, engine_.masterPeak(0));
    meter_.setLevel(1, engine_.masterPeak(1));
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
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(12);

    auto row1 = area.removeFromTop(30);
    playButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(12);
    loopButton.setBounds(row1.removeFromLeft(60));
    row1.removeFromLeft(12);
    loadButton.setBounds(row1.removeFromLeft(110));
    row1.removeFromLeft(8);
    clearButton.setBounds(row1.removeFromLeft(100));
    area.removeFromTop(8);

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(4);
    masterSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(8);

    meter_.setBounds(area.removeFromTop(44));
    area.removeFromTop(10);

    // Bottom-anchored: device selector, then the keyboard above it.
    deviceSelector.setBounds(area.removeFromBottom(130));
    area.removeFromBottom(8);
    keyboard_.setBounds(area.removeFromBottom(64));
    area.removeFromBottom(10);

    // The piano roll takes the flexible middle.
    pianoRoll_.setBounds(area);
}

} // namespace looper
