#include "MainComponent.h"

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
    stopButton.onClick = [this] { post(Cmd::SetPlaying, 0.0); post(Cmd::Seek, 0.0); };
    loopButton.onClick = [this]
    {
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        updateLoopRegion();
    };
    addTrackButton.onClick = [this] { addTrack(); };

    for (auto* b : { &playButton, &stopButton, &addTrackButton })
        addAndMakeVisible(b);
    addAndMakeVisible(loopButton);

    trackSelector_.onChange = [this]
    {
        const int id = trackSelector_.getSelectedId();
        if (id <= 0)
            return;
        selectedTrackIndex_ = id - 1;
        engine_.setArmedTrack(selectedTrackIndex_);
        refreshPianoRollForSelected();
        updateTrackControls();
    };
    addAndMakeVisible(trackSelector_);

    trackMuteButton.onClick = [this] { setSelectedTrackMuted(trackMuteButton.getToggleState()); };
    addAndMakeVisible(trackMuteButton);

    trackGainSlider.setRange(-60.0, 6.0, 0.1);
    trackGainSlider.setValue(0.0, juce::dontSendNotification);
    trackGainSlider.setTextValueSuffix(" dB");
    trackGainSlider.onValueChange = [this] { setSelectedTrackGain((float) trackGainSlider.getValue()); };
    addAndMakeVisible(trackGainSlider);

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

    addAndMakeVisible(tempoSlider);
    addAndMakeVisible(masterSlider);
    tempoLabel.attachToComponent(&tempoSlider, true);
    masterLabel.attachToComponent(&masterSlider, true);

    // ---- master filter (stored in the document) ----
    filterButton.onClick = [this]
    {
        const bool on = filterButton.getToggleState();
        history_.mutableCurrent().filter.enabled = on;
        engine_.setMasterFilterEnabled(on);
    };
    addAndMakeVisible(filterButton);

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
    addAndMakeVisible(filterModeBox_);

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
    addAndMakeVisible(filterCutoffSlider);

    filterResoSlider.setRange(0.1, 5.0, 0.01);
    filterResoSlider.setValue(0.707, juce::dontSendNotification);
    filterResoSlider.setTextValueSuffix(" Q");
    filterResoSlider.onValueChange = [this]
    {
        const float q = (float) filterResoSlider.getValue();
        history_.mutableCurrent().filter.resonance = q;
        engine_.setMasterFilterResonance(q);
    };
    addAndMakeVisible(filterResoSlider);

    // ---- master delay (stored in the document, so it saves + restores) ----
    delayButton.onClick = [this]
    {
        const bool on = delayButton.getToggleState();
        history_.mutableCurrent().delay.enabled = on;
        engine_.setMasterDelayEnabled(on);
    };
    addAndMakeVisible(delayButton);

    delayTimeSlider.setRange(20.0, 1000.0, 1.0);
    delayTimeSlider.setValue(300.0, juce::dontSendNotification);
    delayTimeSlider.setTextValueSuffix(" ms");
    delayTimeSlider.onValueChange = [this]
    {
        const float ms = (float) delayTimeSlider.getValue();
        history_.mutableCurrent().delay.timeMs = ms;
        engine_.setMasterDelayTimeMs(ms);
    };
    addAndMakeVisible(delayTimeSlider);

    delayFbSlider.setRange(0.0, 95.0, 1.0);
    delayFbSlider.setValue(35.0, juce::dontSendNotification);
    delayFbSlider.setTextValueSuffix(" %");
    delayFbSlider.onValueChange = [this]
    {
        const float fb = (float) (delayFbSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.feedback = fb;
        engine_.setMasterDelayFeedback(fb);
    };
    addAndMakeVisible(delayFbSlider);

    delayMixSlider.setRange(0.0, 100.0, 1.0);
    delayMixSlider.setValue(30.0, juce::dontSendNotification);
    delayMixSlider.setTextValueSuffix(" %");
    delayMixSlider.onValueChange = [this]
    {
        const float mix = (float) (delayMixSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.mix = mix;
        engine_.setMasterDelayMix(mix);
    };
    addAndMakeVisible(delayMixSlider);

    // ---- master reverb (stored in the document) ----
    reverbButton.onClick = [this]
    {
        const bool on = reverbButton.getToggleState();
        history_.mutableCurrent().reverb.enabled = on;
        engine_.setMasterReverbEnabled(on);
    };
    addAndMakeVisible(reverbButton);

    reverbRoomSlider.setRange(0.0, 100.0, 1.0);
    reverbRoomSlider.setValue(50.0, juce::dontSendNotification);
    reverbRoomSlider.setTextValueSuffix(" room");
    reverbRoomSlider.onValueChange = [this]
    {
        const float v = (float) (reverbRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.roomSize = v;
        engine_.setMasterReverbRoomSize(v);
    };
    addAndMakeVisible(reverbRoomSlider);

    reverbDampSlider.setRange(0.0, 100.0, 1.0);
    reverbDampSlider.setValue(50.0, juce::dontSendNotification);
    reverbDampSlider.setTextValueSuffix(" damp");
    reverbDampSlider.onValueChange = [this]
    {
        const float v = (float) (reverbDampSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.damping = v;
        engine_.setMasterReverbDamping(v);
    };
    addAndMakeVisible(reverbDampSlider);

    reverbMixSlider.setRange(0.0, 100.0, 1.0);
    reverbMixSlider.setValue(30.0, juce::dontSendNotification);
    reverbMixSlider.setTextValueSuffix(" %");
    reverbMixSlider.onValueChange = [this]
    {
        const float v = (float) (reverbMixSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.mix = v;
        engine_.setMasterReverbMix(v);
    };
    addAndMakeVisible(reverbMixSlider);

    // ---- master-gain automation: arm, then move the master fader while playing ----
    autoRecButton.onClick   = [this] { recordAutomation_ = autoRecButton.getToggleState(); };
    autoClearButton.onClick = [this] { history_.mutableCurrent().masterGainDb.clear(); };
    addAndMakeVisible(autoRecButton);
    addAndMakeVisible(autoClearButton);

    positionLabel.setFont(juce::Font(juce::FontOptions(20.0f)));
    positionLabel.setText("Bar 1  Beat 1   |   0.00 s   |   STOPPED", juce::dontSendNotification);
    addAndMakeVisible(positionLabel);

    clipLabel.setText("No clip loaded", juce::dontSendNotification);
    addAndMakeVisible(clipLabel);

    pianoRoll_.onChange = [this](const engine::Pattern& p) { editPattern(p); };

    const auto tabBg = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    tabs_.addTab("Arrange", tabBg, &arrangementView_, false);
    tabs_.addTab("Edit", tabBg, &pianoRoll_, false);
    tabs_.setCurrentTabIndex(1); // start on the note editor
    addAndMakeVisible(tabs_);

    addAndMakeVisible(meter_);
    addAndMakeVisible(keyboard_);

    // Mirror the initial document into the engine + UI.
    rebuildTrackSelector();
    syncEngineTracks();
    engine_.setArmedTrack(0);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    updateTrackControls();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();

    engine_.deviceManager().addChangeListener(this);
    logAudioDeviceStatus();

    setWantsKeyboardFocus(true);
    setSize(700, 800);
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
        case 4:  chooseFile(); break; // import audio
        case 5:  bounceProject(); break;
        case 6:  showAudioSettings(); break;
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
    return track.clips.empty() ? empty : track.clips[0].pattern;
}

void MainComponent::editPattern(const engine::Pattern& pattern)
{
    const int idx = selectedTrackIndex_;
    history_.edit("Edit notes", [&pattern, idx](model::Song& s)
    {
        if (idx >= 0 && idx < (int) s.tracks.size() && ! s.tracks[(size_t) idx].clips.empty())
            s.tracks[(size_t) idx].clips[0].pattern = pattern;
    });
    engine_.setTrackPattern(idx, pattern);
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
    rebuildTrackSelector();
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    updateTrackControls();
}

void MainComponent::syncEngineTracks()
{
    const auto& song = history_.current();
    const int   n    = juce::jmin((int) song.tracks.size(), engine_.maxTracks());

    for (int i = 0; i < n; ++i)
    {
        const auto& track = song.tracks[(size_t) i];
        engine_.setTrackPattern(i, track.clips.empty() ? engine::Pattern {} : track.clips[0].pattern);
        engine_.setTrackMuted(i, track.muted);
        engine_.setTrackGainDb(i, track.gainDb);
    }
    engine_.setActiveTrackCount(n);
}

void MainComponent::rebuildTrackSelector()
{
    trackSelector_.clear(juce::dontSendNotification);

    const auto& song = history_.current();
    for (int i = 0; i < (int) song.tracks.size(); ++i)
    {
        const auto& name = song.tracks[(size_t) i].name;
        trackSelector_.addItem(name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(name), i + 1);
    }

    if (selectedTrackIndex_ >= (int) song.tracks.size())
        selectedTrackIndex_ = juce::jmax(0, (int) song.tracks.size() - 1);

    trackSelector_.setSelectedId(selectedTrackIndex_ + 1, juce::dontSendNotification);
}

void MainComponent::refreshPianoRollForSelected()
{
    pianoRoll_.setPattern(currentPattern());
}

void MainComponent::updateTrackControls()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    trackGainSlider.setValue(track.gainDb, juce::dontSendNotification);
    trackMuteButton.setToggleState(track.muted, juce::dontSendNotification);
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

void MainComponent::setSelectedTrackGain(float gainDb)
{
    // Live tweak: update the current document in place (not a separate undo step).
    const int idx  = selectedTrackIndex_;
    auto&     song = history_.mutableCurrent();
    if (idx >= 0 && idx < (int) song.tracks.size())
        song.tracks[(size_t) idx].gainDb = gainDb;
    engine_.setTrackGainDb(idx, gainDb);
}

void MainComponent::setSelectedTrackMuted(bool muted)
{
    const int idx  = selectedTrackIndex_;
    auto&     song = history_.mutableCurrent();
    if (idx >= 0 && idx < (int) song.tracks.size())
        song.tracks[(size_t) idx].muted = muted;
    engine_.setTrackMuted(idx, muted);
}

void MainComponent::refreshFromModel()
{
    if (selectedTrackIndex_ >= trackCount())
        selectedTrackIndex_ = juce::jmax(0, trackCount() - 1);

    rebuildTrackSelector();
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    arrangementView_.setSong(history_.current());
    updateTrackControls();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();
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
        for (const auto& track : song.tracks)
        {
            patterns.push_back(track.clips.empty() ? engine::Pattern {} : track.clips[0].pattern);
            gains.push_back(track.muted ? -100.0f : track.gainDb);
        }
        if (patterns.empty())
        {
            patterns.push_back({});
            gains.push_back(0.0f);
        }

        const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 44100.0;
        const double bpm        = song.bpm;
        auto         buffer     = engine::OfflineRenderer::render(patterns, gains, bpm, sampleRate, 8.0);

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

    addTrackButton.setEnabled(trackCount() < engine_.maxTracks());

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

    arrangementView_.setPlayheadBeats(uiTempoMap_.ppqFromSamples(playhead));

    // Master-gain automation playback (coarse, message-thread; sample-accurate on export).
    if (! recordAutomation_ && engine_.isPlaying() && ! history_.current().masterGainDb.empty())
    {
        const double beat = uiTempoMap_.ppqFromSamples(playhead);
        const float  db   = history_.current().masterGainDb.valueAt(beat, (float) masterSlider.getValue());
        post(Cmd::SetMasterGainDb, db);
        masterSlider.setValue(db, juce::dontSendNotification);
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

    auto area = full.reduced(12);

    auto row1 = area.removeFromTop(30);
    playButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(12);
    loopButton.setBounds(row1.removeFromLeft(60));
    area.removeFromTop(8);

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(4);

    auto masterRow = area.removeFromTop(26);
    autoRecButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(6);
    autoClearButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(10);
    masterSlider.setBounds(masterRow.withTrimmedLeft(64));
    area.removeFromTop(6);

    auto filterRow = area.removeFromTop(26);
    filterButton.setBounds(filterRow.removeFromLeft(64));
    filterRow.removeFromLeft(6);
    filterModeBox_.setBounds(filterRow.removeFromLeft(104));
    filterRow.removeFromLeft(8);
    const int fw = juce::jmax(80, (filterRow.getWidth() - 8) / 2);
    filterCutoffSlider.setBounds(filterRow.removeFromLeft(fw));
    filterRow.removeFromLeft(8);
    filterResoSlider.setBounds(filterRow);
    area.removeFromTop(6);

    auto delayRow = area.removeFromTop(26);
    delayButton.setBounds(delayRow.removeFromLeft(70));
    delayRow.removeFromLeft(8);
    const int dw = juce::jmax(60, (delayRow.getWidth() - 16) / 3);
    delayTimeSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayFbSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayMixSlider.setBounds(delayRow);
    area.removeFromTop(6);

    auto reverbRow = area.removeFromTop(26);
    reverbButton.setBounds(reverbRow.removeFromLeft(70));
    reverbRow.removeFromLeft(8);
    const int rw = juce::jmax(60, (reverbRow.getWidth() - 16) / 3);
    reverbRoomSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbDampSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbMixSlider.setBounds(reverbRow);
    area.removeFromTop(8);

    meter_.setBounds(area.removeFromTop(44));
    area.removeFromTop(10);

    auto trackRow = area.removeFromTop(28);
    addTrackButton.setBounds(trackRow.removeFromLeft(90));
    trackRow.removeFromLeft(8);
    trackSelector_.setBounds(trackRow.removeFromLeft(150));
    trackRow.removeFromLeft(12);
    trackMuteButton.setBounds(trackRow.removeFromLeft(60));
    trackRow.removeFromLeft(10);
    trackGainSlider.setBounds(trackRow);
    area.removeFromTop(8);

    keyboard_.setBounds(area.removeFromBottom(64));
    area.removeFromBottom(10);

    tabs_.setBounds(area);
}

} // namespace looper
