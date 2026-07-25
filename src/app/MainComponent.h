#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

#include "engine/AudioEngine.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/Song.h"

#include "ArrangementView.h"
#include "LevelMeter.h"
#include "MixerStrip.h"
#include "PianoRoll.h"

namespace looper
{
/**
    A generic tab-content component that forwards resized() to a callback. Used
    for the mixer tab, whose children (channel strips, master strip) need
    repositioning whenever JUCE assigns it new bounds — on the initial layout, a
    window resize, or when the TabbedComponent switches to it.
*/
class CallbackComponent final : public juce::Component
{
public:
    std::function<void()> onResized;
    void resized() override { if (onResized) onResized(); }
};

/**
    Phase 3 UI. Owns the project document (a Song under an undo History) and a
    headless AudioEngine. The document may hold several instrument tracks; the
    piano roll edits the selected one, and the mixer tab shows a channel strip per
    track. All edits go through the history (undo/redo) and are mirrored into the
    engine's fixed track pool.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener,
                            private juce::MenuBarModel
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

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
    void                   newProject();
    void                   saveProject();
    void                   openProject();
    void                   bounceProject();
    void                   showAudioSettings();
    void                   addTrack();
    void                   syncEngineTracks();
    void                   refreshPianoRollForSelected();
    void                   updateDelayControls();
    void                   updateFilterControls();
    void                   updateReverbControls();
    void                   updateSendBusControls();
    void                   updateMixerStrips();
    void                   setTrackGain(int index, float gainDb);
    void                   setTrackMuted(int index, bool muted);
    void                   setTrackSolo(int index, bool solo);
    void                   setTrackSendLevel(int index, float level);
    void                   selectTrack(int index);
    void                   selectTrackAndClip(int trackIndex, int clipIndex);
    void                   addClipToSelectedTrack();
    void                   updateEditingLabel();
    void                   layoutLeftPane();
    void                   layoutRightPane();
    void                   layoutMixerView();
    void                   layoutArrangeTab();
    void                   layoutEditTab();
    int                    trackCount() const;

    engine::AudioEngine         engine_;
    model::History<model::Song> history_;
    int                         selectedTrackIndex_ = 0;
    int                         selectedClipIndex_  = 0;
    bool                        recordAutomation_   = false;

    juce::MenuBarComponent          menuBar_;

    // Resizable two-pane workspace: a controls sidebar and an arrange/edit +
    // keyboard pane, separated by a draggable divider.
    juce::Component                 leftPane_, rightPane_;
    juce::StretchableLayoutManager  paneLayout_;
    juce::StretchableLayoutResizerBar paneResizer_ { &paneLayout_, 1, true };

    juce::TextButton   playButton     { "Play" };
    juce::TextButton   stopButton     { "Stop" };
    juce::TextButton   addTrackButton { "Add Track" };
    juce::ToggleButton loopButton      { "Loop" };

    juce::Slider       tempoSlider, masterSlider;
    juce::ToggleButton filterButton { "Filter" };
    juce::ComboBox     filterModeBox_;
    juce::Slider       filterCutoffSlider, filterResoSlider;
    juce::ToggleButton delayButton { "Delay" };
    juce::Slider       delayTimeSlider, delayFbSlider, delayMixSlider;
    juce::ToggleButton reverbButton { "Reverb" };
    juce::Slider       reverbRoomSlider, reverbDampSlider, reverbMixSlider;
    juce::ToggleButton sendBusButton { "Send FX" };
    juce::Slider       sendRoomSlider, sendDampSlider, sendReturnSlider;
    juce::ToggleButton autoRecButton   { "Rec Auto" };
    juce::TextButton   autoClearButton { "Clr Auto" };
    juce::Label        tempoLabel  { {}, "Tempo" };
    juce::Label        masterLabel { {}, "Master" };
    juce::Label  positionLabel, clipLabel;

    juce::MidiKeyboardComponent        keyboard_ { engine_.keyboardState(),
                                                   juce::MidiKeyboardComponent::horizontalKeyboard };
    LevelMeter                         meter_;

    CallbackComponent                  editTab_;
    juce::Label                        editingLabel_;
    PianoRoll                          pianoRoll_;

    CallbackComponent                  arrangeTab_;
    juce::Viewport                     arrangementViewport_;
    ArrangementView                    arrangementView_;
    juce::TextButton                   zoomInButton_   { "+" };
    juce::TextButton                   zoomOutButton_  { "-" };
    juce::TextButton                   addClipButton_  { "Add Clip" };

    CallbackComponent                  mixerView_;
    juce::OwnedArray<MixerStrip>       trackStrips_;
    juce::TabbedComponent              tabs_ { juce::TabbedButtonBar::TabsAtTop };
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
