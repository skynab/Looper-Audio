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
#include "DockWorkspace.h"
#include "DrumsPane.h"
#include "FileBrowserPanel.h"
#include "LevelMeter.h"
#include "MixerStrip.h"
#include "PianoRoll.h"
#include "SynthEditor.h"
#include "TrackEffectsPanel.h"

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
    void                   setDrumPadMix(int padIndex, const model::DrumPad& pad);
    void                   addDrumPad();
    void                   removeDrumPad(int padIndex);
    int                    selectedDrumTrackIndex() const;
    void                   syncEngineTracks();
    void                   refreshPianoRollForSelected();
    void                   refreshSynthEditorForSelected();
    void                   refreshDrumsPaneForSelected();
    void                   refreshTrackEffectsForSelected();
    void                   setTrackInsertEffects(const model::FilterSettings& filter,
                                                 const model::DelaySettings& delay,
                                                 const model::ReverbSettings& reverb);
    void                   setTrackSynthSettings(const model::SynthSettings& settings);
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
    void                   setTrackPan(int index, float pan);
    void                   setTrackSendLevel(int index, float level);
    void                   selectTrack(int index);
    void                   selectTrackAndClip(int trackIndex, int clipIndex);
    void                   addClipToSelectedTrack();
    void                   setClipLength(int trackIndex, int clipIndex, double newLengthBeats);
    void                   copyNotes();
    void                   pasteNotes();
    void                   copyClip();
    void                   pasteClip();
    void                   duplicateClip();
    void                   quantizeNotes(double swingAmount);
    void                   setPatternBars(int bars);
    void                   updateBarsControl();
    void                   updateEditingLabel();
    void                   layoutLeftPane();
    void                   buildDefaultDockLayout();
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

    // The whole dockable workspace: a tree of tab groups the user arranges by
    // dragging tabs (onto a region's middle to add a tab there, onto an edge
    // to split it). See DockWorkspace; the default arrangement this app ships
    // with is built in buildDefaultDockLayout().
    DockWorkspace      workspace_;
    FileBrowserPanel   fileBrowser_;
    CallbackComponent  leftPane_; // the transport controls (Play/Stop/...), a panel like any other

    juce::TextButton   playButton     { "Play" };
    juce::TextButton   stopButton     { "Stop" };
    juce::TextButton   recordButton   { "Record" };
    juce::TextButton   addTrackButton { "Add Track" };
    juce::TextButton   addDrumTrackButton_ { "Add Drum" };
    juce::TextButton   toggleMasterPanelButton_ { "Hide Master" };
    bool               masterPanelVisible_ = true;
    juce::ToggleButton loopButton      { "Loop" };
    juce::ToggleButton metronomeButton { "Click" };
    juce::ComboBox     countInBox_;

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
    juce::Label                        barsLabel_ { {}, "Bars" };
    juce::ComboBox                     barsBox_; // pattern length of the open clip
    PianoRoll                          pianoRoll_;

    SynthEditor                        synthEditor_; // its own dock panel — see refreshSynthEditorForSelected
    DrumsPane                          drumsPane_;   // ditto — see refreshDrumsPaneForSelected
    TrackEffectsPanel                  trackEffects_; // ditto — see refreshTrackEffectsForSelected

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

    // An app-level clipboard holding model values, deliberately not the system
    // clipboard: pasting between two running copies of the app isn't worth a
    // serialization format yet. Notes and clips are kept apart so the Edit
    // menu's commands can say exactly what they act on, rather than depending
    // on which pane happens to have focus.
    std::vector<engine::Note> noteClipboard_;
    std::vector<model::Clip>  clipClipboard_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
