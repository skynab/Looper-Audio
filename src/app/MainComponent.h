#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "engine/AudioEngine.h"
#include "engine/GuitarTone.h"
#include "engine/TempoMap.h"
#include "model/History.h"
#include "model/PresetSerialization.h"
#include "model/Song.h"

#include "ArrangementView.h"
#include "DockWorkspace.h"
#include "DrumsPane.h"
#include "EffectChainPanel.h"
#include "EqCurveView.h"
#include "FretboardPane.h"
#include "FileBrowserPanel.h"
#include "LevelMeter.h"
#include "MixerStrip.h"
#include "PianoRoll.h"
#include "PluginEditorWindow.h"
#include "ChordStamp.h"
#include "ClipLengthRepair.h"
#include "DragCommit.h"
#include "TrackSelection.h"
#include "SessionView.h"
#include "TrackColours.h"
#include "StatusBanner.h"
#include "SynthEditor.h"

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

    /** True while the document differs from the file it came from. */
    bool hasUnsavedChanges() const;

    /** Runs @p onProceed once it's safe to discard the current document,
        offering to save first if there's anything to lose. Public because
        quitting has to ask too — see LooperAudioApplication::systemRequestedQuit
        — and every destructive path must ask the same question the same way. */
    void confirmDiscardChanges(std::function<void()> onProceed);

    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void logAudioDeviceStatus();
    void updateLoopRegion();
    double loopEndBeats() const;
    void stopAtEndOfArrangement();
    void seekToBeat(double beat);
    void stepByBars(int bars);
    double songEndBeats() const;
    void chooseFile();
    void showStatus(const juce::String& message);
    void showError(const juce::String& message);
    void showBusy(const juce::String& message);

    /** Turns a plain juce::Slider into one whose drags are undoable, the same
        "rewind to where the drag started, commit the final value as one edit"
        technique the mixer faders use — but written generically so a slider
        that lives directly in MainComponent (the master panel's) doesn't need
        its own Fader-style enum and dedicated begin/end methods the way a
        reusable component like MixerStrip does. @p read/@p write are the get
        and set for whichever model::Song field the slider controls. */
    void wireUndoableSlider(juce::Slider& slider, juce::String label,
                            std::function<float(const model::Song&)> read,
                            std::function<void(model::Song&, float)> write);
    void post(engine::EngineCommand::Type type, double a = 0.0, double b = 0.0);

    void                   editPattern(const engine::Pattern& pattern);
    void                   refreshFromModel();
    const engine::Pattern& currentPattern() const;
    void                   newProject();
    void                   createEmptyProject();
    void                   saveProject(std::function<void(bool saved)> onDone = {});
    void                   saveProjectAs(std::function<void(bool saved)> onDone = {});
    bool                   writeProjectTo(const juce::File& file);
    void                   openProject();
    void                   chooseProjectToOpen();
    void                   updateWindowTitle();
    void                   bounceProject();
    void                   showAudioSettings();
    void                   importAudioToNewTrack();
    void                   importAudioFileAtBeat(const juce::File& file, double startBeats, int targetTrackIndex = -1);
    void                   previewAudioFile(const juce::File& file);
    void                   importMidiFileDialog();
    void                   exportMidiFileDialog();
    void                   setProjectRootFolderDialog();
    void                   repairRecordedClipLengths();
    void                   toggleRecording();
    void                   finishRecordingIfReady();
    juce::File             recordingsDirectory() const;
    juce::File             presetsDirectory() const;
    void                   refreshPresetList();
    void                   savePresetDialog();
    void                   applyPreset(int index);
    void                   deletePresetAt(int index);
    void                   seedFactoryPresets();
    juce::File             factoryDrumKitDirectory() const;
    void                   seedFactoryDrumKit();
    model::DrumKit         defaultDrumKitWithFactorySamples() const;
    model::Song            makeStarterSong() const;
    void                   selectTrackAndRefreshAll(int newTrackIndex);
    void                   addTrack();
    void                   addDrumTrack();
    void                   addGuitarTrack();
    void                   refreshFretboardForSelected();
    void                   setTrackGuitarSettings(const model::GuitarSettings& settings);
    void                   stampChord(const engine::ChordShape& shape, int fretOffset,
                                      const engine::StrumSettings& strum);
    void                   playChordAtFret(engine::MovableShape shape, int rootString, int fret,
                                           const engine::StrumSettings& strum, bool writeToClip);
    const model::Track*    guitarTrackForChords();
    double                 beatsPerBar() const;
    bool                   commitStampedNotes(const std::vector<engine::Note>& notes,
                                              const juce::String& what, double atBeats);
    void                   assignDrumSample(int padIndex, const juce::File& file);
    void                   setDrumPadMix(int padIndex, const model::DrumPad& pad);
    void                   addDrumPad();
    void                   removeDrumPad(int padIndex);
    int                    selectedDrumTrackIndex() const;
    void                   syncEngineTracks();
    void                   refreshPianoRollForSelected();
    void                   refreshSynthEditorForSelected();
    void                   refreshDrumsPaneForSelected();
    void                   refreshEffectChainForSelected();
    void                   addEffectSlot(model::EffectKind kind, const model::PluginRef& plugin);
    void                   removeEffectSlot(int slotIndex);
    void                   moveEffectSlot(int slotIndex, int delta);
    void                   setEffectSlotBypass(int slotIndex, bool enabled);
    void                   setEffectSlotParams(const model::EffectSlot& slot, int slotIndex);
    void                   scanForPlugins();
    void                   openPluginEditor(int slotIndex);
    void                   closePluginEditors();
    void                   refreshSessionView();
    void                   addSessionScene();
    void                   deleteSessionScene(int sceneIndex);
    void                   captureClipIntoSession(int trackIndex, int sceneIndex);
    void                   setTrackSynthSettings(const model::SynthSettings& settings);
    void                   previewNote(int noteNumber);
    void                   previewChord(const std::vector<engine::Note>& notes);
    void                   updateDelayControls();
    void                   updateFilterControls();
    void                   updateReverbControls();
    void                   updateEqControls();
    void                   updateSendBusControls();
    void                   updateSendBusEffectVisibility();
    void                   updateMixerStrips();
    void                   beginFaderDrag(int trackIndex, MixerStrip::Fader fader);
    void                   endFaderDrag(int trackIndex, MixerStrip::Fader fader);
    void                   beginEffectSlotParamsDrag(int slotIndex);
    void                   endEffectSlotParamsDrag(int slotIndex);
    void                   beginSynthSettingsDrag();
    void                   endSynthSettingsDrag();
    void                   beginGuitarSettingsDrag();
    void                   endGuitarSettingsDrag();
    void                   setTrackGain(int index, float gainDb);
    void                   setTrackMuted(int index, bool muted);
    void                   setTrackSolo(int index, bool solo);
    void                   setTrackPan(int index, float pan);
    void                   setTrackSendLevel(int index, float level);
    void                   selectTrack(int index);
    void                   selectTrackAndClip(int trackIndex, int clipIndex);
    void                   addClipToSelectedTrack();
    void                   showGenerateLoopDialog();
    void                   setClipLength(int trackIndex, int clipIndex, double newLengthBeats);
    void                   copyNotes();
    void                   pasteNotes();
    void                   copyClip();
    void                   pasteClip();
    void                   copyTrack();
    void                   pasteTrack();
    void                   duplicateTrackAt(int trackIndex);
    void                   duplicateClip();
    bool                   hasSelectedClip() const;
    void                   deleteSelectedClip();
    void                   deleteSelectedTrack();
    void                   deleteTrackAt(int trackIndex);
    void                   renameSelectedTrack();
    void                   renameTrackAt(int trackIndex);
    void                   showTrackSettingsMenu(int trackIndex);
    void                   setTrackColour(int trackIndex, unsigned int argb);
    void                   setTrackType(int trackIndex, model::TrackType newType);
    void                   moveClipToTrack(int srcTrackIndex, int clipIndex, int destTrackIndex, double newStartBeats);
    void                   applyGuitarTone(engine::GuitarTone tone);
    void                   quantizeNotes(double swingAmount);
    void                   setPatternBars(int bars);
    void                   setTimeSignature(int numerator, int denominator);
    void                   updateTimeSignatureControls();
    void                   updateBarsControl();
    void                   updateEditingLabel();
    void                   layoutLeftPane();
    void                   applyTransportCollapse();
    int                    panelMenuIndex(const juce::String& name) const;
    void                   togglePanel(int index);
    void                   buildDefaultDockLayout();
    void                   loadDockLayout();
    void                   saveDockLayout();
    void                   layoutMixerView();
    void                   layoutMasterPanel();
    void                   setUpZoomControls(juce::Component& parent, juce::DrawableButton& icon,
                                             juce::Slider& slider, juce::Slider& box,
                                             double minZoom, double maxZoom,
                                             const juce::String& tooltip,
                                             std::function<void(float)> onZoom);
    void                   setKeysZoom(float zoom);
    void                   updateKeysZoomControls();
    void                   setKeysTimeZoom(float zoom);
    void                   followKeysPlayhead();
    void                   updateKeysTimeZoomControls();
    void                   setTimelineZoom(float zoom);
    void                   updateZoomControls();
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

    // The file this document came from and will Save over — empty until it has
    // been saved once — plus the state id that was last written, which is what
    // hasUnsavedChanges() compares against. windowTitle_ caches what the title
    // bar already says, so the 30Hz timer only touches it on a real change.
    juce::File         projectFile_;
    unsigned long long savedStateId_ = 0;
    juce::String       windowTitle_;

    // Where a fader was grabbed, so the whole drag can be committed as one
    // undo step when it is released rather than one step per pixel.
    bool               faderDragging_  = false;
    int                faderDragTrack_ = -1;
    MixerStrip::Fader  faderDragWhich_ = MixerStrip::Fader::Gain;
    float              faderDragFrom_  = 0.0f;

    // Where an effect slot's parameters were before a drag on one of its
    // controls started, so the whole gesture can commit as one undo step —
    // same reasoning as the fader-drag members above, but for a whole
    // model::EffectSlot rather than one float (see commitStructDrag).
    bool              effectSlotDragging_ = false;
    int               effectSlotDragTrack_ = -1;
    int               effectSlotDragIndex_ = -1;
    model::EffectSlot effectSlotDragFrom_;

    // Same technique again, for the Synth pane's settings — see
    // beginSynthSettingsDrag/endSynthSettingsDrag.
    bool                  synthSettingsDragging_ = false;
    int                   synthSettingsDragTrack_ = -1;
    model::SynthSettings  synthSettingsDragFrom_;

    // Same technique again, for the fretboard's settings — see
    // beginGuitarSettingsDrag/endGuitarSettingsDrag.
    bool                   guitarSettingsDragging_ = false;
    int                    guitarSettingsDragTrack_ = -1;
    model::GuitarSettings  guitarSettingsDragFrom_;

    // The preset list SynthEditor is currently showing, in the same order —
    // presetBox_'s indices are indices into this. Reloaded from disk by
    // refreshPresetList() whenever a preset is saved or deleted.
    std::vector<juce::File> presetFiles_;

    juce::MenuBarComponent          menuBar_;

    // Every tooltip in the app was dead text until this existed: JUCE only
    // shows them while some TooltipWindow is alive to draw them.
    juce::TooltipWindow             tooltips_;

    // Transient messages. A child of this component rather than of any pane,
    // so collapsing or closing a pane can't hide what the app is telling you.
    StatusBanner                    status_;

    // The whole dockable workspace: a tree of tab groups the user arranges by
    // dragging tabs (onto a region's middle to add a tab there, onto an edge
    // to split it). See DockWorkspace; the default arrangement this app ships
    // with is built in buildDefaultDockLayout().
    DockWorkspace      workspace_;
    FileBrowserPanel   fileBrowser_;
    CallbackComponent  leftPane_; // the transport controls (Play/Stop/...), a panel like any other

    // Playback transport, left to right. Play/pause is one toggle rather than
    // two buttons; the old separate Stop is gone, since pausing and returning
    // to the start are now distinct controls (pause, and first-frame).
    juce::DrawableButton firstFrameButton    { "First",    juce::DrawableButton::ImageFitted };
    juce::DrawableButton previousFrameButton { "Previous", juce::DrawableButton::ImageFitted };
    juce::DrawableButton playPauseButton     { "PlayPause", juce::DrawableButton::ImageFitted };
    juce::DrawableButton nextFrameButton     { "Next",     juce::DrawableButton::ImageFitted };
    juce::DrawableButton lastFrameButton     { "Last",     juce::DrawableButton::ImageFitted };
    juce::DrawableButton recordButton { "Record", juce::DrawableButton::ImageFitted };
    juce::TextButton   addTrackButton { "Add Track" };
    juce::TextButton   addDrumTrackButton_ { "Add Drum" };
    juce::TextButton   addGuitarTrackButton_ { "Add Guitar" };
    juce::ToggleButton loopButton      { "Loop" };
    // Collapses the transport pane to its first row, so the pane can be
    // dragged down to a single strip when the readouts aren't wanted.
    juce::TextButton   collapseTransportButton_;
    bool               transportCollapsed_ = false;
    juce::ToggleButton metronomeButton { "Click" };
    juce::ComboBox     countInBox_;
    juce::ComboBox     timeSigBox_;
    juce::Label        timeSigLabel_;

    juce::Slider       tempoSlider, masterSlider;
    juce::ToggleButton filterButton { "Filter" };
    juce::ComboBox     filterModeBox_;
    juce::Slider       filterCutoffSlider, filterResoSlider;
    juce::ToggleButton delayButton { "Delay" };
    juce::Slider       delayTimeSlider, delayFbSlider, delayMixSlider;
    juce::ToggleButton reverbButton { "Reverb" };
    juce::Slider       reverbRoomSlider, reverbDampSlider, reverbMixSlider;
    juce::ToggleButton eqButton { "EQ" };
    juce::Slider       eqBassSlider, eqMidSlider, eqTrebleSlider;
    EqCurveView        eqCurveView_;
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
    EffectChainPanel                   effectChain_;
    juce::OwnedArray<PluginEditorWindow> pluginWindows_;
    SessionView                        sessionView_;
    FretboardPane                      fretboard_; // ditto — see refreshTrackEffectsForSelected

    CallbackComponent                  arrangeTab_;
    juce::Viewport                     arrangementViewport_;
    ArrangementView                    arrangementView_;
    // Timeline zoom: a magnifying glass labelling a slider, with an editable
    // multiplier beside it. Replaced a pair of unlabelled +/- buttons.
    juce::DrawableButton               zoomIcon_ { "Zoom", juce::DrawableButton::ImageFitted };
    juce::Slider                       zoomSlider_;
    juce::Slider                       zoomBox_;

    // The same control for the keys pane, zooming the pitch axis.
    juce::DrawableButton               keysZoomIcon_ { "Zoom", juce::DrawableButton::ImageFitted };
    juce::Slider                       keysZoomSlider_;
    juce::Slider                       keysZoomBox_;

    // ...and again for the time axis, which scrolls in this viewport once the
    // grid is wider than the pane.
    juce::DrawableButton               keysTimeZoomIcon_ { "Zoom", juce::DrawableButton::ImageFitted };
    juce::Slider                       keysTimeZoomSlider_;
    juce::Slider                       keysTimeZoomBox_;
    juce::Viewport                     keysViewport_;
    juce::ToggleButton                 keysFollowButton_;
    juce::TextButton                   addClipButton_       { "Add Clip" };
    juce::TextButton                   generateLoopButton_  { "Generate Loop..." };

    CallbackComponent                  mixerView_;
    CallbackComponent                  masterPanel_; // own top-level dock tab; see layoutMasterPanel()
    juce::OwnedArray<MixerStrip>       trackStrips_;
    std::unique_ptr<juce::FileChooser> chooser_;

    engine::TempoMap uiTempoMap_;

    // Advanced per stamp so two identical chords humanise differently —
    // a repeated strum that lands identically is the thing humanising is
    // meant to avoid.
    unsigned int chordStampSeed_ = 1;

    // An app-level clipboard holding model values, deliberately not the system
    // clipboard: pasting between two running copies of the app isn't worth a
    // serialization format yet. Notes and clips are kept apart so the Edit
    // menu's commands can say exactly what they act on, rather than depending
    // on which pane happens to have focus.
    std::vector<engine::Note> noteClipboard_;
    std::vector<model::Clip>  clipClipboard_;

    // Its own buffer rather than sharing the clip one: pasting a track when a
    // clip was copied, or the reverse, is the kind of guess that loses work.
    std::optional<model::Track> trackClipboard_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
