#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

#include "engine/AudioEngine.h"
#include "engine/TempoMap.h"

#include "LevelMeter.h"
#include "PianoRoll.h"

namespace looper
{
/**
    Phase 2 UI. Owns a headless AudioEngine and interacts with it only through the
    engine's thread-safe surface: posting commands, loading files, submitting
    patterns, and reading published atomics (playhead, meters) on a timer.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void logAudioDeviceStatus();
    void updateLoopRegion();
    void chooseFile();
    void post(engine::EngineCommand::Type type, double a = 0.0, double b = 0.0);

    engine::AudioEngine engine_;

    juce::TextButton   playButton  { "Play" };
    juce::TextButton   stopButton  { "Stop" };
    juce::TextButton   loadButton  { "Load audio..." };
    juce::TextButton   clearButton { "Clear notes" };
    juce::ToggleButton loopButton  { "Loop" };

    juce::Slider tempoSlider, masterSlider;
    juce::Label  tempoLabel  { {}, "Tempo" };
    juce::Label  masterLabel { {}, "Master" };
    juce::Label  positionLabel, clipLabel;

    juce::AudioDeviceSelectorComponent deviceSelector;
    juce::MidiKeyboardComponent        keyboard_ { engine_.keyboardState(),
                                                   juce::MidiKeyboardComponent::horizontalKeyboard };
    LevelMeter                         meter_;
    PianoRoll                          pianoRoll_;
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_; // mirror for bars/beats display

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
