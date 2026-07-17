#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "engine/AudioEngine.h"
#include "engine/TempoMap.h"

namespace looper
{
/**
    Phase 1 UI. No longer an AudioAppComponent — it owns a headless AudioEngine
    and interacts with it only through the engine's thread-safe surface: posting
    commands, and reading published atomics (playhead, meters) on a timer.
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
    void post(engine::EngineCommand::Type type, double a = 0.0, double b = 0.0);

    engine::AudioEngine engine_;

    juce::TextButton   playButton   { "Play" };
    juce::TextButton   stopButton   { "Stop" };
    juce::ToggleButton loopButton   { "Loop" };
    juce::ToggleButton sourceButton { "Test source (sine)" };

    juce::Slider tempoSlider, freqSlider, gainSlider, masterSlider;
    juce::Label  tempoLabel  { {}, "Tempo" };
    juce::Label  freqLabel   { {}, "Freq" };
    juce::Label  gainLabel   { {}, "Src gain" };
    juce::Label  masterLabel { {}, "Master" };
    juce::Label  positionLabel;

    juce::AudioDeviceSelectorComponent deviceSelector;

    engine::TempoMap     uiTempoMap_;              // mirror for bars/beats display
    float                meterLevel_[2] { 0.0f, 0.0f };
    juce::Rectangle<int> meterArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
