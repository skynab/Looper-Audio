#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <string>

#include "engine/AudioEngine.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/Song.h"

#include "ArrangementView.h"
#include "DockRegion.h"
#include "FileBrowserPanel.h"
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
                            private juce::MenuBarModel,
                            public juce::DragAndDropContainer
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
    void                   importAudioToNewTrack();
    void                   importAudioFileAtBeat(const juce::File& file, double startBeats, int targetTrackIndex = -1);
    void                   previewAudioFile(const juce::File& file);
    void                   toggleRecording();
    void                   finishRecordingIfReady();
    juce::File             recordingsDirectory() const;
    void                   selectNewlyAddedTrack(int newTrackIndex);
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
    void                   movePanelBetweenRegions(const juce::String& panelName, DockRegion& target);
    void                   layoutMixerView();
    void                   layoutArrangeTab();
    void                   layoutEditTab();
    int                    trackCount() const;

    engine::AudioEngine         engine_;
    model::History<model::Song> history_;
    int                         selectedTrackIndex_ = 0;
    int                         selectedClipIndex_  = 0;
    bool                        recordAutomation_   = false;
    bool                        awaitingRecordedTake_ = false;

    juce::MenuBarComponent          menuBar_;

    // Resizable workspace: a Files region, a transport sidebar, then two more
    // dockable regions side by side — each region a tab group, separated by
    // draggable dividers. Panels (Files, Arrange, Edit, Mixer) start out split
    // across the regions so arrangement and mixer tools are visible at once;
    // dragging a tab header onto another region moves that panel there.
    DockRegion                        dockRegionFiles_, dockRegionA_, dockRegionB_;
    FileBrowserPanel                  fileBrowser_;
    juce::Component                   leftPane_;
    juce::StretchableLayoutManager    paneLayout_;
    juce::StretchableLayoutResizerBar paneResizerFiles_ { &paneLayout_, 1, true };
    juce::StretchableLayoutResizerBar paneResizer_      { &paneLayout_, 3, true };
    juce::StretchableLayoutResizerBar paneResizer2_     { &paneLayout_, 5, true };

    juce::TextButton   playButton     { "Play" };
    juce::TextButton   stopButton     { "Stop" };
    juce::TextButton   recordButton   { "Record" };
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
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
