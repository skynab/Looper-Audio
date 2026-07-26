#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include <string>

#include "engine/AudioEngine.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/Song.h"

#include "ArrangementView.h"
#include "DockRegion.h"
#include "DrumKitEditor.h"
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
    void                   importMidiFileDialog();
    void                   exportMidiFileDialog();
    void                   setProjectRootFolderDialog();
    void                   toggleRecording();
    void                   finishRecordingIfReady();
    juce::File             recordingsDirectory() const;
    void                   selectNewlyAddedTrack(int newTrackIndex);
    void                   addTrack();
    void                   addDrumTrack();
    void                   assignDrumSample(int padIndex, const juce::File& file);
    void                   syncEngineTracks();
    void                   refreshPianoRollForSelected();
    void                   previewNote(int noteNumber);
    void                   updateDelayControls();
    void                   updateFilterControls();
    void                   updateReverbControls();
    void                   updateSendBusControls();
    void                   updateSendBusEffectVisibility();
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
    void                   layoutMainWorkspaceArea();
    void                   movePanelBetweenRegions(const juce::String& panelName, DockRegion& target);
    void                   loadDockLayout();
    void                   saveDockLayout();
    void                   layoutMixerView();
    void                   layoutMasterPanel();
    void                   layoutArrangeTab();
    void                   layoutEditTab();
    int                    trackCount() const;

    engine::AudioEngine         engine_;
    model::History<model::Song> history_;
    int                         selectedTrackIndex_ = 0;
    int                         selectedClipIndex_  = 0;
    bool                        recordAutomation_   = false;
    bool                        awaitingRecordedTake_ = false;

    // App-level preferences (not project data): which panel lives in which
    // dock region, and the file browser's user bookmarks. Saved on the
    // panel-move/bookmark-change that produces them, not the project.
    juce::PropertiesFile settings_;

    juce::MenuBarComponent          menuBar_;

    // Resizable workspace: a horizontal row of dockable regions (Files,
    // Transport, two more side by side) inside mainWorkspaceArea_, with a
    // fifth region for the on-screen keyboard stacked below it — every
    // region a tab group, separated by draggable dividers. Panels (Files,
    // Transport, Arrange, Edit, Mixer, Keyboard) start out split across the
    // regions so every tool is visible at once; dragging a tab header onto
    // another region moves that panel there, regardless of which of the
    // two splits (horizontal row or the outer vertical one) it's in.
    DockRegion                        dockRegionFiles_, dockRegionTransport_, dockRegionA_, dockRegionB_;
    DockRegion                        dockRegionKeyboard_;
    FileBrowserPanel                  fileBrowser_;
    CallbackComponent                 leftPane_;
    CallbackComponent                 mainWorkspaceArea_; // holds the horizontal row above
    juce::StretchableLayoutManager    paneLayout_;         // the horizontal row
    juce::StretchableLayoutResizerBar paneResizerFiles_ { &paneLayout_, 1, true };
    juce::StretchableLayoutResizerBar paneResizer_      { &paneLayout_, 3, true };
    juce::StretchableLayoutResizerBar paneResizer2_     { &paneLayout_, 5, true };
    juce::StretchableLayoutManager    outerLayout_; // vertical: mainWorkspaceArea_ over dockRegionKeyboard_
    juce::StretchableLayoutResizerBar outerResizer_ { &outerLayout_, 1, false };

    juce::TextButton   playButton     { "Play" };
    juce::TextButton   stopButton     { "Stop" };
    juce::TextButton   recordButton   { "Record" };
    juce::TextButton   addTrackButton { "Add Track" };
    juce::TextButton   addDrumTrackButton_ { "Add Drum" };
    juce::TextButton   toggleMasterPanelButton_ { "Hide Master" };
    bool               masterPanelVisible_ = true;
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
    juce::ComboBox     sendEffectTypeBox_;
    juce::Slider       sendRoomSlider, sendDampSlider; // shown when the send bus effect is Reverb
    juce::Slider       sendDelayTimeSlider, sendDelayFbSlider; // shown when it's Delay
    juce::Slider       sendReturnSlider;
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
    DrumKitEditor                      drumKitEditor_;
    PianoRoll                          pianoRoll_;

    CallbackComponent                  arrangeTab_;
    juce::Viewport                     arrangementViewport_;
    ArrangementView                    arrangementView_;
    juce::TextButton                   zoomInButton_   { "+" };
    juce::TextButton                   zoomOutButton_  { "-" };
    juce::TextButton                   addClipButton_  { "Add Clip" };

    CallbackComponent                  mixerView_;
    CallbackComponent                  masterPanel_; // collapsible via toggleMasterPanelButton_
    juce::OwnedArray<MixerStrip>       trackStrips_;
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
