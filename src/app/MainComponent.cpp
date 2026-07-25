#include "MainComponent.h"

#include "engine/ClipSlot.h"
#include "engine/OfflineRenderer.h"
#include "model/Serialization.h"

#include <cmath>
#include <vector>

namespace looper
{
using Cmd = engine::EngineCommand::Type;

MainComponent::MainComponent()
{
    menuBar_.setModel(this);
    addAndMakeVisible(menuBar_);

    // Dockable workspace: a Files region, a transport sidebar, then two more
    // tab-group regions side by side, each separated by a draggable divider.
    // Panels start out split across the regions (see below) so file
    // management, arrangement, and mixer tools are all visible at the same
    // time; dragging a tab onto another region moves that panel there.
    addAndMakeVisible(dockRegionFiles_);
    addAndMakeVisible(paneResizerFiles_);
    addAndMakeVisible(leftPane_);
    addAndMakeVisible(paneResizer_);
    addAndMakeVisible(dockRegionA_);
    addAndMakeVisible(paneResizer2_);
    addAndMakeVisible(dockRegionB_);
    paneLayout_.setItemLayout(0, 180, 320, 220);   // dock region (Files): min/max/preferred
    paneLayout_.setItemLayout(1, 8, 8, 8);         // divider: fixed width
    paneLayout_.setItemLayout(2, 220, 380, 260);   // left pane (transport): min/max/preferred
    paneLayout_.setItemLayout(3, 8, 8, 8);         // divider: fixed width
    paneLayout_.setItemLayout(4, 400, -1.0, -1.0); // dock region A (Arrange/Edit): flexible
    paneLayout_.setItemLayout(5, 8, 8, 8);         // divider: fixed width
    paneLayout_.setItemLayout(6, 260, 520, 340);   // dock region B (Mixer): min/max/preferred

    for (auto* region : { &dockRegionFiles_, &dockRegionA_, &dockRegionB_ })
        region->onForeignPanelDropped = [this](const juce::String& name, DockRegion& target)
        {
            movePanelBetweenRegions(name, target);
        };

    // ---- document: one instrument track holding the piano-roll pattern ----
    {
        model::Song song;
        const int trackId = model::addTrack(song, model::TrackType::Instrument, "Synth 1").id;
        model::Clip clip;
        clip.type        = model::ClipType::Instrument;
        clip.pattern     = pianoRoll_.pattern();
        clip.lengthBeats = clip.pattern.lengthBeats;
        model::addClip(song, trackId, clip);
        history_.reset(song);
    }

    // ---- transport ----
    playButton.onClick = [this] { post(Cmd::SetPlaying, 1.0); };
    stopButton.onClick = [this]
    {
        post(Cmd::SetPlaying, 0.0);
        post(Cmd::Seek, 0.0);
        if (awaitingRecordedTake_)
            engine_.stopRecording(); // the transport stopping alone would also
                                     // end the take, but this makes it explicit
    };
    loopButton.onClick = [this]
    {
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        updateLoopRegion();
    };
    recordButton.onClick = [this] { toggleRecording(); };
    recordButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::red);
    leftPane_.addAndMakeVisible(playButton);
    leftPane_.addAndMakeVisible(stopButton);
    leftPane_.addAndMakeVisible(recordButton);
    leftPane_.addAndMakeVisible(loopButton);

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
    leftPane_.addAndMakeVisible(tempoSlider);
    tempoLabel.attachToComponent(&tempoSlider, true);

    positionLabel.setFont(juce::Font(juce::FontOptions(20.0f)));
    positionLabel.setText("Bar 1  Beat 1   |   0.00 s   |   STOPPED", juce::dontSendNotification);
    leftPane_.addAndMakeVisible(positionLabel);

    clipLabel.setText("No clip loaded", juce::dontSendNotification);
    leftPane_.addAndMakeVisible(clipLabel);

    // ==== everything below lives in the Mixer tab ====

    addTrackButton.onClick = [this] { addTrack(); };
    mixerView_.addAndMakeVisible(addTrackButton);

    masterSlider.setRange(-60.0, 6.0, 0.1);
    masterSlider.setValue(0.0, juce::dontSendNotification);
    masterSlider.setTextValueSuffix(" dB");
    masterSlider.onValueChange = [this]
    {
        const float db = (float) masterSlider.getValue();
        post(Cmd::SetMasterGainDb, db);
        if (recordAutomation_ && engine_.isPlaying())
        {
            const double beat = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
            history_.mutableCurrent().masterGainDb.addPoint(beat, db);
        }
    };
    mixerView_.addAndMakeVisible(masterSlider);
    masterLabel.attachToComponent(&masterSlider, true);

    // ---- master filter (stored in the document) ----
    filterButton.onClick = [this]
    {
        const bool on = filterButton.getToggleState();
        history_.mutableCurrent().filter.enabled = on;
        engine_.setMasterFilterEnabled(on);
    };
    mixerView_.addAndMakeVisible(filterButton);

    filterModeBox_.addItem("Low-pass", 1);
    filterModeBox_.addItem("High-pass", 2);
    filterModeBox_.addItem("Band-pass", 3);
    filterModeBox_.setSelectedId(1, juce::dontSendNotification);
    filterModeBox_.onChange = [this]
    {
        const int mode = juce::jmax(0, filterModeBox_.getSelectedId() - 1);
        history_.mutableCurrent().filter.mode = mode;
        engine_.setMasterFilterMode(mode);
    };
    mixerView_.addAndMakeVisible(filterModeBox_);

    filterCutoffSlider.setRange(20.0, 18000.0, 1.0);
    filterCutoffSlider.setSkewFactorFromMidPoint(1000.0);
    filterCutoffSlider.setValue(1000.0, juce::dontSendNotification);
    filterCutoffSlider.setTextValueSuffix(" Hz");
    filterCutoffSlider.onValueChange = [this]
    {
        const float hz = (float) filterCutoffSlider.getValue();
        history_.mutableCurrent().filter.cutoff = hz;
        engine_.setMasterFilterCutoff(hz);
    };
    mixerView_.addAndMakeVisible(filterCutoffSlider);

    filterResoSlider.setRange(0.1, 5.0, 0.01);
    filterResoSlider.setValue(0.707, juce::dontSendNotification);
    filterResoSlider.setTextValueSuffix(" Q");
    filterResoSlider.onValueChange = [this]
    {
        const float q = (float) filterResoSlider.getValue();
        history_.mutableCurrent().filter.resonance = q;
        engine_.setMasterFilterResonance(q);
    };
    mixerView_.addAndMakeVisible(filterResoSlider);

    // ---- master delay (stored in the document, so it saves + restores) ----
    delayButton.onClick = [this]
    {
        const bool on = delayButton.getToggleState();
        history_.mutableCurrent().delay.enabled = on;
        engine_.setMasterDelayEnabled(on);
    };
    mixerView_.addAndMakeVisible(delayButton);

    delayTimeSlider.setRange(20.0, 1000.0, 1.0);
    delayTimeSlider.setValue(300.0, juce::dontSendNotification);
    delayTimeSlider.setTextValueSuffix(" ms");
    delayTimeSlider.onValueChange = [this]
    {
        const float ms = (float) delayTimeSlider.getValue();
        history_.mutableCurrent().delay.timeMs = ms;
        engine_.setMasterDelayTimeMs(ms);
    };
    mixerView_.addAndMakeVisible(delayTimeSlider);

    delayFbSlider.setRange(0.0, 95.0, 1.0);
    delayFbSlider.setValue(35.0, juce::dontSendNotification);
    delayFbSlider.setTextValueSuffix(" %");
    delayFbSlider.onValueChange = [this]
    {
        const float fb = (float) (delayFbSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.feedback = fb;
        engine_.setMasterDelayFeedback(fb);
    };
    mixerView_.addAndMakeVisible(delayFbSlider);

    delayMixSlider.setRange(0.0, 100.0, 1.0);
    delayMixSlider.setValue(30.0, juce::dontSendNotification);
    delayMixSlider.setTextValueSuffix(" %");
    delayMixSlider.onValueChange = [this]
    {
        const float mix = (float) (delayMixSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.mix = mix;
        engine_.setMasterDelayMix(mix);
    };
    mixerView_.addAndMakeVisible(delayMixSlider);

    // ---- master reverb (stored in the document) ----
    reverbButton.onClick = [this]
    {
        const bool on = reverbButton.getToggleState();
        history_.mutableCurrent().reverb.enabled = on;
        engine_.setMasterReverbEnabled(on);
    };
    mixerView_.addAndMakeVisible(reverbButton);

    reverbRoomSlider.setRange(0.0, 100.0, 1.0);
    reverbRoomSlider.setValue(50.0, juce::dontSendNotification);
    reverbRoomSlider.setTextValueSuffix(" room");
    reverbRoomSlider.onValueChange = [this]
    {
        const float v = (float) (reverbRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.roomSize = v;
        engine_.setMasterReverbRoomSize(v);
    };
    mixerView_.addAndMakeVisible(reverbRoomSlider);

    reverbDampSlider.setRange(0.0, 100.0, 1.0);
    reverbDampSlider.setValue(50.0, juce::dontSendNotification);
    reverbDampSlider.setTextValueSuffix(" damp");
    reverbDampSlider.onValueChange = [this]
    {
        const float v = (float) (reverbDampSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.damping = v;
        engine_.setMasterReverbDamping(v);
    };
    mixerView_.addAndMakeVisible(reverbDampSlider);

    reverbMixSlider.setRange(0.0, 100.0, 1.0);
    reverbMixSlider.setValue(30.0, juce::dontSendNotification);
    reverbMixSlider.setTextValueSuffix(" %");
    reverbMixSlider.onValueChange = [this]
    {
        const float v = (float) (reverbMixSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.mix = v;
        engine_.setMasterReverbMix(v);
    };
    mixerView_.addAndMakeVisible(reverbMixSlider);

    // ---- send bus: a shared reverb every track can send into (stored in the document) ----
    sendBusButton.onClick = [this]
    {
        const bool on = sendBusButton.getToggleState();
        history_.mutableCurrent().sendBus.enabled = on;
        engine_.setSendBusEnabled(on);
    };
    mixerView_.addAndMakeVisible(sendBusButton);

    sendRoomSlider.setRange(0.0, 100.0, 1.0);
    sendRoomSlider.setValue(60.0, juce::dontSendNotification);
    sendRoomSlider.setTextValueSuffix(" room");
    sendRoomSlider.onValueChange = [this]
    {
        const float v = (float) (sendRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.roomSize = v;
        engine_.setSendBusRoomSize(v);
    };
    mixerView_.addAndMakeVisible(sendRoomSlider);

    sendDampSlider.setRange(0.0, 100.0, 1.0);
    sendDampSlider.setValue(40.0, juce::dontSendNotification);
    sendDampSlider.setTextValueSuffix(" damp");
    sendDampSlider.onValueChange = [this]
    {
        const float v = (float) (sendDampSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.damping = v;
        engine_.setSendBusDamping(v);
    };
    mixerView_.addAndMakeVisible(sendDampSlider);

    sendReturnSlider.setRange(0.0, 100.0, 1.0);
    sendReturnSlider.setValue(50.0, juce::dontSendNotification);
    sendReturnSlider.setTextValueSuffix(" ret");
    sendReturnSlider.onValueChange = [this]
    {
        const float v = (float) (sendReturnSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.returnLevel = v;
        engine_.setSendBusReturnLevel(v);
    };
    mixerView_.addAndMakeVisible(sendReturnSlider);

    // ---- gain automation: arm, then move the master fader or a track's fader
    // while playing (Rec Auto arms both; Clr Auto clears both, the master lane
    // and the currently selected track's) ----
    autoRecButton.onClick   = [this] { recordAutomation_ = autoRecButton.getToggleState(); };
    autoClearButton.onClick = [this]
    {
        auto& song = history_.mutableCurrent();
        song.masterGainDb.clear();
        if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
            song.tracks[(size_t) selectedTrackIndex_].gainAutomation.clear();
    };
    mixerView_.addAndMakeVisible(autoRecButton);
    mixerView_.addAndMakeVisible(autoClearButton);

    mixerView_.addAndMakeVisible(meter_);

    // ---- per-track channel strips ----
    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip = new MixerStrip();
        strip->onGainChange = [this, i](float db) { setTrackGain(i, db); };
        strip->onMuteChange = [this, i](bool m)   { setTrackMuted(i, m); };
        strip->onSoloChange = [this, i](bool s)   { setTrackSolo(i, s); };
        strip->onSendChange = [this, i](float lv) { setTrackSendLevel(i, lv); };
        strip->onSelect     = [this, i]           { selectTrack(i); };
        trackStrips_.add(strip);
        mixerView_.addAndMakeVisible(strip);
    }

    mixerView_.onResized = [this] { layoutMixerView(); };

    pianoRoll_.onChange = [this](const engine::Pattern& p) { editPattern(p); };

    // ---- edit tab: a header showing which track/clip is open, plus the piano roll ----
    editingLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    editTab_.addAndMakeVisible(editingLabel_);
    editTab_.addAndMakeVisible(pianoRoll_);
    editTab_.onResized = [this] { layoutEditTab(); };

    // ---- arrange tab: a zoomable/scrollable timeline, click to seek ----
    arrangementViewport_.setViewedComponent(&arrangementView_, false);
    arrangeTab_.addAndMakeVisible(arrangementViewport_);

    zoomInButton_.onClick  = [this] { arrangementView_.setZoom(arrangementView_.zoom() * 1.25f); };
    zoomOutButton_.onClick = [this] { arrangementView_.setZoom(arrangementView_.zoom() / 1.25f); };
    addClipButton_.onClick = [this] { addClipToSelectedTrack(); };
    arrangeTab_.addAndMakeVisible(zoomInButton_);
    arrangeTab_.addAndMakeVisible(zoomOutButton_);
    arrangeTab_.addAndMakeVisible(addClipButton_);
    arrangeTab_.onResized = [this] { layoutArrangeTab(); };

    arrangementView_.onSeek = [this](double beat)
    {
        const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
        uiTempoMap_.setSampleRate(sampleRate);
        post(Cmd::Seek, (double) uiTempoMap_.samplesFromPpq(juce::jmax(0.0, beat)));
    };

    arrangementView_.onClipSelected = [this](int trackIndex, int clipIndex)
    {
        selectTrackAndClip(trackIndex, clipIndex);
    };

    arrangementView_.onClipMoved = [this](int trackIndex, int clipIndex, double newStartBeats)
    {
        history_.edit("Move clip", [trackIndex, clipIndex, newStartBeats](model::Song& s)
        {
            if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
                return;
            auto& clips = s.tracks[(size_t) trackIndex].clips;
            if (clipIndex >= 0 && clipIndex < (int) clips.size())
                clips[(size_t) clipIndex].startBeats = juce::jmax(0.0, newStartBeats);
        });

        arrangementView_.setSong(history_.current());
        syncEngineTracks(); // pushes every track's whole clip list, including this move
    };

    // Default docking layout: Files gets its own region, Arrange + Edit share
    // region A, Mixer gets its own region B — so file management, mixer, and
    // arrangement tools are all visible at once out of the box. Drag any
    // tab's header onto another region to move it there instead.
    dockRegionFiles_.addPanel("Files", fileBrowser_);
    dockRegionA_.addPanel("Arrange", arrangeTab_);
    dockRegionA_.addPanel("Edit", editTab_);
    dockRegionA_.showPanel("Edit"); // start on the note editor
    dockRegionB_.addPanel("Mixer", mixerView_);

    fileBrowser_.setRecordingsDirectory(recordingsDirectory());
    fileBrowser_.showDirectory(recordingsDirectory());
    fileBrowser_.onFilePreview = [this](const juce::File& file) { previewAudioFile(file); };

    arrangementView_.onFileDropped = [this](const juce::File& file, double dropBeat)
    {
        importAudioFileAtBeat(file, dropBeat);
    };

    addAndMakeVisible(keyboard_);

    // Mirror the initial document into the engine + UI.
    syncEngineTracks();
    engine_.setArmedTrack(0);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();
    updateEditingLabel();
    updateSendBusControls();

    engine_.deviceManager().addChangeListener(this);
    logAudioDeviceStatus();

    setWantsKeyboardFocus(true);
    setSize(900, 800);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    menuBar_.setModel(nullptr);
    engine_.deviceManager().removeChangeListener(this);
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (topLevelMenuIndex == 0) // File
    {
        menu.addItem(1, "New Project");
        menu.addItem(2, "Open Project...");
        menu.addItem(3, "Save Project...");
        menu.addSeparator();
        menu.addItem(4, "Import Audio...");
        menu.addItem(7, "Import Audio to Track...");
        menu.addItem(5, "Bounce to WAV...");
        menu.addSeparator();
        menu.addItem(6, "Audio Settings...");
    }
    else if (topLevelMenuIndex == 1) // Edit
    {
        menu.addItem(10, "Undo", history_.canUndo());
        menu.addItem(11, "Redo", history_.canRedo());
        menu.addSeparator();
        menu.addItem(12, "Clear Notes");
    }

    return menu;
}

void MainComponent::menuItemSelected(int menuItemID, int)
{
    switch (menuItemID)
    {
        case 1:  newProject(); break;
        case 2:  openProject(); break;
        case 3:  saveProject(); break;
        case 4:  chooseFile(); break; // import audio (preview player)
        case 5:  bounceProject(); break;
        case 6:  showAudioSettings(); break;
        case 7:  importAudioToNewTrack(); break;
        case 10: history_.undo(); refreshFromModel(); break;
        case 11: history_.redo(); refreshFromModel(); break;
        case 12: pianoRoll_.clear(); break;
        default: break;
    }
}

void MainComponent::newProject()
{
    model::Song song;
    const int id = model::addTrack(song, model::TrackType::Instrument, "Synth 1").id;
    model::Clip clip;
    clip.type                = model::ClipType::Instrument;
    clip.lengthBeats         = 4.0;
    clip.pattern.lengthBeats = 4.0;
    model::addClip(song, id, clip);

    history_.reset(song);
    selectedTrackIndex_ = 0;
    tempoSlider.setValue(song.bpm, juce::dontSendNotification);
    uiTempoMap_.setTempo(song.bpm);
    post(Cmd::SetTempo, song.bpm);
    refreshFromModel();
    clipLabel.setText("No clip loaded", juce::dontSendNotification);
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        engine_.deviceManager(), 0, 0, 0, 2, false, false, false, false);
    selector->setSize(500, 420);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle                  = "Audio Settings";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

void MainComponent::post(engine::EngineCommand::Type type, double a, double b)
{
    engine_.postCommand({ type, a, b });
}

int MainComponent::trackCount() const
{
    return (int) history_.current().tracks.size();
}

const engine::Pattern& MainComponent::currentPattern() const
{
    static const engine::Pattern empty;
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return empty;
    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size())
        return empty;
    return track.clips[(size_t) selectedClipIndex_].pattern;
}

void MainComponent::editPattern(const engine::Pattern& pattern)
{
    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;
    history_.edit("Edit notes", [&pattern, trackIdx, clipIdx](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx >= 0 && clipIdx < (int) clips.size())
            clips[(size_t) clipIdx].pattern = pattern;
    });
    syncEngineTracks(); // rebuilds every track's clip list, including this edit
}

void MainComponent::addTrack()
{
    if (trackCount() >= engine_.maxTracks())
        return;

    history_.edit("Add track", [](model::Song& s)
    {
        const auto name = "Synth " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Instrument, name.toStdString()).id;
        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        model::addClip(s, id, clip);
    });

    selectedTrackIndex_ = trackCount() - 1;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Adds a new clip to the currently selected track, positioned 2 beats after
    its last existing clip (or at beat 0 if it has none), and selects it for
    editing. */
void MainComponent::addClipToSelectedTrack()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int trackIdx = selectedTrackIndex_;
    int       newClipIndex = -1;

    history_.edit("Add clip", [trackIdx, &newClipIndex](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& track = s.tracks[(size_t) trackIdx];

        double nextStart = 0.0;
        for (const auto& c : track.clips)
            nextStart = juce::jmax(nextStart, c.startBeats + c.lengthBeats);
        if (! track.clips.empty())
            nextStart += 2.0; // a small gap after the last clip

        model::Clip clip;
        clip.id                  = model::allocateId(s);
        clip.type                = model::ClipType::Instrument;
        clip.startBeats          = nextStart;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        track.clips.push_back(clip);
        newClipIndex = (int) track.clips.size() - 1;
    });

    if (newClipIndex < 0)
        return;

    selectedClipIndex_ = newClipIndex;
    syncEngineTracks();
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

void MainComponent::syncEngineTracks()
{
    const auto& song = history_.current();
    const int   n    = juce::jmin((int) song.tracks.size(), engine_.maxTracks());

    for (int i = 0; i < n; ++i)
    {
        const auto& track = song.tracks[(size_t) i];

        std::vector<engine::ClipSlot> slots;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Instrument)
                continue; // audio clips aren't sequenced

            engine::ClipSlot slot;
            slot.pattern = clip.pattern;
            slot.startBeats = clip.startBeats;
            // A track's only clip keeps looping indefinitely from its start
            // (today's validated "plays until Stop" behaviour); real length
            // gating only applies once a track has more than one clip.
            slot.lengthBeats = track.clips.size() == 1 ? 1.0e9 : clip.lengthBeats;
            slots.push_back(slot);
        }
        engine_.setTrackClips(i, slots);

        // Audio clip -> the track's own audio-clip player. Only the first
        // audio-type clip on a track is used (one audio clip per track, v1 —
        // matches "Import Audio to Track", the only way to create one today).
        std::string audioFile;
        double      audioStartBeats = 0.0;
        for (const auto& clip : track.clips)
        {
            if (clip.type == model::ClipType::Audio && ! clip.audioFile.empty())
            {
                audioFile       = clip.audioFile;
                audioStartBeats = clip.startBeats;
                break;
            }
        }
        if (! audioFile.empty())
        {
            if (audioFile != loadedTrackAudioFile_[(size_t) i])
            {
                // Path changed (or first load): decode it. Expensive, so only
                // done when necessary, not on every document edit.
                loadedTrackAudioFile_[(size_t) i] = audioFile;
                engine_.loadAudioFileForTrack(i, juce::File(audioFile), audioStartBeats);
            }
            else
            {
                // Same file already decoded — just reposition it (e.g. after a drag).
                engine_.setTrackAudioClipStartBeats(i, audioStartBeats);
            }
        }

        engine_.setTrackMuted(i, track.muted);
        engine_.setTrackSolo(i, track.solo);
        engine_.setTrackGainDb(i, track.gainDb);
        engine_.setTrackSendLevel(i, track.sendLevel);
    }
    engine_.setActiveTrackCount(n);
}

void MainComponent::refreshPianoRollForSelected()
{
    pianoRoll_.setPattern(currentPattern());
}

void MainComponent::updateEditingLabel()
{
    const auto& song = history_.current();

    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        editingLabel_.setText("No track selected", juce::dontSendNotification);
        return;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    const auto  name  = track.name.empty() ? ("Track " + juce::String(selectedTrackIndex_ + 1))
                                           : juce::String(track.name);

    juce::String text = "Editing: " + name;
    if (! track.clips.empty())
    {
        text << "   |   Clip " << (selectedClipIndex_ + 1) << " of " << (int) track.clips.size();

        if (selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) track.clips.size()
            && track.clips[(size_t) selectedClipIndex_].type == model::ClipType::Audio)
            text << "  (audio clip — not MIDI-editable)";
    }
    editingLabel_.setText(text, juce::dontSendNotification);
}

void MainComponent::updateMixerStrips()
{
    const auto& song = history_.current();

    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip  = trackStrips_[i];
        const bool active = i < (int) song.tracks.size();
        strip->setVisible(active);

        if (active)
        {
            const auto& track = song.tracks[(size_t) i];
            strip->setTrackName(track.name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(track.name));
            strip->setGainDb(track.gainDb);
            strip->setMuted(track.muted);
            strip->setSoloed(track.solo);
            strip->setSendLevel(track.sendLevel);
        }
        strip->setSelected(i == selectedTrackIndex_);
    }

    layoutMixerView();
}

void MainComponent::updateDelayControls()
{
    const auto& d = history_.current().delay;
    delayButton.setToggleState(d.enabled, juce::dontSendNotification);
    delayTimeSlider.setValue(d.timeMs, juce::dontSendNotification);
    delayFbSlider.setValue(d.feedback * 100.0, juce::dontSendNotification);
    delayMixSlider.setValue(d.mix * 100.0, juce::dontSendNotification);

    engine_.setMasterDelayEnabled(d.enabled);
    engine_.setMasterDelayTimeMs(d.timeMs);
    engine_.setMasterDelayFeedback(d.feedback);
    engine_.setMasterDelayMix(d.mix);
}

void MainComponent::updateFilterControls()
{
    const auto& f = history_.current().filter;
    filterButton.setToggleState(f.enabled, juce::dontSendNotification);
    filterModeBox_.setSelectedId(f.mode + 1, juce::dontSendNotification);
    filterCutoffSlider.setValue(f.cutoff, juce::dontSendNotification);
    filterResoSlider.setValue(f.resonance, juce::dontSendNotification);

    engine_.setMasterFilterEnabled(f.enabled);
    engine_.setMasterFilterMode(f.mode);
    engine_.setMasterFilterCutoff(f.cutoff);
    engine_.setMasterFilterResonance(f.resonance);
}

void MainComponent::updateReverbControls()
{
    const auto& rv = history_.current().reverb;
    reverbButton.setToggleState(rv.enabled, juce::dontSendNotification);
    reverbRoomSlider.setValue(rv.roomSize * 100.0, juce::dontSendNotification);
    reverbDampSlider.setValue(rv.damping * 100.0, juce::dontSendNotification);
    reverbMixSlider.setValue(rv.mix * 100.0, juce::dontSendNotification);

    engine_.setMasterReverbEnabled(rv.enabled);
    engine_.setMasterReverbRoomSize(rv.roomSize);
    engine_.setMasterReverbDamping(rv.damping);
    engine_.setMasterReverbMix(rv.mix);
}

void MainComponent::updateSendBusControls()
{
    const auto& sb = history_.current().sendBus;
    sendBusButton.setToggleState(sb.enabled, juce::dontSendNotification);
    sendRoomSlider.setValue(sb.roomSize * 100.0, juce::dontSendNotification);
    sendDampSlider.setValue(sb.damping * 100.0, juce::dontSendNotification);
    sendReturnSlider.setValue(sb.returnLevel * 100.0, juce::dontSendNotification);

    engine_.setSendBusEnabled(sb.enabled);
    engine_.setSendBusRoomSize(sb.roomSize);
    engine_.setSendBusDamping(sb.damping);
    engine_.setSendBusReturnLevel(sb.returnLevel);
}

void MainComponent::setTrackGain(int index, float gainDb)
{
    // Live tweak: update the current document in place (not a separate undo step).
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.gainDb = gainDb;

        // The same global "Rec Auto" toggle used for master-gain automation
        // also arms per-track gain automation — touch whichever fader you want
        // to automate while it's on.
        if (recordAutomation_ && engine_.isPlaying())
        {
            const double beat = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
            track.gainAutomation.addPoint(beat, gainDb);
        }
    }
    engine_.setTrackGainDb(index, gainDb);
}

void MainComponent::setTrackMuted(int index, bool muted)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
        song.tracks[(size_t) index].muted = muted;
    engine_.setTrackMuted(index, muted);
}

void MainComponent::setTrackSolo(int index, bool solo)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
        song.tracks[(size_t) index].solo = solo;
    engine_.setTrackSolo(index, solo);
}

void MainComponent::setTrackSendLevel(int index, float level)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
        song.tracks[(size_t) index].sendLevel = level;
    engine_.setTrackSendLevel(index, level);
}

void MainComponent::selectTrack(int index)
{
    // A mixer-strip click doesn't know about specific clips, so it defaults to
    // the track's first one.
    selectTrackAndClip(index, 0);
}

void MainComponent::selectTrackAndClip(int trackIndex, int clipIndex)
{
    if (trackIndex < 0 || trackIndex >= trackCount())
        return;

    selectedTrackIndex_ = trackIndex;
    selectedClipIndex_  = clipIndex;
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    updateMixerStrips(); // refreshes the selection highlight
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

void MainComponent::refreshFromModel()
{
    if (selectedTrackIndex_ >= trackCount())
        selectedTrackIndex_ = juce::jmax(0, trackCount() - 1);

    const int clipCount = (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount())
                             ? (int) history_.current().tracks[(size_t) selectedTrackIndex_].clips.size()
                             : 0;
    if (selectedClipIndex_ >= clipCount)
        selectedClipIndex_ = juce::jmax(0, clipCount - 1);

    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();
    updateSendBusControls();
    updateEditingLabel();
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown())
    {
        const int code = key.getKeyCode();
        if (code == 'Z' || code == 'z')
        {
            if (key.getModifiers().isShiftDown())
                history_.redo();
            else
                history_.undo();
            refreshFromModel();
            return true;
        }
        if (code == 'Y' || code == 'y')
        {
            history_.redo();
            refreshFromModel();
            return true;
        }
    }
    return false;
}

void MainComponent::chooseFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load an audio file", juce::File{},
                                                   "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3");

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File{})
            previewAudioFile(file);
    });
}

/** Loads a file into the global preview player (not tied to any track) — used
    by the "Import Audio..." menu item and by double-clicking a file in the
    file-browser pane. */
void MainComponent::previewAudioFile(const juce::File& file)
{
    if (engine_.loadAudioFile(file))
        clipLabel.setText(engine_.loadedClipName()
                              + juce::String::formatted("   (%.2f s)", engine_.loadedClipSeconds()),
                          juce::dontSendNotification);
    else
        clipLabel.setText("Could not load: " + file.getFileName(), juce::dontSendNotification);
}

/** Imports an audio file onto a brand-new Audio track (as its one clip, at
    beat 0), so it actually plays back as part of the mix — unlike "Import
    Audio..." above, which only feeds the disconnected global preview player. */
void MainComponent::importAudioToNewTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        clipLabel.setText("Track limit reached", juce::dontSendNotification);
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser>("Import audio to a new track", juce::File{},
                                                   "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File{})
            importAudioFileAtBeat(file, 0.0);
    });
}

/** Imports a file onto a brand-new Audio track (as its one clip, starting at
    @p startBeats) — the shared machinery behind both "Import Audio to
    Track..." (always beat 0) and dragging a file from the file-browser pane
    onto the arrangement (beat = wherever it was dropped). */
void MainComponent::importAudioFileAtBeat(const juce::File& file, double startBeats)
{
    if (trackCount() >= engine_.maxTracks())
    {
        clipLabel.setText("Track limit reached", juce::dontSendNotification);
        return;
    }

    const auto path = file.getFullPathName().toStdString();
    int        newTrackIndex = -1;

    history_.edit("Import audio track", [&path, &newTrackIndex, startBeats](model::Song& s)
    {
        const auto name = "Audio " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Audio, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(s);
        clip.type        = model::ClipType::Audio;
        clip.startBeats  = juce::jmax(0.0, startBeats);
        clip.lengthBeats = 4.0; // display size only; audio clips don't loop/gate on length yet
        clip.audioFile   = path;
        s.tracks.back().clips.push_back(clip);

        newTrackIndex = (int) s.tracks.size() - 1;
    });

    selectNewlyAddedTrack(newTrackIndex);
    clipLabel.setText("Imported: " + file.getFileName() + "  (new track)", juce::dontSendNotification);
}

void MainComponent::selectNewlyAddedTrack(int newTrackIndex)
{
    if (newTrackIndex < 0)
        return;

    selectedTrackIndex_ = newTrackIndex;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Toggles between arming/starting a take and stopping it. The take doesn't
    finish and turn into a track until finishRecordingIfReady() observes the
    engine has confirmed the buffer is safe to read (polled from the timer). */
void MainComponent::toggleRecording()
{
    if (! awaitingRecordedTake_)
    {
        if (! engine_.beginRecording())
        {
            clipLabel.setText("No audio input device available", juce::dontSendNotification);
            return;
        }

        awaitingRecordedTake_ = true;
        recordButton.setButtonText("Stop Rec");
        recordButton.setToggleState(true, juce::dontSendNotification);
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopRecording();
        post(Cmd::SetPlaying, 0.0);
        recordButton.setButtonText("Record");
        recordButton.setToggleState(false, juce::dontSendNotification);
    }
}

void MainComponent::finishRecordingIfReady()
{
    if (! awaitingRecordedTake_ || ! engine_.isRecordingFinished())
        return;
    awaitingRecordedTake_ = false;

    const int length = engine_.recordedTakeLength();
    if (length <= 0)
    {
        clipLabel.setText("Recording was empty (no input captured)", juce::dontSendNotification);
        return;
    }

    const auto& takeBuffer = engine_.recordedTakeBuffer();
    juce::AudioBuffer<float> trimmed(takeBuffer.getNumChannels(), length);
    for (int ch = 0; ch < takeBuffer.getNumChannels(); ++ch)
        trimmed.copyFrom(ch, 0, takeBuffer, ch, 0, length);

    const auto file = recordingsDirectory().getNonexistentChildFile("Recording", ".wav");
    if (! engine::OfflineRenderer::writeWav(file, trimmed, engine_.sampleRate()))
    {
        clipLabel.setText("Failed to write recording", juce::dontSendNotification);
        return;
    }

    const auto path = file.getFullPathName().toStdString();
    int        newTrackIndex = -1;

    history_.edit("Record audio", [&path, &newTrackIndex](model::Song& s)
    {
        const auto name = "Recording " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Audio, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(s);
        clip.type        = model::ClipType::Audio;
        clip.startBeats  = 0.0;
        clip.lengthBeats = 4.0;
        clip.audioFile   = path;
        s.tracks.back().clips.push_back(clip);

        newTrackIndex = (int) s.tracks.size() - 1;
    });

    selectNewlyAddedTrack(newTrackIndex);
    clipLabel.setText("Recorded: " + file.getFileName(), juce::dontSendNotification);
}

juce::File MainComponent::recordingsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Recordings");
    dir.createDirectory();
    return dir;
}

void MainComponent::saveProject()
{
    chooser_ = std::make_unique<juce::FileChooser>("Save project", juce::File{}, "*.looper");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        file = file.withFileExtension("looper");
        const std::string text = model::serialize(history_.current());
        file.replaceWithText(juce::String::fromUTF8(text.c_str()));
    });
}

void MainComponent::openProject()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open project", juce::File{}, "*.looper");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        model::Song song;
        if (! model::deserialize(file.loadFileAsString().toStdString(), song))
        {
            clipLabel.setText("Could not open: " + file.getFileName(), juce::dontSendNotification);
            return;
        }

        history_.reset(song);
        selectedTrackIndex_ = 0;

        tempoSlider.setValue(song.bpm, juce::dontSendNotification);
        uiTempoMap_.setTempo(song.bpm);
        post(Cmd::SetTempo, song.bpm);
        refreshFromModel();
    });
}

void MainComponent::bounceProject()
{
    chooser_ = std::make_unique<juce::FileChooser>("Bounce to WAV", juce::File{}, "*.wav");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;

        file = file.withFileExtension("wav");

        const auto& song = history_.current();
        std::vector<engine::Pattern> patterns;
        std::vector<float>           gains;
        std::vector<bool>            solos;
        std::vector<double>          clipStarts;
        std::vector<float>           sends;
        for (const auto& track : song.tracks)
        {
            patterns.push_back(track.clips.empty() ? engine::Pattern {} : track.clips[0].pattern);
            gains.push_back(track.muted ? -100.0f : track.gainDb);
            solos.push_back(track.solo);
            clipStarts.push_back(track.clips.empty() ? 0.0 : track.clips[0].startBeats);
            sends.push_back(track.sendLevel);
        }
        if (patterns.empty())
        {
            patterns.push_back({});
            gains.push_back(0.0f);
            solos.push_back(false);
            clipStarts.push_back(0.0);
            sends.push_back(0.0f);
        }

        const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 44100.0;
        const double bpm        = song.bpm;
        auto         buffer     = engine::OfflineRenderer::render(patterns, gains, solos, clipStarts, sends,
                                                                  song.sendBus.enabled, song.sendBus.roomSize,
                                                                  song.sendBus.damping, song.sendBus.returnLevel,
                                                                  bpm, sampleRate, 8.0);

        if (song.filter.enabled)
        {
            engine::FilterEffect ff;
            ff.prepare(sampleRate, 512);
            ff.setEnabled(true);
            ff.setMode(song.filter.mode);
            ff.setCutoff(song.filter.cutoff);
            ff.setResonance(song.filter.resonance);
            ff.process(buffer);
        }

        if (song.delay.enabled)
        {
            engine::DelayEffect fx;
            fx.prepare(sampleRate, 512);
            fx.setEnabled(true);
            fx.setTimeMs(song.delay.timeMs);
            fx.setFeedback(song.delay.feedback);
            fx.setMix(song.delay.mix);
            fx.process(buffer);
        }

        if (song.reverb.enabled)
        {
            engine::ReverbEffect rv;
            rv.prepare(sampleRate, 512);
            rv.setEnabled(true);
            rv.setRoomSize(song.reverb.roomSize);
            rv.setDamping(song.reverb.damping);
            rv.setMix(song.reverb.mix);
            rv.process(buffer);
        }

        if (! song.masterGainDb.empty())
        {
            const double samplesPerBeat = sampleRate * 60.0 / bpm;
            const int    n              = buffer.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const double beat = samplesPerBeat > 0.0 ? (double) i / samplesPerBeat : 0.0;
                const float  g    = juce::Decibels::decibelsToGain(song.masterGainDb.valueAt(beat, 0.0f));
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.getWritePointer(ch)[i] *= g;
            }
        }

        clipLabel.setText(engine::OfflineRenderer::writeWav(file, buffer, sampleRate)
                              ? "Bounced: " + file.getFileName()
                              : juce::String("Bounce failed"),
                          juce::dontSendNotification);
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
    engine_.pump();
    finishRecordingIfReady();

    addTrackButton.setEnabled(trackCount() < engine_.maxTracks());
    addClipButton_.setEnabled(selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount());

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

    const int n = trackCount();
    for (int i = 0; i < n; ++i)
    {
        trackStrips_[i]->setLevel(0, engine_.trackPeak(i, 0));
        trackStrips_[i]->setLevel(1, engine_.trackPeak(i, 1));
    }

    arrangementView_.setPlayheadBeats(uiTempoMap_.ppqFromSamples(playhead));

    // Gain automation playback (coarse, message-thread; sample-accurate on
    // export — see bounceProject()). Master and per-track lanes both apply.
    if (! recordAutomation_ && engine_.isPlaying())
    {
        const double beat = uiTempoMap_.ppqFromSamples(playhead);
        const auto&  song = history_.current();

        if (! song.masterGainDb.empty())
        {
            const float db = song.masterGainDb.valueAt(beat, (float) masterSlider.getValue());
            post(Cmd::SetMasterGainDb, db);
            masterSlider.setValue(db, juce::dontSendNotification);
        }

        for (int i = 0; i < n; ++i)
        {
            const auto& lane = song.tracks[(size_t) i].gainAutomation;
            if (lane.empty())
                continue;

            const float db = lane.valueAt(beat, song.tracks[(size_t) i].gainDb);
            engine_.setTrackGainDb(i, db);
            trackStrips_[i]->setGainDb(db);
        }
    }
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
    auto full = getLocalBounds();
    menuBar_.setBounds(full.removeFromTop(24));

    keyboard_.setBounds(full.removeFromBottom(64));
    full.removeFromBottom(10);

    juce::Component* panes[] = { &dockRegionFiles_, &paneResizerFiles_, &leftPane_, &paneResizer_,
                                &dockRegionA_,      &paneResizer2_,     &dockRegionB_ };
    paneLayout_.layOutComponents(panes, 7, full.getX(), full.getY(),
                                 full.getWidth(), full.getHeight(),
                                 false,  // side-by-side, not stacked
                                 true);  // and stretch each to the full height

    layoutLeftPane();
}

void MainComponent::layoutLeftPane()
{
    auto area = leftPane_.getLocalBounds().reduced(12);

    auto row1 = area.removeFromTop(30);
    playButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(12);
    loopButton.setBounds(row1.removeFromLeft(60));
    row1.removeFromLeft(12);
    recordButton.setBounds(row1.removeFromLeft(80));
    area.removeFromTop(8);

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
}

void MainComponent::movePanelBetweenRegions(const juce::String& panelName, DockRegion& target)
{
    DockRegion* source = nullptr;
    for (auto* region : { &dockRegionFiles_, &dockRegionA_, &dockRegionB_ })
        if (region->hasPanel(panelName))
            source = region;
    if (source == nullptr || source == &target)
        return;

    juce::Component* content = nullptr;
    if (panelName == "Files")        content = &fileBrowser_;
    else if (panelName == "Arrange") content = &arrangeTab_;
    else if (panelName == "Edit")    content = &editTab_;
    else if (panelName == "Mixer")   content = &mixerView_;
    if (content == nullptr)
        return;

    source->removePanel(panelName);
    target.addPanel(panelName, *content);
}

void MainComponent::layoutArrangeTab()
{
    auto area = arrangeTab_.getLocalBounds();

    auto toolbar = area.removeFromTop(28).reduced(4, 2);
    zoomOutButton_.setBounds(toolbar.removeFromLeft(28));
    toolbar.removeFromLeft(4);
    zoomInButton_.setBounds(toolbar.removeFromLeft(28));
    toolbar.removeFromLeft(12);
    addClipButton_.setBounds(toolbar.removeFromLeft(90));

    arrangementViewport_.setBounds(area);
}

void MainComponent::layoutEditTab()
{
    auto area = editTab_.getLocalBounds();
    editingLabel_.setBounds(area.removeFromTop(22).reduced(6, 0));
    pianoRoll_.setBounds(area);
}

void MainComponent::layoutMixerView()
{
    auto area = mixerView_.getLocalBounds().reduced(10);
    if (area.isEmpty())
        return;

    auto toolbar = area.removeFromTop(28);
    addTrackButton.setBounds(toolbar.removeFromLeft(100));
    area.removeFromTop(8);

    // ---- master strip: master fader/automation, filter, delay, reverb, meter ----
    auto masterArea = area.removeFromRight(300);
    area.removeFromRight(12);

    auto masterRow = masterArea.removeFromTop(26);
    autoRecButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(6);
    autoClearButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(10);
    masterSlider.setBounds(masterRow.withTrimmedLeft(64));
    masterArea.removeFromTop(6);

    auto filterRow = masterArea.removeFromTop(26);
    filterButton.setBounds(filterRow.removeFromLeft(64));
    filterRow.removeFromLeft(6);
    filterModeBox_.setBounds(filterRow.removeFromLeft(104));
    filterRow.removeFromLeft(8);
    const int fw = juce::jmax(60, (filterRow.getWidth() - 8) / 2);
    filterCutoffSlider.setBounds(filterRow.removeFromLeft(fw));
    filterRow.removeFromLeft(8);
    filterResoSlider.setBounds(filterRow);
    masterArea.removeFromTop(6);

    auto delayRow = masterArea.removeFromTop(26);
    delayButton.setBounds(delayRow.removeFromLeft(70));
    delayRow.removeFromLeft(8);
    const int dw = juce::jmax(50, (delayRow.getWidth() - 16) / 3);
    delayTimeSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayFbSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayMixSlider.setBounds(delayRow);
    masterArea.removeFromTop(6);

    auto reverbRow = masterArea.removeFromTop(26);
    reverbButton.setBounds(reverbRow.removeFromLeft(70));
    reverbRow.removeFromLeft(8);
    const int rw = juce::jmax(50, (reverbRow.getWidth() - 16) / 3);
    reverbRoomSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbDampSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbMixSlider.setBounds(reverbRow);
    masterArea.removeFromTop(6);

    auto sendRow = masterArea.removeFromTop(26);
    sendBusButton.setBounds(sendRow.removeFromLeft(70));
    sendRow.removeFromLeft(8);
    const int sw = juce::jmax(50, (sendRow.getWidth() - 16) / 3);
    sendRoomSlider.setBounds(sendRow.removeFromLeft(sw));
    sendRow.removeFromLeft(8);
    sendDampSlider.setBounds(sendRow.removeFromLeft(sw));
    sendRow.removeFromLeft(8);
    sendReturnSlider.setBounds(sendRow);
    masterArea.removeFromTop(8);

    meter_.setBounds(masterArea.removeFromTop(44));

    // ---- per-track channel strips, filling the remaining width ----
    const int stripWidth = 96;
    const int gap        = 6;
    int       x          = area.getX();
    const int n           = trackCount();

    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip = trackStrips_[i];
        if (i < n)
        {
            strip->setBounds(x, area.getY(), stripWidth, area.getHeight());
            x += stripWidth + gap;
        }
    }
}

} // namespace looper
