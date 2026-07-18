#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

#include "engine/AudioEngine.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/Song.h"

#include "LevelMeter.h"
#include "PianoRoll.h"

namespace looper
{
/**
    Phase 3 UI. Owns the project document (a Song under an undo History) and a
    headless AudioEngine. Pattern edits go through the history so undo/redo work;
    the engine is kept in sync with the document's current pattern.
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
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void logAudioDeviceStatus();
    void updateLoopRegion();
    void chooseFile();
    void post(engine::EngineCommand::Type type, double a = 0.0, double b = 0.0);

    void                   editPattern(const engine::Pattern& pattern);
    void                   refreshFromModel();
    const engine::Pattern& currentPattern() const;

    engine::AudioEngine         engine_;
    model::History<model::Song> history_;

    juce::TextButton   playButton  { "Play" };
    juce::TextButton   stopButton  { "Stop" };
    juce::TextButton   loadButton  { "Load audio..." };
    juce::TextButton   clearButton { "Clear notes" };
    juce::TextButton   undoButton  { "Undo" };
    juce::TextButton   redoButton  { "Redo" };
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

    engine::TempoMap uiTempoMap_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
