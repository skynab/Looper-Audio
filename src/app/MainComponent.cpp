#include "MainComponent.h"

#include "engine/OfflineRenderer.h"
#include "model/Serialization.h"

#include <cmath>
#include <vector>

namespace looper
{
using Cmd = engine::EngineCommand::Type;

MainComponent::MainComponent()
    : deviceSelector(engine_.deviceManager(),
                     0, 0, 0, 2, false, false, false, false)
{
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
    loadButton.onClick     = [this] { chooseFile(); };
    clearButton.onClick    = [this] { pianoRoll_.clear(); };
    undoButton.onClick     = [this] { history_.undo(); refreshFromModel(); };
    redoButton.onClick     = [this] { history_.redo(); refreshFromModel(); };
    saveButton.onClick     = [this] { saveProject(); };
    openButton.onClick     = [this] { openProject(); };
    bounceButton.onClick   = [this] { bounceProject(); };
    addTrackButton.onClick = [this] { addTrack(); };

    for (auto* b : { &playButton, &stopButton, &loadButton, &clearButton, &undoButton, &redoButton,
                     &saveButton, &openButton, &bounceButton, &addTrackButton })
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
    };
    addAndMakeVisible(trackSelector_);

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
    masterSlider.onValueChange = [this] { post(Cmd::SetMasterGainDb, masterSlider.getValue()); };

    addAndMakeVisible(tempoSlider);
    addAndMakeVisible(masterSlider);
    tempoLabel.attachToComponent(&tempoSlider, true);
    masterLabel.attachToComponent(&masterSlider, true);

    positionLabel.setFont(juce::Font(juce::FontOptions(20.0f)));
    positionLabel.setText("Bar 1  Beat 1   |   0.00 s   |   STOPPED", juce::dontSendNotification);
    addAndMakeVisible(positionLabel);

    clipLabel.setText("No clip loaded", juce::dontSendNotification);
    addAndMakeVisible(clipLabel);

    pianoRoll_.onChange = [this](const engine::Pattern& p) { editPattern(p); };
    addAndMakeVisible(pianoRoll_);

    addAndMakeVisible(meter_);
    addAndMakeVisible(keyboard_);
    addAndMakeVisible(deviceSelector);

    // Mirror the initial document into the engine + UI.
    rebuildTrackSelector();
    syncEngineTracks();
    engine_.setArmedTrack(0);
    refreshPianoRollForSelected();

    engine_.deviceManager().addChangeListener(this);
    logAudioDeviceStatus();

    setWantsKeyboardFocus(true);
    setSize(680, 840);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine_.deviceManager().removeChangeListener(this);
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

void MainComponent::refreshFromModel()
{
    if (selectedTrackIndex_ >= trackCount())
        selectedTrackIndex_ = juce::jmax(0, trackCount() - 1);

    rebuildTrackSelector();
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
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

        std::vector<engine::Pattern> patterns;
        for (const auto& track : history_.current().tracks)
            patterns.push_back(track.clips.empty() ? engine::Pattern {} : track.clips[0].pattern);
        if (patterns.empty())
            patterns.push_back({});

        const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 44100.0;
        const double bpm        = history_.current().bpm;
        const auto   buffer     = engine::OfflineRenderer::render(patterns, bpm, sampleRate, 8.0);

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

    undoButton.setEnabled(history_.canUndo());
    redoButton.setEnabled(history_.canRedo());
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
    auto area = getLocalBounds().reduced(12);

    auto row1 = area.removeFromTop(30);
    playButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(12);
    loopButton.setBounds(row1.removeFromLeft(60));
    row1.removeFromLeft(18);
    undoButton.setBounds(row1.removeFromLeft(70));
    row1.removeFromLeft(6);
    redoButton.setBounds(row1.removeFromLeft(70));
    area.removeFromTop(8);

    auto row2 = area.removeFromTop(28);
    loadButton.setBounds(row2.removeFromLeft(110));
    row2.removeFromLeft(8);
    clearButton.setBounds(row2.removeFromLeft(100));
    row2.removeFromLeft(16);
    saveButton.setBounds(row2.removeFromLeft(80));
    row2.removeFromLeft(8);
    openButton.setBounds(row2.removeFromLeft(80));
    row2.removeFromLeft(8);
    bounceButton.setBounds(row2.removeFromLeft(90));
    area.removeFromTop(8);

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(4);
    masterSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(8);

    meter_.setBounds(area.removeFromTop(44));
    area.removeFromTop(10);

    auto trackRow = area.removeFromTop(28);
    addTrackButton.setBounds(trackRow.removeFromLeft(100));
    trackRow.removeFromLeft(10);
    trackSelector_.setBounds(trackRow.removeFromLeft(220));
    area.removeFromTop(8);

    deviceSelector.setBounds(area.removeFromBottom(130));
    area.removeFromBottom(8);
    keyboard_.setBounds(area.removeFromBottom(64));
    area.removeFromBottom(10);

    pianoRoll_.setBounds(area);
}

} // namespace looper
