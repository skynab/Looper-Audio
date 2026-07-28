#include "MainComponent.h"

#include "Icons.h"

#include "engine/ClipSlot.h"
#include "engine/GuitarChords.h"
#include "engine/NoteOps.h"
#include "engine/MidiFileIO.h"
#include "engine/OfflineRenderer.h"
#include "model/Serialization.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace looper
{
/** View-menu ids for panels start well clear of the fixed commands, so adding
    a pane can never collide with one. */
static constexpr int kFirstPanelMenuId = 100;

using Cmd = engine::EngineCommand::Type;

/**
    The keyboard shortcuts, defined once.

    Each is the single source for both the key the app listens for and the
    text its menu item advertises, so the two can't drift — a menu promising
    a shortcut that does nothing is worse than no shortcut. Written as
    descriptions rather than raw key codes because "command" already means
    Ctrl away from macOS, and because they read as what the user sees.
*/
namespace keys
{
    inline const juce::KeyPress newProject = juce::KeyPress::createFromDescription("command + N");
    inline const juce::KeyPress open       = juce::KeyPress::createFromDescription("command + O");
    inline const juce::KeyPress save       = juce::KeyPress::createFromDescription("command + S");
    inline const juce::KeyPress saveAs     = juce::KeyPress::createFromDescription("command + shift + S");
    inline const juce::KeyPress bounce     = juce::KeyPress::createFromDescription("command + shift + B");

    inline const juce::KeyPress undo       = juce::KeyPress::createFromDescription("command + Z");
    inline const juce::KeyPress redo       = juce::KeyPress::createFromDescription("command + shift + Z");
    inline const juce::KeyPress redoAlt    = juce::KeyPress::createFromDescription("command + Y");

    // Notes and clips get separate shortcuts for the same reason they get
    // separate menu commands: one pair whose meaning depends on which pane
    // has focus is a coin toss at the moment you press it.
    inline const juce::KeyPress copyNotes  = juce::KeyPress::createFromDescription("command + C");
    inline const juce::KeyPress pasteNotes = juce::KeyPress::createFromDescription("command + V");
    inline const juce::KeyPress copyClip   = juce::KeyPress::createFromDescription("command + shift + C");
    inline const juce::KeyPress pasteClip  = juce::KeyPress::createFromDescription("command + shift + V");
    inline const juce::KeyPress duplicate  = juce::KeyPress::createFromDescription("command + D");
    inline const juce::KeyPress quantize   = juce::KeyPress::createFromDescription("command + U");
    inline const juce::KeyPress deleteClip = juce::KeyPress::createFromDescription("command + backspace");

    // Transport. Space is unmodified because it's the control reached for
    // most, and every DAW spells it this way; a focused text field consumes
    // its own keys first, so it can't interrupt typing.
    inline const juce::KeyPress playPause  = juce::KeyPress(juce::KeyPress::spaceKey);
    inline const juce::KeyPress toStart    = juce::KeyPress(juce::KeyPress::homeKey);
    inline const juce::KeyPress toEnd      = juce::KeyPress(juce::KeyPress::endKey);
    inline const juce::KeyPress backOneBar = juce::KeyPress::createFromDescription("command + cursor left");
    inline const juce::KeyPress onOneBar   = juce::KeyPress::createFromDescription("command + cursor right");
    inline const juce::KeyPress record     = juce::KeyPress::createFromDescription("command + R");
    inline const juce::KeyPress loop       = juce::KeyPress::createFromDescription("command + L");

    /** Every shortcut above, for the startup check. A description with a typo
        in it parses to an invalid KeyPress that matches nothing and prints no
        shortcut text — a failure that otherwise surfaces only when someone
        presses the key and nothing happens. */
    inline const juce::KeyPress all[] = {
        newProject, open, save, saveAs, bounce,
        undo, redo, redoAlt,
        copyNotes, pasteNotes, copyClip, pasteClip, duplicate, quantize, deleteClip,
        playPause, toStart, toEnd, backOneBar, onOneBar, record, loop
    };
}

namespace
{
    /** Adds a menu item that advertises its shortcut. PopupMenu's plain
        addItem overload has nowhere to put one, and an undiscoverable
        shortcut may as well not exist. */
    void addItem(juce::PopupMenu& menu, int id, const juce::String& text,
                 const juce::KeyPress& shortcut, bool enabled = true)
    {
        juce::PopupMenu::Item item(text);
        item.itemID                = id;
        item.isEnabled             = enabled;
        item.shortcutKeyDescription = shortcut.getTextDescriptionWithIcons();
        menu.addItem(std::move(item));
    }

    /** "Undo Delete track" rather than a bare "Undo". Every edit already
        records what it was; not showing it left the user to remember what
        they'd done, which is the one thing undo exists to spare them. */
    juce::String withAction(const char* verb, bool available, const std::string& action)
    {
        juce::String text(verb);
        if (available && ! action.empty())
            text += " " + juce::String(action);
        return text;
    }

    /** "Play / pause  (space)" — a control with no menu entry has nowhere
        else to say what its shortcut is. */
    juce::String withShortcut(const juce::String& text, const juce::KeyPress& key)
    {
        return text + "  (" + key.getTextDescriptionWithIcons() + ")";
    }

    juce::PropertiesFile::Options makeSettingsOptions()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Looper-Audio";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Looper-Audio";
        opts.osxLibrarySubFolder = "Application Support";
        return opts;
    }
}

MainComponent::MainComponent()
    : settings_(makeSettingsOptions())
{
    menuBar_.setModel(this);
    addAndMakeVisible(menuBar_);

    // Dockable workspace: a tree of tab groups, arranged entirely by dragging
    // tabs (see DockWorkspace). The panel registry below is the one place
    // that maps a panel's name to the Component behind it — the workspace
    // moves panels around by name from then on, including when restoring a
    // saved layout.
    addAndMakeVisible(workspace_);
    workspace_.onLayoutChanged = [this] { saveDockLayout(); };

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
    // Play/pause is one control: pausing leaves the playhead where it is, and
    // returning to the start is first-frame's job. That's why the old separate
    // Stop button is gone rather than kept alongside.
    playPauseButton.onClick = [this]
    {
        if (engine_.isPlaying())
        {
            post(Cmd::SetPlaying, 0.0);
            if (awaitingRecordedTake_)
                engine_.stopRecording(); // the transport stopping would also end
                                         // the take, but this makes it explicit
        }
        else
        {
            post(Cmd::SetPlaying, 1.0);
        }
    };

    firstFrameButton.onClick    = [this] { seekToBeat(0.0); };
    previousFrameButton.onClick = [this] { stepByBars(-1); };
    nextFrameButton.onClick     = [this] { stepByBars(+1); };
    lastFrameButton.onClick     = [this] { seekToBeat(songEndBeats()); };

    {
        auto play  = icons::fromSvg(icons::kPlay);
        auto pause = icons::fromSvg(icons::kPause);
        playPauseButton.setImages(play.get(), nullptr, nullptr, nullptr, pause.get());
    }
    {
        auto first = icons::fromSvg(icons::kFirstFrame);
        firstFrameButton.setImages(first.get());
    }
    {
        auto previous = icons::fromSvg(icons::kPreviousFrame);
        previousFrameButton.setImages(previous.get());
    }
    {
        auto next = icons::fromSvg(icons::kNextFrame);
        nextFrameButton.setImages(next.get());
    }
    {
        auto last = icons::fromSvg(icons::kLastFrame);
        lastFrameButton.setImages(last.get());
    }

    for (auto* button : { &firstFrameButton, &previousFrameButton, &playPauseButton,
                          &nextFrameButton, &lastFrameButton })
    {
        // The glyphs are the control; a button background would only box them in.
        button->setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
        button->setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    }

    // The transport has no menu to advertise its shortcuts from, so its
    // tooltips carry them — built from the same KeyPress the app listens for.
    firstFrameButton.setTooltip(withShortcut("Go to start", keys::toStart));
    previousFrameButton.setTooltip(withShortcut("Back one bar", keys::backOneBar));
    playPauseButton.setTooltip(withShortcut("Play / pause", keys::playPause));
    nextFrameButton.setTooltip(withShortcut("Forward one bar", keys::onOneBar));
    lastFrameButton.setTooltip(withShortcut("Go to end", keys::toEnd));
    loopButton.onClick = [this]
    {
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);
        updateLoopRegion();
    };
    recordButton.onClick = [this] { toggleRecording(); };
    {
        // One control, two states: the disc arms, the square stops. They're
        // this button's normal and "on" images, so which one shows follows
        // getToggleState() — see toggleRecording, which sets it.
        auto record = icons::fromSvg(icons::kRecordButton);
        auto stop   = icons::fromSvg(icons::kRecordStopButton);
        recordButton.setImages(record.get(), nullptr, nullptr, nullptr, stop.get());
    }
    // The icons carry their own ring, so a button background would only box
    // them in.
    recordButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    recordButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    recordButton.setTooltip(withShortcut("Record", keys::record));
    // Collapse toggle: hides everything below the button row, leaving just the
    // transport controls. The dock region's height is the user's to set by
    // dragging its divider — this is what makes a one-row pane worth dragging
    // down to, rather than resizing the region from under them.
    transportCollapsed_ = settings_.getValue("transportCollapsed", "0") != "0";
    collapseTransportButton_.onClick = [this]
    {
        transportCollapsed_ = ! transportCollapsed_;
        settings_.setValue("transportCollapsed", transportCollapsed_ ? "1" : "0");
        settings_.saveIfNeeded();
        applyTransportCollapse();
    };
    leftPane_.addAndMakeVisible(collapseTransportButton_);

    leftPane_.addAndMakeVisible(firstFrameButton);
    leftPane_.addAndMakeVisible(previousFrameButton);
    leftPane_.addAndMakeVisible(playPauseButton);
    leftPane_.addAndMakeVisible(nextFrameButton);
    leftPane_.addAndMakeVisible(lastFrameButton);
    leftPane_.addAndMakeVisible(recordButton);
    leftPane_.addAndMakeVisible(loopButton);

    // Click + count-in. Both are app preferences rather than project data —
    // how you like to record, not part of the song — so they persist through
    // settings_ alongside the dock layout.
    metronomeButton.onClick = [this]
    {
        engine_.setMetronomeEnabled(metronomeButton.getToggleState());
        settings_.setValue("metronomeEnabled", metronomeButton.getToggleState());
        settings_.saveIfNeeded();
    };
    metronomeButton.setToggleState(settings_.getBoolValue("metronomeEnabled", false),
                                   juce::dontSendNotification);
    engine_.setMetronomeEnabled(metronomeButton.getToggleState());
    leftPane_.addAndMakeVisible(metronomeButton);

    countInBox_.addItem("No count-in", 1);
    countInBox_.addItem("1 bar", 2);
    countInBox_.addItem("2 bars", 3);
    countInBox_.onChange = [this]
    {
        const int bars = juce::jmax(0, countInBox_.getSelectedId() - 1);
        engine_.setCountInBars(bars);
        settings_.setValue("countInBars", bars);
        settings_.saveIfNeeded();
    };
    countInBox_.setSelectedId(juce::jlimit(0, 2, settings_.getIntValue("countInBars", 0)) + 1,
                              juce::dontSendNotification);
    engine_.setCountInBars(juce::jmax(0, countInBox_.getSelectedId() - 1));
    leftPane_.addAndMakeVisible(countInBox_);

    leftPane_.onResized = [this] { layoutLeftPane(); };
    applyTransportCollapse(); // apply whatever state was restored above

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

    addDrumTrackButton_.onClick = [this] { addDrumTrack(); };
    addGuitarTrackButton_.onClick = [this] { addGuitarTrack(); };
    mixerView_.addAndMakeVisible(addDrumTrackButton_);
    mixerView_.addAndMakeVisible(addGuitarTrackButton_);

    // ---- master panel: collapsible (see toggleMasterPanelButton_) since at
    // narrow widths it was clipping against the track strips ----
    mixerView_.addAndMakeVisible(masterPanel_);
    masterPanel_.onResized = [this] { layoutMasterPanel(); };
    masterPanelVisible_ = settings_.getValue("masterPanelVisible", "1") != "0";
    masterPanel_.setVisible(masterPanelVisible_);
    toggleMasterPanelButton_.setButtonText(masterPanelVisible_ ? "Hide Master" : "Show Master");
    toggleMasterPanelButton_.onClick = [this]
    {
        masterPanelVisible_ = ! masterPanelVisible_;
        masterPanel_.setVisible(masterPanelVisible_);
        toggleMasterPanelButton_.setButtonText(masterPanelVisible_ ? "Hide Master" : "Show Master");
        settings_.setValue("masterPanelVisible", masterPanelVisible_ ? "1" : "0");
        settings_.saveIfNeeded();
        layoutMixerView();
    };
    mixerView_.addAndMakeVisible(toggleMasterPanelButton_);

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
    masterPanel_.addAndMakeVisible(masterSlider);
    masterLabel.attachToComponent(&masterSlider, true);

    // ---- master filter (stored in the document) ----
    filterButton.onClick = [this]
    {
        const bool on = filterButton.getToggleState();
        history_.mutableCurrent().filter.enabled = on;
        engine_.setMasterFilterEnabled(on);
    };
    masterPanel_.addAndMakeVisible(filterButton);

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
    masterPanel_.addAndMakeVisible(filterModeBox_);

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
    masterPanel_.addAndMakeVisible(filterCutoffSlider);

    filterResoSlider.setRange(0.1, 5.0, 0.01);
    filterResoSlider.setValue(0.707, juce::dontSendNotification);
    filterResoSlider.setTextValueSuffix(" Q");
    filterResoSlider.onValueChange = [this]
    {
        const float q = (float) filterResoSlider.getValue();
        history_.mutableCurrent().filter.resonance = q;
        engine_.setMasterFilterResonance(q);
    };
    masterPanel_.addAndMakeVisible(filterResoSlider);

    // ---- master delay (stored in the document, so it saves + restores) ----
    delayButton.onClick = [this]
    {
        const bool on = delayButton.getToggleState();
        history_.mutableCurrent().delay.enabled = on;
        engine_.setMasterDelayEnabled(on);
    };
    masterPanel_.addAndMakeVisible(delayButton);

    delayTimeSlider.setRange(20.0, 1000.0, 1.0);
    delayTimeSlider.setValue(300.0, juce::dontSendNotification);
    delayTimeSlider.setTextValueSuffix(" ms");
    delayTimeSlider.onValueChange = [this]
    {
        const float ms = (float) delayTimeSlider.getValue();
        history_.mutableCurrent().delay.timeMs = ms;
        engine_.setMasterDelayTimeMs(ms);
    };
    masterPanel_.addAndMakeVisible(delayTimeSlider);

    delayFbSlider.setRange(0.0, 95.0, 1.0);
    delayFbSlider.setValue(35.0, juce::dontSendNotification);
    delayFbSlider.setTextValueSuffix(" %");
    delayFbSlider.onValueChange = [this]
    {
        const float fb = (float) (delayFbSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.feedback = fb;
        engine_.setMasterDelayFeedback(fb);
    };
    masterPanel_.addAndMakeVisible(delayFbSlider);

    delayMixSlider.setRange(0.0, 100.0, 1.0);
    delayMixSlider.setValue(30.0, juce::dontSendNotification);
    delayMixSlider.setTextValueSuffix(" %");
    delayMixSlider.onValueChange = [this]
    {
        const float mix = (float) (delayMixSlider.getValue() / 100.0);
        history_.mutableCurrent().delay.mix = mix;
        engine_.setMasterDelayMix(mix);
    };
    masterPanel_.addAndMakeVisible(delayMixSlider);

    // ---- master reverb (stored in the document) ----
    reverbButton.onClick = [this]
    {
        const bool on = reverbButton.getToggleState();
        history_.mutableCurrent().reverb.enabled = on;
        engine_.setMasterReverbEnabled(on);
    };
    masterPanel_.addAndMakeVisible(reverbButton);

    reverbRoomSlider.setRange(0.0, 100.0, 1.0);
    reverbRoomSlider.setValue(50.0, juce::dontSendNotification);
    reverbRoomSlider.setTextValueSuffix(" room");
    reverbRoomSlider.onValueChange = [this]
    {
        const float v = (float) (reverbRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.roomSize = v;
        engine_.setMasterReverbRoomSize(v);
    };
    masterPanel_.addAndMakeVisible(reverbRoomSlider);

    reverbDampSlider.setRange(0.0, 100.0, 1.0);
    reverbDampSlider.setValue(50.0, juce::dontSendNotification);
    reverbDampSlider.setTextValueSuffix(" damp");
    reverbDampSlider.onValueChange = [this]
    {
        const float v = (float) (reverbDampSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.damping = v;
        engine_.setMasterReverbDamping(v);
    };
    masterPanel_.addAndMakeVisible(reverbDampSlider);

    reverbMixSlider.setRange(0.0, 100.0, 1.0);
    reverbMixSlider.setValue(30.0, juce::dontSendNotification);
    reverbMixSlider.setTextValueSuffix(" %");
    reverbMixSlider.onValueChange = [this]
    {
        const float v = (float) (reverbMixSlider.getValue() / 100.0);
        history_.mutableCurrent().reverb.mix = v;
        engine_.setMasterReverbMix(v);
    };
    masterPanel_.addAndMakeVisible(reverbMixSlider);

    // ---- send bus: a shared reverb-or-delay every track can send into (stored in the document) ----
    sendBusButton.onClick = [this]
    {
        const bool on = sendBusButton.getToggleState();
        history_.mutableCurrent().sendBus.enabled = on;
        engine_.setSendBusEnabled(on);
    };
    masterPanel_.addAndMakeVisible(sendBusButton);

    sendEffectTypeBox_.addItem("Reverb", 1);
    sendEffectTypeBox_.addItem("Delay", 2);
    sendEffectTypeBox_.setSelectedId(1, juce::dontSendNotification);
    sendEffectTypeBox_.onChange = [this]
    {
        const auto type = sendEffectTypeBox_.getSelectedId() == 2 ? model::SendBusEffectType::Delay
                                                                  : model::SendBusEffectType::Reverb;
        history_.mutableCurrent().sendBus.effectType = type;
        engine_.setSendBusEffectType((int) type);
        updateSendBusEffectVisibility();
    };
    masterPanel_.addAndMakeVisible(sendEffectTypeBox_);

    sendRoomSlider.setRange(0.0, 100.0, 1.0);
    sendRoomSlider.setValue(60.0, juce::dontSendNotification);
    sendRoomSlider.setTextValueSuffix(" room");
    sendRoomSlider.onValueChange = [this]
    {
        const float v = (float) (sendRoomSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.roomSize = v;
        engine_.setSendBusRoomSize(v);
    };
    masterPanel_.addAndMakeVisible(sendRoomSlider);

    sendDampSlider.setRange(0.0, 100.0, 1.0);
    sendDampSlider.setValue(40.0, juce::dontSendNotification);
    sendDampSlider.setTextValueSuffix(" damp");
    sendDampSlider.onValueChange = [this]
    {
        const float v = (float) (sendDampSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.damping = v;
        engine_.setSendBusDamping(v);
    };
    masterPanel_.addAndMakeVisible(sendDampSlider);

    sendDelayTimeSlider.setRange(20.0, 1000.0, 1.0);
    sendDelayTimeSlider.setValue(300.0, juce::dontSendNotification);
    sendDelayTimeSlider.setTextValueSuffix(" ms");
    sendDelayTimeSlider.onValueChange = [this]
    {
        const float ms = (float) sendDelayTimeSlider.getValue();
        history_.mutableCurrent().sendBus.delayTimeMs = ms;
        engine_.setSendBusDelayTimeMs(ms);
    };
    masterPanel_.addAndMakeVisible(sendDelayTimeSlider);

    sendDelayFbSlider.setRange(0.0, 95.0, 1.0);
    sendDelayFbSlider.setValue(35.0, juce::dontSendNotification);
    sendDelayFbSlider.setTextValueSuffix(" %");
    sendDelayFbSlider.onValueChange = [this]
    {
        const float fb = (float) (sendDelayFbSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.delayFeedback = fb;
        engine_.setSendBusDelayFeedback(fb);
    };
    masterPanel_.addAndMakeVisible(sendDelayFbSlider);

    sendReturnSlider.setRange(0.0, 100.0, 1.0);
    sendReturnSlider.setValue(50.0, juce::dontSendNotification);
    sendReturnSlider.setTextValueSuffix(" ret");
    sendReturnSlider.onValueChange = [this]
    {
        const float v = (float) (sendReturnSlider.getValue() / 100.0);
        history_.mutableCurrent().sendBus.returnLevel = v;
        engine_.setSendBusReturnLevel(v);
    };
    masterPanel_.addAndMakeVisible(sendReturnSlider);

    // ---- gain automation: arm, then move the master fader or a track's fader
    // while playing (Rec Auto arms both; Clr Auto clears both, the master lane
    // and the currently selected track's) ----
    autoRecButton.onClick   = [this] { recordAutomation_ = autoRecButton.getToggleState(); };
    autoClearButton.onClick = [this]
    {
        auto& song = history_.mutableCurrent();
        song.masterGainDb.clear();
        if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
            song.tracks[(size_t) selectedTrackIndex_].automation.clear(); // every parameter, not just gain
    };
    masterPanel_.addAndMakeVisible(autoRecButton);
    masterPanel_.addAndMakeVisible(autoClearButton);

    masterPanel_.addAndMakeVisible(meter_);

    // ---- per-track channel strips ----
    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip = new MixerStrip();
        strip->onGainChange = [this, i](float db) { setTrackGain(i, db); };
        strip->onMuteChange = [this, i](bool m)   { setTrackMuted(i, m); };
        strip->onSoloChange = [this, i](bool s)   { setTrackSolo(i, s); };
        strip->onSendChange = [this, i](float lv) { setTrackSendLevel(i, lv); };
        strip->onPanChange  = [this, i](float p)  { setTrackPan(i, p); };
        strip->onSelect     = [this, i]           { selectTrack(i); };
        trackStrips_.add(strip);
        mixerView_.addAndMakeVisible(strip);
    }

    mixerView_.onResized = [this] { layoutMixerView(); };

    pianoRoll_.onChange = [this](const engine::Pattern& p) { editPattern(p); };
    pianoRoll_.onNotePreview = [this](int noteNumber) { previewNote(noteNumber); };

    // ---- edit tab: a header showing which track/clip is open, and the piano
    // roll (which switches to pad-per-row drum mode for a Drum track — see
    // refreshPianoRollForSelected). Editing the kit itself lives in the
    // Drums pane instead. ----
    editingLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    editTab_.addAndMakeVisible(editingLabel_);

    // Pattern length of the open clip, in bars — how long the content loops
    // over, as opposed to the clip's window on the timeline (which the
    // arrangement's resize handle sets). Deliberately separate controls: they
    // are separate concepts, and resizing the window shouldn't silently
    // re-loop the notes inside it.
    barsLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
    barsLabel_.setJustificationType(juce::Justification::centredRight);
    editTab_.addAndMakeVisible(barsLabel_);

    for (int bars : { 1, 2, 4 })
        barsBox_.addItem(juce::String(bars), bars);
    barsBox_.onChange = [this] { setPatternBars(barsBox_.getSelectedId()); };
    editTab_.addAndMakeVisible(barsBox_);

    editTab_.addAndMakeVisible(pianoRoll_);
    editTab_.onResized = [this] { layoutEditTab(); };

    // ---- drums pane: the kit's sounds on the left, its rhythm on the right ----
    drumsPane_.connectCallbacks();
    drumsPane_.onSampleAssigned = [this](int padIndex, const juce::File& file) { assignDrumSample(padIndex, file); };
    drumsPane_.onPadMixChanged  = [this](int padIndex, const model::DrumPad& pad) { setDrumPadMix(padIndex, pad); };
    drumsPane_.onPadAdded       = [this] { addDrumPad(); };
    drumsPane_.onPadRemoved     = [this](int padIndex) { removeDrumPad(padIndex); };
    drumsPane_.onPatternChanged = [this](const engine::Pattern& p) { editPattern(p); };
    drumsPane_.onNotePreview    = [this](int noteNumber) { previewNote(noteNumber); };

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

    synthEditor_.onSettingsChanged = [this](const model::SynthSettings& s) { setTrackSynthSettings(s); };
    sessionView_.onLaunchClip  = [this](int track, int scene)
    {
        engine_.launchSessionSlot(track, scene);
        if (! engine_.isPlaying())
            post(Cmd::SetPlaying, 1.0); // launching implies you want to hear it
    };
    sessionView_.onLaunchScene = [this](int scene)
    {
        engine_.launchScene(scene);
        if (! engine_.isPlaying())
            post(Cmd::SetPlaying, 1.0);
    };
    sessionView_.onStopTrack   = [this](int track) { engine_.stopSessionSlot(track); };
    sessionView_.onStopAll     = [this] { engine_.stopAllSessionSlots(); };
    sessionView_.onAddScene    = [this] { addSessionScene(); };
    sessionView_.onDeleteScene = [this](int scene) { deleteSessionScene(scene); };
    sessionView_.onClipSelected = [this](int track, int scene) { captureClipIntoSession(track, scene); };

    // Clicking a fret sounds the note through the armed track, which for a
    // Guitar track is its GuitarNode — so the fretboard plays the same
    // instrument the sequencer does, including the one-note-per-string cut.
    fretboard_.onFretPlayed      = [this](int note) { previewNote(note); };
    fretboard_.onSettingsChanged = [this](const model::GuitarSettings& s) { setTrackGuitarSettings(s); };
    fretboard_.onChordStamped = [this](const engine::ChordShape& shape, int fretOffset,
                                       const engine::StrumSettings& strum)
    {
        stampChord(shape, fretOffset, strum);
    };
    fretboard_.onChordAtFret = [this](engine::MovableShape shape, int rootString, int fret,
                                      const engine::StrumSettings& strum, bool writeToClip)
    {
        playChordAtFret(shape, rootString, fret, strum, writeToClip);
    };

    effectChain_.onBuiltInAdded = [this](model::EffectKind kind) { addEffectSlot(kind, {}); };
    effectChain_.onPluginAdded  = [this](const engine::PluginEntry& entry)
    {
        model::PluginRef ref;
        ref.format     = entry.format == "VST3" ? model::PluginFormat::VST3
                       : entry.format == "AudioUnit" ? model::PluginFormat::AudioUnit
                                                     : model::PluginFormat::Unknown;
        ref.identifier = entry.identifier;
        ref.name       = entry.name;
        addEffectSlot(model::EffectKind::Plugin, ref);
    };
    effectChain_.onSlotRemoved          = [this](int slot) { removeEffectSlot(slot); };
    effectChain_.onSlotMoved            = [this](int slot, int delta) { moveEffectSlot(slot, delta); };
    effectChain_.onSlotBypassToggled    = [this](int slot, bool on) { setEffectSlotBypass(slot, on); };
    effectChain_.onSlotParamsChanged    = [this](const model::EffectSlot& s, int i) { setEffectSlotParams(s, i); };
    effectChain_.onScanRequested        = [this] { scanForPlugins(); };

    // A previous scan, so launching doesn't re-probe every plugin on the
    // machine — probing instantiates each one and is slow.
    engine_.pluginHost().restoreScanCache(settings_.getValue("pluginScanCache").toStdString());
    effectChain_.setAvailablePlugins(engine_.pluginHost().knownPlugins());
    effectChain_.onPluginEditorRequested = [this](int slot) { openPluginEditor(slot); };

    workspace_.registerPanel("Files", fileBrowser_);
    workspace_.registerPanel("Transport", leftPane_);
    workspace_.registerPanel("Tracks", arrangeTab_);
    workspace_.registerPanel("Keys", editTab_);
    workspace_.registerPanel("Synth", synthEditor_);
    workspace_.registerPanel("Drums", drumsPane_);
    workspace_.registerPanel("Guitar", fretboard_);
    workspace_.registerPanel("Session", sessionView_);
    workspace_.registerPanel("Track FX", effectChain_);
    workspace_.registerPanel("Mixer", mixerView_);
    workspace_.registerPanel("Keyboard", keyboard_);
    loadDockLayout(); // last session's arrangement, or the default one

    fileBrowser_.setRecordingsDirectory(recordingsDirectory());
    fileBrowser_.showDirectory(recordingsDirectory());
    fileBrowser_.onFilePreview = [this](const juce::File& file) { previewAudioFile(file); };

    {
        std::vector<juce::File> bookmarks;
        for (const auto& line : juce::StringArray::fromLines(settings_.getValue("fileBrowserBookmarks")))
            if (line.isNotEmpty())
                bookmarks.push_back(juce::File(line));
        fileBrowser_.setBookmarks(bookmarks);
    }
    fileBrowser_.onBookmarksChanged = [this]
    {
        juce::StringArray lines;
        for (const auto& dir : fileBrowser_.bookmarks())
            lines.add(dir.getFullPathName());
        settings_.setValue("fileBrowserBookmarks", lines.joinIntoString("\n"));
        settings_.saveIfNeeded();
    };

    {
        std::vector<juce::File> favorites;
        for (const auto& line : juce::StringArray::fromLines(settings_.getValue("fileBrowserFavorites")))
            if (line.isNotEmpty())
                favorites.push_back(juce::File(line));
        fileBrowser_.setFavorites(favorites);
    }
    fileBrowser_.onFavoritesChanged = [this]
    {
        juce::StringArray lines;
        for (const auto& f : fileBrowser_.favorites())
            lines.add(f.getFullPathName());
        settings_.setValue("fileBrowserFavorites", lines.joinIntoString("\n"));
        settings_.saveIfNeeded();
    };

    arrangementView_.onClipResized = [this](int trackIndex, int clipIndex, double newLengthBeats)
    {
        setClipLength(trackIndex, clipIndex, newLengthBeats);
    };

    arrangementView_.onFileDropped = [this](const juce::File& file, double dropBeat, int trackIndex)
    {
        importAudioFileAtBeat(file, dropBeat, trackIndex);
    };

    // Mirror the initial document into the engine + UI.
    syncEngineTracks();
    engine_.setArmedTrack(0);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
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

    addChildComponent(status_);

    // The document the app opens with counts as saved, so an untouched session
    // doesn't prompt on quit. This has to come *after* all the control setup
    // above: several of those updateXxxControls calls write through
    // mutableCurrent(), which advances the state id by design, so a marker
    // taken any earlier is stale by the time construction finishes.
    savedStateId_ = history_.stateId();

    for ([[maybe_unused]] const auto& shortcut : keys::all)
        jassert(shortcut.isValid());

    setWantsKeyboardFocus(true);
    setSize(900, 800);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    saveDockLayout();
    stopTimer();
    menuBar_.setModel(nullptr);
    engine_.deviceManager().removeChangeListener(this);
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "View" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (topLevelMenuIndex == 0) // File
    {
        addItem(menu, 1, "New Project", keys::newProject);
        addItem(menu, 2, "Open Project...", keys::open);
        addItem(menu, 3, "Save Project", keys::save,
                hasUnsavedChanges() || projectFile_ == juce::File{});
        addItem(menu, 24, "Save Project As...", keys::saveAs);
        menu.addSeparator();
        menu.addItem(4, "Import Audio...");
        menu.addItem(7, "Import Audio to Track...");
        menu.addItem(8, "Import MIDI...");
        menu.addItem(9, "Export MIDI...");
        addItem(menu, 5, "Bounce to WAV...", keys::bounce);
        menu.addSeparator();
        menu.addItem(13, "Set Project Root Folder...");
        menu.addSeparator();
        menu.addItem(6, "Audio Settings...");
    }
    else if (topLevelMenuIndex == 1) // Edit
    {
        addItem(menu, 10, withAction("Undo", history_.canUndo(), history_.undoLabel()),
                keys::undo, history_.canUndo());
        addItem(menu, 11, withAction("Redo", history_.canRedo(), history_.redoLabel()),
                keys::redo, history_.canRedo());
        menu.addSeparator();
        menu.addItem(12, "Clear Notes");
        menu.addSeparator();
        // Notes and clips get their own commands rather than one pair whose
        // meaning depends on which pane has focus.
        addItem(menu, 15, "Copy Notes", keys::copyNotes);
        addItem(menu, 16, "Paste Notes", keys::pasteNotes, ! noteClipboard_.empty());
        menu.addSeparator();
        addItem(menu, 17, "Copy Clip", keys::copyClip);
        addItem(menu, 18, "Paste Clip", keys::pasteClip, ! clipClipboard_.empty());
        addItem(menu, 19, "Duplicate Clip", keys::duplicate);
        addItem(menu, 25, "Delete Clip", keys::deleteClip);
        menu.addSeparator();
        menu.addItem(26, "Rename Track...");
        // The last track isn't deletable: a song with none has no pane that
        // can do anything, and no obvious way back.
        menu.addItem(27, "Delete Track", trackCount() > 1);
        menu.addSeparator();
        addItem(menu, 20, "Quantize", keys::quantize);
        menu.addItem(21, "Swing - Light");
        menu.addItem(22, "Swing - Medium");
        menu.addItem(23, "Swing - Heavy");
    }
    else if (topLevelMenuIndex == 2) // View
    {
        // Every pane, ticked when it's open. This is the only way back to a
        // pane once its tab has been closed, so the list is built from what
        // the workspace *knows about* rather than what's currently on screen.
        for (const auto& name : workspace_.registeredPanels())
        {
            const int id = kFirstPanelMenuId + panelMenuIndex(name);
            menu.addItem(id, name, true, workspace_.isPanelOpen(name));
        }

        menu.addSeparator();
        // Splitting is a drag gesture (drop a tab on a pane's edge), so the
        // only layout command left is a way back to the default.
        menu.addItem(14, "Reset Layout");
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
        case 24: saveProjectAs(); break;
        case 25: deleteSelectedClip(); break;
        case 26: renameSelectedTrack(); break;
        case 27: deleteSelectedTrack(); break;
        case 4:  chooseFile(); break; // import audio (preview player)
        case 5:  bounceProject(); break;
        case 6:  showAudioSettings(); break;
        case 7:  importAudioToNewTrack(); break;
        case 8:  importMidiFileDialog(); break;
        case 9:  exportMidiFileDialog(); break;
        case 10: history_.undo(); refreshFromModel(); break;
        case 11: history_.redo(); refreshFromModel(); break;
        case 12: pianoRoll_.clear(); break;
        case 13: setProjectRootFolderDialog(); break;
        case 14: buildDefaultDockLayout(); saveDockLayout(); break;
        case 15: copyNotes(); break;
        case 16: pasteNotes(); break;
        case 17: copyClip(); break;
        case 18: pasteClip(); break;
        case 19: duplicateClip(); break;
        case 20: quantizeNotes(0.0); break;
        case 21: quantizeNotes(0.25); break;
        case 22: quantizeNotes(0.5); break;
        case 23: quantizeNotes(0.66); break;

        // A View-menu panel entry. Opening an already-open pane would be a
        // no-op the user would read as broken, so togglePanel reveals a buried
        // one and closes one that's already in front.
        default:
            if (menuItemID >= kFirstPanelMenuId)
                togglePanel(menuItemID - kFirstPanelMenuId);
            break;
    }
}

/** Discards the current document for an empty one, asking first. */
void MainComponent::newProject()
{
    confirmDiscardChanges([this] { createEmptyProject(); });
}

void MainComponent::createEmptyProject()
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

    // A brand-new project has nothing worth saving yet, so it starts clean —
    // quitting straight after New shouldn't ask about it.
    projectFile_  = juce::File{};
    savedStateId_ = history_.stateId();
    updateWindowTitle();
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
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Same as addTrack(), but a Drum-type track (model::addTrack auto-populates
    its default Kick/Snare/Hat/Other pads — see model::makeDefaultDrumKit). */
void MainComponent::addDrumTrack()
{
    if (trackCount() >= engine_.maxTracks())
        return;

    history_.edit("Add drum track", [](model::Song& s)
    {
        const auto name = "Drums " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Drum, name.toStdString()).id;
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
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Writes a strummed chord into the open clip at the playhead.

    Real notes at real times, not a "strum" flag: the stagger between strings
    is most of what makes a chord sound like a hand rather than an organ, and
    putting it in the pattern keeps it visible and editable afterwards — the
    same choice §18's swing made, for the same reason. */
/** The selected track, if chords can go on it. Reports why not otherwise:
    every one of these used to be a silent return, which is indistinguishable
    from a broken button. */
const model::Track* MainComponent::guitarTrackForChords()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        showError("Select a guitar track first");
        return nullptr;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (track.type != model::TrackType::Guitar)
    {
        showError("\"" + juce::String(track.name) + "\" isn't a guitar track — chords need one");
        return nullptr;
    }

    return &track;
}

/** Bar length in beats, as the chord features measure it. */
double MainComponent::beatsPerBar() const
{
    return juce::jmax(1.0, uiTempoMap_.quartersPerBar());
}

/** Writes already-built notes into the selected guitar clip, at the bar the
    playhead is in. @p notes are positioned relative to the start of that bar,
    so callers don't need to know where it lands.

    Shared by the open-shape palette and by clicking the neck: those differ in
    which notes they produce, not in where the notes go or how that's
    reported. */
bool MainComponent::stampNotes(const std::vector<engine::Note>& notes, const juce::String& what)
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return false;

    const auto& track    = song.tracks[(size_t) selectedTrackIndex_];
    const int   trackIdx = selectedTrackIndex_;
    const int   clipIdx  = selectedClipIndex_;

    if (clipIdx < 0 || clipIdx >= (int) track.clips.size())
    {
        showError("\"" + juce::String(track.name) + "\" has no clip selected to put the chord in");
        return false;
    }

    // Land it on the bar the playhead is in, so stamping while stopped puts
    // the chord where the transport is rather than always at the start.
    const auto&  clip     = track.clips[(size_t) clipIdx];
    const double playhead = uiTempoMap_.ppqFromSamples(engine_.playheadSamples()) - clip.startBeats;
    const double wrapped  = clip.pattern.lengthBeats > 0.0
                              ? engine::wrapPositive(playhead, clip.pattern.lengthBeats) : 0.0;
    const double at       = std::floor(wrapped / beatsPerBar()) * beatsPerBar();

    int added = 0;
    history_.edit("Add chord", [trackIdx, clipIdx, &notes, &added, at](model::Song& s)
    {
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& pattern = clips[(size_t) clipIdx].pattern;
        for (auto note : notes)
        {
            note.startBeats += at;
            if (note.startBeats < pattern.lengthBeats)
            {
                pattern.notes.push_back(note);
                ++added;
            }
        }
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshFretboardForSelected();

    // Stamping writes notes into the clip; unless the transport happens to be
    // rolling over that bar, nothing moves and nothing sounds. Say what landed
    // and where, or a chord that worked looks exactly like one that didn't.
    if (added > 0)
        showStatus(what + " written at beat " + juce::String(at + 1.0, 2));
    else
        showError("No room for " + what + " in this clip");

    return added > 0;
}

/** Stamps one of the open shapes from the chord palette. */
void MainComponent::stampChord(const engine::ChordShape& shape, int fretOffset,
                               const engine::StrumSettings& strum)
{
    const auto* track = guitarTrackForChords();
    if (track == nullptr)
        return;

    stampNotes(engine::GuitarChords::strumChord(shape, track->guitarSettings.tuning.data(),
                                                fretOffset, 0.0, beatsPerBar(),
                                                history_.current().bpm, strum,
                                                (uint32_t) (chordStampSeed_++ | 1u)),
               juce::String(shape.name) + " chord");
}

/** A click on the neck with a chord mode selected: the shape rooted there is
    played, and written into the clip as well if the Write toggle is on.

    Playing is the default because the ask was to *play* chords by clicking the
    neck — a click that silently edited the document instead would be a
    surprising thing for a fretboard to do. Writing is one explicit toggle
    rather than a modifier key, so nothing about it is hidden. */
void MainComponent::playChordAtFret(engine::MovableShape shape, int rootString, int fret,
                                    const engine::StrumSettings& strum, bool writeToClip)
{
    const auto* track = guitarTrackForChords();
    if (track == nullptr)
        return;

    const auto notes = engine::GuitarChords::notesForRoot(shape, track->guitarSettings.tuning.data(),
                                                          rootString, fret);
    if (notes.empty())
    {
        showError(juce::String(engine::movableShapeName(shape)) + " doesn't fit there on the neck");
        return;
    }

    // Sounded through the same preview path a single fret click uses, so the
    // chord is played by the track's own GuitarNode — including its
    // one-note-per-string cut, which is what stops a chord from sounding like
    // six unrelated strings.
    for (int note : notes)
        previewNote(note);

    const juce::String what = juce::String(engine::movableShapeName(shape)) + " on "
                            + juce::String(engine::midiNoteName(notes.front()));

    if (! writeToClip)
    {
        showStatus(what);
        return;
    }

    stampNotes(engine::GuitarChords::strumRootedChord(
                   shape, track->guitarSettings.tuning.data(), rootString, fret,
                   0.0, beatsPerBar(), history_.current().bpm, strum,
                   (uint32_t) (chordStampSeed_++ | 1u)),
               what);
}

/** Same as addTrack(), but a Guitar-type track — six plucked strings in
    standard tuning (see model::GuitarSettings). */
void MainComponent::addGuitarTrack()
{
    if (trackCount() >= engine_.maxTracks())
        return;

    history_.edit("Add guitar track", [](model::Song& s)
    {
        const auto name = "Guitar " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Guitar, name.toStdString()).id;
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
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateEditingLabel();
}

/** Shows the fretboard for the selected track, or a placeholder if it isn't a
    Guitar track — the same gating the Synth and Drums panes use. */
void MainComponent::refreshFretboardForSelected()
{
    const bool isGuitar = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                        && history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Guitar;

    if (isGuitar)
        fretboard_.setSettings(history_.current().tracks[(size_t) selectedTrackIndex_].guitarSettings);
    else
        fretboard_.setNoGuitarTrackSelected();
}

/** Live tweak from the fretboard — document in place, then the engine, same
    as the mixer faders and the Synth pane. Tuning goes through the same path,
    since retuning a string is just another parameter to the model. */
void MainComponent::setTrackGuitarSettings(const model::GuitarSettings& settings)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.mutableCurrent().tracks[(size_t) index].guitarSettings = settings;

    engine_.setTrackGuitarSettings(index, settings.decaySeconds, settings.brightness,
                                   settings.pickPosition, settings.pickHardness, settings.muteOnNoteOff);
    engine_.setTrackGuitarTuning(index, settings.tuning);
}

/** Assigns @p file to pad @p padIndex of the currently selected track's drum
    kit (called from the drum-kit editor's Load... button or a file dropped
    onto one of its rows). A real document edit, so it goes through history_
    like any other content change. */
void MainComponent::assignDrumSample(int padIndex, const juce::File& file)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int  trackIdx = selectedTrackIndex_;
    const auto path      = file.getFullPathName().toStdString();

    history_.edit("Assign drum sample", [trackIdx, padIndex, path](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& pads = s.tracks[(size_t) trackIdx].drumKit.pads;
        if (padIndex >= 0 && padIndex < (int) pads.size())
            pads[(size_t) padIndex].samplePath = path;
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected(); // redraws the pad's row with its new sample name
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
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Redraws the session grid from the document. Which cells are *playing* is
    pushed separately from the engine each timer tick — see timerCallback —
    because a launch stays pending until the next bar line and the grid would
    otherwise light the wrong cell. */
void MainComponent::refreshSessionView()
{
    sessionView_.setSong(history_.current());
}

/** Adds a scene (a grid row), giving every track an empty slot in it. */
/** Removes a session row and every clip in it. Stops playback first: the
    slots the engine is holding are addressed by index, and the row below
    would inherit the index of the one that just went away. */
void MainComponent::deleteSessionScene(int sceneIndex)
{
    const auto& song = history_.current();
    if (sceneIndex < 0 || sceneIndex >= (int) song.scenes.size())
        return;

    engine_.stopAllSessionSlots();

    history_.edit("Delete scene", [sceneIndex](model::Song& s) { model::removeScene(s, sceneIndex); });

    syncEngineTracks();
    refreshSessionView();
}

void MainComponent::addSessionScene()
{
    history_.edit("Add scene", [](model::Song& s)
    {
        model::addScene(s, "Scene " + std::to_string(s.scenes.size() + 1));
    });

    syncEngineTracks();
    refreshSessionView();
}

/** Clicking an empty cell fills it with a copy of the track's currently open
    clip — the quickest way to get material into the grid without a separate
    "new session clip" flow. Does nothing if there's nothing to copy. */
void MainComponent::captureClipIntoSession(int trackIndex, int sceneIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) trackIndex].clips;
    if (clips.empty())
        return;

    const int  sourceIndex = juce::jlimit(0, (int) clips.size() - 1,
                                          trackIndex == selectedTrackIndex_ ? selectedClipIndex_ : 0);
    const auto source      = clips[(size_t) sourceIndex];

    history_.edit("Add session clip", [trackIndex, sceneIndex, &source](model::Song& s)
    {
        model::setSessionClip(s, trackIndex, sceneIndex, source);
    });

    syncEngineTracks();
    refreshSessionView();
}

/** Shows the selected track's insert effects. Unlike the Synth and Drums
    panes this applies to *every* track type — an audio track wants a filter
    as much as an instrument one does. */
void MainComponent::refreshEffectChainForSelected()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
    {
        effectChain_.setNoTrackSelected();
        return;
    }

    effectChain_.setChain(history_.current().tracks[(size_t) selectedTrackIndex_].effectChain);
}

/** Live tweak from the Track FX pane — updates the document in place (not a
    separate undo step per knob notch) and mirrors it into the engine, the
    same pattern the mixer faders and the Synth pane use. */
/** One chain slot's parameters in the engine's terms. `enabled` is the
    slot's own bypass, not the per-settings flag — bypass has to mean the same
    thing for a hosted plugin as for a built-in. */
static engine::EffectSlotParams toSlotParams(const model::EffectSlot& slot)
{
    engine::EffectSlotParams params;
    params.enabled         = slot.enabled;
    params.filterMode      = slot.filter.mode;
    params.filterCutoff    = slot.filter.cutoff;
    params.filterResonance = slot.filter.resonance;
    params.delayTimeMs     = slot.delay.timeMs;
    params.delayFeedback   = slot.delay.feedback;
    params.delayMix        = slot.delay.mix;
    params.reverbRoomSize  = slot.reverb.roomSize;
    params.reverbDamping   = slot.reverb.damping;
    params.reverbMix       = slot.reverb.mix;
    params.driveAmount     = slot.drive.drive;
    params.driveTone       = slot.drive.tone;
    params.driveLevel      = slot.drive.level;
    params.driveHardClip   = slot.drive.hardClip;
    params.driveCabinet    = slot.drive.cabinet;
    params.compThresholdDb = slot.compressor.thresholdDb;
    params.compRatio       = slot.compressor.ratio;
    params.compAttackMs    = slot.compressor.attackMs;
    params.compReleaseMs   = slot.compressor.releaseMs;
    params.compMakeUpDb    = slot.compressor.makeUpDb;
    params.tremoloRateHz   = slot.tremolo.rateHz;
    params.tremoloDepth    = slot.tremolo.depth;
    return params;
}

/** Adds a slot to the end of the selected track's chain. Structural, so it
    goes through history_ — and adding a plugin rebuilds the engine chain,
    which is what instantiates it. */
void MainComponent::addEffectSlot(model::EffectKind kind, const model::PluginRef& plugin)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Add effect", [index, kind, &plugin](model::Song& s)
    {
        model::EffectSlot slot;
        slot.kind    = kind;
        slot.enabled = true; // added because you want to hear it
        slot.plugin  = plugin;
        s.tracks[(size_t) index].effectChain.push_back(std::move(slot));
    });

    // Any open editor belongs to a node the rebuild is about to delete.
    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
}

void MainComponent::removeEffectSlot(int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Remove effect", [index, slotIndex](model::Song& s)
    {
        auto& chain = s.tracks[(size_t) index].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) chain.size())
            chain.erase(chain.begin() + slotIndex);
    });

    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
}

/** Moves a slot one place up or down. Order is the whole point of a chain, so
    this is a real document edit rather than a view-only sort. */
void MainComponent::moveEffectSlot(int slotIndex, int delta)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int index = selectedTrackIndex_;
    history_.edit("Reorder effects", [index, slotIndex, delta](model::Song& s)
    {
        auto&     chain  = s.tracks[(size_t) index].effectChain;
        const int target = slotIndex + delta;
        if (slotIndex < 0 || slotIndex >= (int) chain.size() || target < 0 || target >= (int) chain.size())
            return;
        std::swap(chain[(size_t) slotIndex], chain[(size_t) target]);
    });

    closePluginEditors();
    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
}

/** Bypass. Not structural — the node stays in the chain — so this is a live
    tweak straight into the document and the engine, with no rebuild and no
    plugin reinstantiation. */
void MainComponent::setEffectSlotBypass(int slotIndex, bool enabled)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    auto& chain = history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    chain[(size_t) slotIndex].enabled = enabled;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, toSlotParams(chain[(size_t) slotIndex]));
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
}

/** A knob turn on a built-in slot: live, non-undoable per notch, same as the
    mixer faders. */
void MainComponent::setEffectSlotParams(const model::EffectSlot& slot, int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    auto& chain = history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    chain[(size_t) slotIndex] = slot;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, toSlotParams(slot));
}

/** Probes for plugins and caches the result, so the next launch doesn't
    re-probe everything. In-process, so a plugin that crashes on probe takes
    the app with it — the dead man's pedal means it's skipped next time (see
    engine::PluginHost, and §20 for what's still owed here). */
void MainComponent::scanForPlugins()
{
    const auto pedal = recordingsDirectory().getParentDirectory().getChildFile("plugin-scan.tmp");

    for (const auto& format : engine_.pluginHost().availableFormats())
        engine_.pluginHost().scanFormat(format, pedal);

    settings_.setValue("pluginScanCache", juce::String(engine_.pluginHost().saveScanCache()));
    settings_.saveIfNeeded();

    effectChain_.setAvailablePlugins(engine_.pluginHost().knownPlugins());
    showStatus("Found " + juce::String((int) engine_.pluginHost().knownPlugins().size()) + " plugin(s)");
}

/** Opens a hosted plugin's own editor. */
void MainComponent::openPluginEditor(int slotIndex)
{
    auto* node = engine_.trackPluginNode(selectedTrackIndex_, slotIndex);
    if (node == nullptr || node->instance() == nullptr)
    {
        showError("That plugin isn't loaded on this machine");
        return;
    }

    // One window per plugin instance; re-opening focuses the existing one.
    for (auto* existing : pluginWindows_)
        if (existing->plugin() == node->instance())
        {
            existing->toFront(true);
            return;
        }

    auto* window = pluginWindows_.add(new PluginEditorWindow(node->instance()->getName(), *node->instance()));
    window->onCloseRequested = [this](PluginEditorWindow* w) { pluginWindows_.removeObject(w); };
}

/** Closes every plugin editor. Called before anything that rebuilds a chain,
    because the rebuild deletes the PluginNodes those editors are drawing —
    an editor outliving its processor is a crash, not a glitch. */
void MainComponent::closePluginEditors()
{
    pluginWindows_.clear();
}
/** Copies the piano roll's selected notes, or the whole pattern if nothing
    is selected — the same "no selection means everything" rule quantize
    uses, so both commands are useful before the selection gesture is
    discovered. */
void MainComponent::copyNotes()
{
    const auto& pattern   = currentPattern();
    const auto& selection = pianoRoll_.selectedNoteIndices();

    noteClipboard_.clear();
    if (selection.empty())
    {
        noteClipboard_ = pattern.notes;
    }
    else
    {
        for (int index : selection)
            if (index >= 0 && index < (int) pattern.notes.size())
                noteClipboard_.push_back(pattern.notes[(size_t) index]);
    }

    showStatus("Copied " + juce::String((int) noteClipboard_.size()) + " note(s)");
}

/** Pastes notes into the open clip at the positions they were copied from,
    which is what makes "copy this part into that clip" work. Anything past
    the destination pattern's end is dropped rather than pasted somewhere it
    can't be seen or heard. */
void MainComponent::pasteNotes()
{
    if (noteClipboard_.empty() || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;
    const auto notes   = noteClipboard_;

    history_.edit("Paste notes", [trackIdx, clipIdx, &notes](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& pattern = clips[(size_t) clipIdx].pattern;
        for (const auto& note : notes)
            if (note.startBeats < pattern.lengthBeats)
                pattern.notes.push_back(note);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
}

/** Copies the selected clip whole — pattern, length and all. */
void MainComponent::copyClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    clipClipboard_.assign(1, clips[(size_t) selectedClipIndex_]);
    showStatus("Copied clip");
}

/** Pastes onto the selected track at the playhead, snapped to a beat — the
    playhead is the one position the user can see, which makes where it lands
    predictable. */
void MainComponent::pasteClip()
{
    if (clipClipboard_.empty() || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const double dropBeat = std::round(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()));
    const int    trackIdx = selectedTrackIndex_;
    auto         pasted   = clipClipboard_.front();
    int          newIndex = -1;

    history_.edit("Paste clip", [trackIdx, dropBeat, &pasted, &newIndex](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& track  = s.tracks[(size_t) trackIdx];

        auto clip       = pasted;
        clip.id         = model::allocateId(s); // a paste is a new clip, not the same one twice
        clip.startBeats = juce::jmax(0.0, dropBeat);
        track.clips.push_back(std::move(clip));
        newIndex = (int) track.clips.size() - 1;
    });

    if (newIndex >= 0)
        selectedClipIndex_ = newIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Copy + paste in one step, landing the copy immediately after the original
    — the usual way to extend a part by a bar. */
/** Deletes the selected arrangement clip. No confirmation: undo is the safety
    net for editing actions, and a prompt on every delete is friction the user
    pays for on the many times they meant it. */
void MainComponent::deleteSelectedClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) track.clips.size())
        return;

    const int trackId   = track.id;
    const int clipIndex = selectedClipIndex_;

    history_.edit("Delete clip", [trackId, clipIndex](model::Song& s)
    {
        model::removeClip(s, trackId, clipIndex);
    });

    // The clip after the deleted one shuffles down into its index; selecting
    // it keeps the selection somewhere real, and clamps at the end.
    const auto& clips = history_.current().tracks[(size_t) selectedTrackIndex_].clips;
    selectedClipIndex_ = clips.empty() ? 0 : juce::jmin(clipIndex, (int) clips.size() - 1);

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Deletes the selected track and everything on it. Undo covers it, as with
    clips — but the last track isn't deletable, because a song with no tracks
    has no pane that can do anything and no obvious way back. */
void MainComponent::deleteSelectedTrack()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    if (song.tracks.size() <= 1)
        return;

    const int trackId = song.tracks[(size_t) selectedTrackIndex_].id;
    const int removed = selectedTrackIndex_;

    history_.edit("Delete track", [trackId](model::Song& s) { model::removeTrack(s, trackId); });

    selectTrackAndRefreshAll(juce::jmin(removed, trackCount() - 1));
}

/** Renames the selected track. Track names are the only label distinguishing
    one strip, row or tab from the next, and until now they were whatever the
    Add button happened to generate. */
void MainComponent::renameSelectedTrack()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& track   = song.tracks[(size_t) selectedTrackIndex_];
    const int   trackId = track.id;

    auto* window = new juce::AlertWindow("Rename Track", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", juce::String(track.name), "Name:");
    window->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<MainComponent>(this), window, trackId](int result)
            {
                if (self == nullptr || result != 1)
                    return;

                const auto name = window->getTextEditorContents("name").trim();
                if (name.isEmpty())
                    return; // an unnamed track is worse than the generated name

                self->history_.edit("Rename track", [trackId, name](model::Song& s)
                {
                    model::renameTrack(s, trackId, name.toStdString());
                });

                self->syncEngineTracks();
                self->updateMixerStrips();
                self->refreshSessionView();
                self->arrangementView_.setSong(self->history_.current());
                self->updateEditingLabel();
            }),
        true);
}

void MainComponent::duplicateClip()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const auto source   = clips[(size_t) selectedClipIndex_];
    const int  trackIdx = selectedTrackIndex_;
    int        newIndex = -1;

    history_.edit("Duplicate clip", [trackIdx, &source, &newIndex](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIdx];

        auto clip       = source;
        clip.id         = model::allocateId(s);
        clip.startBeats = source.startBeats + source.lengthBeats;
        track.clips.push_back(std::move(clip));
        newIndex = (int) track.clips.size() - 1;
    });

    if (newIndex >= 0)
        selectedClipIndex_ = newIndex;

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

/** Snaps the open clip's notes onto the grid, optionally swung. Acts on the
    piano roll's selection, or the whole pattern when nothing is selected. */
void MainComponent::quantizeNotes(double swingAmount)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const int  trackIdx  = selectedTrackIndex_;
    const int  clipIdx   = selectedClipIndex_;
    const auto selection = pianoRoll_.selectedNoteIndices();

    history_.edit(swingAmount > 0.0 ? "Swing" : "Quantize",
                  [trackIdx, clipIdx, swingAmount, &selection](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        // The grid the editor draws is 16ths, so that's what notes snap to.
        engine::NoteOps::quantizeNotes(clips[(size_t) clipIdx].pattern.notes, 0.25, swingAmount, selection);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();

    // Reloading the pattern clears the selection, which would silently widen
    // a follow-up Swing to the whole part. Quantizing never adds, removes or
    // reorders notes, so the same indices still mean the same notes.
    pianoRoll_.setSelectedNoteIndices(selection);
}

/** Sets a clip's window on the timeline (from the arrangement's resize
    handle). Note this is the window, not the pattern's loop length — see
    setPatternBars. A track holding a *single* clip is still given an
    unbounded window by syncEngineTracks (the long-standing "one clip plays
    until Stop" rule), so resizing a lone clip changes what you see and what
    gets exported, but not when it stops sounding; that only bites once the
    track has more than one clip. */
void MainComponent::setClipLength(int trackIndex, int clipIndex, double newLengthBeats)
{
    history_.edit("Resize clip", [trackIndex, clipIndex, newLengthBeats](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < (int) clips.size())
            clips[(size_t) clipIndex].lengthBeats = juce::jmax(1.0, newLengthBeats);
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Sets how many bars the open clip's pattern loops over. Growing the pattern
    also grows the clip's window if the window would otherwise be too short to
    contain it — keeping a clip able to hold its own content isn't the same as
    silently re-looping it, which is why the window is only ever grown here,
    never shrunk. */
void MainComponent::setPatternBars(int bars)
{
    if (bars <= 0 || selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const double beatsPerBar = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    const double lengthBeats = beatsPerBar * bars;
    const int    trackIdx    = selectedTrackIndex_;
    const int    clipIdx     = selectedClipIndex_;

    history_.edit("Set pattern length", [trackIdx, clipIdx, lengthBeats](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& clip = clips[(size_t) clipIdx];
        clip.pattern.lengthBeats = lengthBeats;
        clip.lengthBeats         = juce::jmax(clip.lengthBeats, lengthBeats);

        // Notes now past the end would be unreachable in the editor and
        // silent in the sequencer, so drop them rather than leave them
        // invisibly attached to the clip.
        auto& notes = clip.pattern.notes;
        notes.erase(std::remove_if(notes.begin(), notes.end(),
                                   [lengthBeats](const engine::Note& n) { return n.startBeats >= lengthBeats; }),
                    notes.end());
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    updateEditingLabel();
}

/** Mirrors the open clip's pattern length into the Bars box. */
void MainComponent::updateBarsControl()
{
    const double beatsPerBar = juce::jmax(1.0, uiTempoMap_.quartersPerBar());
    const auto&  pattern     = currentPattern();
    const int    bars        = juce::jmax(1, (int) std::llround(pattern.lengthBeats / beatsPerBar));

    // Only reflects lengths the box actually offers; an odd length set
    // elsewhere leaves it blank rather than silently rounding the clip.
    barsBox_.setSelectedId(bars == 1 || bars == 2 || bars == 4 ? bars : 0, juce::dontSendNotification);
}

/** model::PluginFormat -> the name JUCE's format manager uses. The document
    stores an enum so the file format doesn't depend on JUCE's spelling; this
    is the one place the two meet. */
static std::string pluginFormatName(model::PluginFormat format)
{
    switch (format)
    {
        case model::PluginFormat::VST3:      return "VST3";
        case model::PluginFormat::AudioUnit: return "AudioUnit";
        case model::PluginFormat::Unknown:   break;
    }
    return {};
}

/** Converts a track's model automation lanes into the engine's curve form.
    The engine can't use model::AutomationLane directly — `model` already
    depends on `engine`, so the dependency can't run both ways — and this is
    the single place the two representations meet, used by both live playback
    and the offline exporter. */
static engine::TrackAutomation toTrackAutomation(const model::Track& track)
{
    engine::TrackAutomation curves;

    auto copyLane = [&track](model::TrackParam param, engine::AutomationCurve& into)
    {
        if (const auto* lane = track.lane(param))
        {
            for (const auto& point : lane->points())
                into.addPoint(point.beat, point.value);
            into.sortPoints();
        }
    };

    copyLane(model::TrackParam::Gain, curves.gain);
    copyLane(model::TrackParam::Pan, curves.pan);
    copyLane(model::TrackParam::SendLevel, curves.sendLevel);
    return curves;
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

        // Audio clips -> the track's own audio-clip player. Each Audio-type
        // clip becomes one AudioClipSlot, gated to its own
        // [startBeats, startBeats+lengthBeats) window exactly like the
        // instrument clips above — a track's only audio clip keeps an
        // unbounded window (plays once from its start, the original
        // single-clip behaviour); real gating (silence between clips, and
        // after the last one) only applies once a track has more than one.
        // Unconditionally resubmitted every sync, same as instrument clips —
        // cheap, since AudioEngine caches decoded audio by file path (see
        // AudioEngine::setTrackAudioClips), so this never re-decodes a file
        // it's already loaded, even across tracks that share one.
        const int numAudioClips = (int) std::count_if(track.clips.begin(), track.clips.end(),
                                                       [](const model::Clip& c)
                                                       { return c.type == model::ClipType::Audio && ! c.audioFile.empty(); });

        std::vector<engine::AudioClipSpec> audioSpecs;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;

            engine::AudioClipSpec spec;
            spec.file        = juce::File(clip.audioFile);
            spec.startBeats  = clip.startBeats;
            spec.lengthBeats = numAudioClips == 1 ? 1.0e9 : clip.lengthBeats;
            audioSpecs.push_back(spec);
        }
        if (! audioSpecs.empty())
            engine_.setTrackAudioClips(i, audioSpecs);

        // Drum kit -> routes this track's notes to the drum sampler instead
        // of the synth (see InstrumentTrack::instrument — unlike audio
        // clips, the synth doesn't naturally stay silent without content, so
        // this has to be explicit). Unconditionally resubmitted every sync
        // for the same reason as the clip lists above: cheap, since
        // AudioEngine caches decoded samples by path.
        engine_.setTrackInstrument(i, track.type == model::TrackType::Drum   ? engine::TrackInstrument::Drum
                                    : track.type == model::TrackType::Guitar ? engine::TrackInstrument::Guitar
                                                                             : engine::TrackInstrument::Synth);

        if (track.type == model::TrackType::Guitar)
        {
            const auto& guitar = track.guitarSettings;
            engine_.setTrackGuitarSettings(i, guitar.decaySeconds, guitar.brightness,
                                           guitar.pickPosition, guitar.pickHardness, guitar.muteOnNoteOff);
            engine_.setTrackGuitarTuning(i, guitar.tuning);
        }
        if (track.type == model::TrackType::Drum)
        {
            // Pad solo is resolved here rather than on the audio thread: the
            // whole pad map is rebuilt and swapped on any kit change anyway,
            // so the engine only ever needs the effective mute. Same
            // "solo overrides, mute always wins" rule as track solo.
            const bool anyPadSoloed = std::any_of(track.drumKit.pads.begin(), track.drumKit.pads.end(),
                                                  [](const model::DrumPad& p) { return p.solo; });

            std::vector<engine::DrumPadSpec> padSpecs;
            for (const auto& pad : track.drumKit.pads)
            {
                engine::DrumPadSpec spec;
                spec.noteNumber     = pad.noteNumber;
                spec.file           = pad.samplePath.empty() ? juce::File() : juce::File(pad.samplePath);
                spec.gainDb         = pad.gainDb;
                spec.pan            = pad.pan;
                spec.pitchSemitones = pad.pitchSemitones;
                spec.muted          = pad.muted || (anyPadSoloed && ! pad.solo);
                padSpecs.push_back(spec);
            }
            engine_.setTrackDrumKit(i, padSpecs);
        }

        engine_.setTrackMuted(i, track.muted);
        engine_.setTrackSolo(i, track.solo);
        engine_.setTrackGainDb(i, track.gainDb);
        engine_.setTrackPan(i, track.pan);
        engine_.setTrackAutomation(i, toTrackAutomation(track));

        // The session grid's column for this track. Empty slots are submitted
        // too — the index is the scene, so the list has to stay aligned with
        // Song::scenes even where there's nothing to play.
        std::vector<engine::SessionSlotData> sessionSlots;
        sessionSlots.reserve(track.sessionSlots.size());
        for (const auto& slot : track.sessionSlots)
        {
            engine::SessionSlotData data;
            data.hasClip = slot.hasClip && slot.clip.type == model::ClipType::Instrument;
            if (data.hasClip)
                data.pattern = slot.clip.pattern;
            sessionSlots.push_back(std::move(data));
        }
        engine_.setTrackSessionSlots(i, sessionSlots);
        engine_.setTrackSendLevel(i, track.sendLevel);

        const auto& synth = track.synthSettings;
        engine_.setTrackSynthWaveform(i, synth.waveform);
        engine_.setTrackSynthAttackMs(i, synth.attackMs);
        engine_.setTrackSynthDecayMs(i, synth.decayMs);
        engine_.setTrackSynthSustain(i, synth.sustain);
        engine_.setTrackSynthReleaseMs(i, synth.releaseMs);
        engine_.setTrackSynthFilterEnabled(i, synth.filterEnabled);
        engine_.setTrackSynthFilterMode(i, synth.filterMode);
        engine_.setTrackSynthFilterCutoff(i, synth.filterCutoff);
        engine_.setTrackSynthFilterResonance(i, synth.filterResonance);
        engine_.setTrackSynthGainDb(i, synth.gainDb);

        // The chain's shape, in order. Only pushed when it actually changed —
        // rebuilding resets every tail in the chain, so an unrelated edit must
        // not glitch a delay (see AudioEngine::setTrackEffectChain).
        std::vector<engine::EffectSlotSpec> chainSpecs;
        chainSpecs.reserve(track.effectChain.size());
        for (const auto& slot : track.effectChain)
        {
            engine::EffectSlotSpec spec;
            switch (slot.kind)
            {
                case model::EffectKind::Filter: spec.kind = engine::EffectNodeKind::Filter; break;
                case model::EffectKind::Delay:  spec.kind = engine::EffectNodeKind::Delay;  break;
                case model::EffectKind::Reverb: spec.kind = engine::EffectNodeKind::Reverb; break;
                case model::EffectKind::Drive:  spec.kind = engine::EffectNodeKind::Drive;  break;
                case model::EffectKind::Compressor: spec.kind = engine::EffectNodeKind::Compressor; break;
                case model::EffectKind::Tremolo:    spec.kind = engine::EffectNodeKind::Tremolo;    break;
                case model::EffectKind::Plugin:
                    spec.kind             = engine::EffectNodeKind::Plugin;
                    spec.pluginFormat     = pluginFormatName(slot.plugin.format);
                    spec.pluginIdentifier = slot.plugin.identifier;
                    spec.pluginState      = slot.plugin.state;
                    break;
            }
            chainSpecs.push_back(std::move(spec));
        }
        // A rebuild destroys this track's nodes, hosted plugins included, so
        // any editor drawing one has to go first. Only on an actual rebuild —
        // closing plugin windows on every unrelated edit would be maddening.
        if (engine_.setTrackEffectChain(i, chainSpecs))
            closePluginEditors();

        // Parameters, one call per slot, addressed by position — a chain may
        // hold two filters, and "the filter" stops meaning anything then.
        for (size_t s = 0; s < track.effectChain.size(); ++s)
            engine_.setTrackEffectSlotParams(i, (int) s, toSlotParams(track.effectChain[s]));
    }
    engine_.setActiveTrackCount(n);

    // The arrangement just changed, so the loop it runs over has too. This is
    // the one place every clip edit passes through, which is why it lives
    // here rather than in each of them.
    updateLoopRegion();
}

void MainComponent::refreshPianoRollForSelected()
{
    pianoRoll_.setPattern(currentPattern());
    updateBarsControl();

    const bool isDrum = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                     && history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Drum;

    if (isDrum)
        pianoRoll_.setDrumPads(history_.current().tracks[(size_t) selectedTrackIndex_].drumKit.pads);
    else
        pianoRoll_.setMelodicMode();
}

/** Shows the Drums pane's kit and step grid for the selected track, or a
    placeholder if it isn't a Drum track — the same gating
    refreshSynthEditorForSelected does for Instrument tracks. */
void MainComponent::refreshDrumsPaneForSelected()
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
    {
        drumsPane_.setNoDrumTrackSelected();
        return;
    }

    drumsPane_.setKit(history_.current().tracks[(size_t) trackIndex].drumKit.pads, currentPattern());
}

/** The selected track's index if it's a Drum track, or -1 — the one check
    every drum-kit edit below needs before touching the document. */
int MainComponent::selectedDrumTrackIndex() const
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return -1;
    return history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Drum
               ? selectedTrackIndex_ : -1;
}

/** Live tweak of one pad's mute/solo/gain/pan/pitch — updates the document in
    place (not a separate undo step), same as a mixer fader. Deliberately does
    not rebuild the kit editor's rows: they hold the very slider being dragged
    (see DrumKitEditor::setPads); only the step grid, which dims muted pads,
    needs refreshing. */
void MainComponent::setDrumPadMix(int padIndex, const model::DrumPad& pad)
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
        return;

    auto& pads = history_.mutableCurrent().tracks[(size_t) trackIndex].drumKit.pads;
    if (padIndex < 0 || padIndex >= (int) pads.size())
        return;

    pads[(size_t) padIndex] = pad;
    syncEngineTracks(); // rebuilds this track's pad map with the new mix settings
    drumsPane_.refreshPadsForMixChange(pads);
}

/** Adds a pad to the selected kit, on the next free MIDI note above the
    highest one it already uses — a structural edit, so it goes through
    history_ like adding a track or clip. */
void MainComponent::addDrumPad()
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
        return;

    history_.edit("Add drum pad", [trackIndex](model::Song& s)
    {
        auto& pads = s.tracks[(size_t) trackIndex].drumKit.pads;

        int highestNote = 35; // one below the usual GM kick, so an empty kit starts at 36
        for (const auto& pad : pads)
            highestNote = juce::jmax(highestNote, pad.noteNumber);

        model::DrumPad pad;
        pad.noteNumber = juce::jmin(127, highestNote + 1);
        pad.label      = "Pad " + std::to_string(pads.size() + 1);
        pads.push_back(pad);
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
}

/** Removes a pad, along with any notes that triggered it — leaving orphaned
    hits behind would show up as a silent row nothing can play. Never removes
    the last pad (the editor's Remove button is disabled at one pad). */
void MainComponent::removeDrumPad(int padIndex)
{
    const int trackIndex = selectedDrumTrackIndex();
    if (trackIndex < 0)
        return;

    history_.edit("Remove drum pad", [trackIndex, padIndex](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        auto& pads  = track.drumKit.pads;
        if (padIndex < 0 || padIndex >= (int) pads.size() || pads.size() <= 1)
            return;

        const int removedNote = pads[(size_t) padIndex].noteNumber;
        pads.erase(pads.begin() + padIndex);

        for (auto& clip : track.clips)
        {
            auto& notes = clip.pattern.notes;
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [removedNote](const engine::Note& n) { return n.noteNumber == removedNote; }),
                        notes.end());
        }
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
}

/** Shows the Synth pane's controls for the selected track's timbre, or a
    placeholder if it's not an Instrument track (Drum/Audio tracks have no
    synth to edit) — the same is-it-this-track-type gating
    refreshPianoRollForSelected already does for the drum-kit editor. */
void MainComponent::refreshSynthEditorForSelected()
{
    const bool isInstrument = selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount()
                            && history_.current().tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Instrument;

    if (isInstrument)
        synthEditor_.setSettings(history_.current().tracks[(size_t) selectedTrackIndex_].synthSettings);
    else
        synthEditor_.setNoTrackSelected();
}

/** Live tweak from the Synth pane (a knob turn) — updates the current
    document in place, same non-undoable-per-notch pattern as setTrackGain,
    and mirrors it into the engine. */
void MainComponent::setTrackSynthSettings(const model::SynthSettings& settings)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    history_.mutableCurrent().tracks[(size_t) selectedTrackIndex_].synthSettings = settings;

    const int index = selectedTrackIndex_;
    engine_.setTrackSynthWaveform(index, settings.waveform);
    engine_.setTrackSynthAttackMs(index, settings.attackMs);
    engine_.setTrackSynthDecayMs(index, settings.decayMs);
    engine_.setTrackSynthSustain(index, settings.sustain);
    engine_.setTrackSynthReleaseMs(index, settings.releaseMs);
    engine_.setTrackSynthFilterEnabled(index, settings.filterEnabled);
    engine_.setTrackSynthFilterMode(index, settings.filterMode);
    engine_.setTrackSynthFilterCutoff(index, settings.filterCutoff);
    engine_.setTrackSynthFilterResonance(index, settings.filterResonance);
    engine_.setTrackSynthGainDb(index, settings.gainDb);
}

/** Briefly sounds @p noteNumber through whichever track is currently armed —
    the same live-MIDI path the on-screen keyboard already uses (see
    InstrumentTrack::render's receivesLiveMidi routing), so it plays through
    that track's actual instrument: the synth pitch for an Instrument track,
    or the matching pad's sample for a Drum track. Fired when clicking to add
    a note in the piano roll, so pitches (or pads) can be found by ear. */
void MainComponent::previewNote(int noteNumber)
{
    engine_.keyboardState().noteOn(1, noteNumber, 0.8f);

    // Guarded by a SafePointer rather than capturing `this` directly: the
    // note-off fires 150ms later, and quitting the app within that window
    // would otherwise run this lambda against a destroyed MainComponent (and
    // a destroyed engine). A dangling preview is easy to trigger — click a
    // note, close the window — and would crash on the way out.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Timer::callAfterDelay(150, [safeThis, noteNumber]
    {
        if (auto* self = safeThis.getComponent())
            self->engine_.keyboardState().noteOff(1, noteNumber, 0.8f);
    });
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
            strip->setPan(track.pan);
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
    sendEffectTypeBox_.setSelectedId(sb.effectType == model::SendBusEffectType::Delay ? 2 : 1,
                                     juce::dontSendNotification);
    sendRoomSlider.setValue(sb.roomSize * 100.0, juce::dontSendNotification);
    sendDampSlider.setValue(sb.damping * 100.0, juce::dontSendNotification);
    sendDelayTimeSlider.setValue(sb.delayTimeMs, juce::dontSendNotification);
    sendDelayFbSlider.setValue(sb.delayFeedback * 100.0, juce::dontSendNotification);
    sendReturnSlider.setValue(sb.returnLevel * 100.0, juce::dontSendNotification);
    updateSendBusEffectVisibility();

    engine_.setSendBusEnabled(sb.enabled);
    engine_.setSendBusEffectType((int) sb.effectType);
    engine_.setSendBusRoomSize(sb.roomSize);
    engine_.setSendBusDamping(sb.damping);
    engine_.setSendBusDelayTimeMs(sb.delayTimeMs);
    engine_.setSendBusDelayFeedback(sb.delayFeedback);
    engine_.setSendBusReturnLevel(sb.returnLevel);
}

void MainComponent::updateSendBusEffectVisibility()
{
    const bool isDelay = history_.current().sendBus.effectType == model::SendBusEffectType::Delay;
    sendRoomSlider.setVisible(! isDelay);
    sendDampSlider.setVisible(! isDelay);
    sendDelayTimeSlider.setVisible(isDelay);
    sendDelayFbSlider.setVisible(isDelay);
}

void MainComponent::setTrackGain(int index, float gainDb)
{
    // Live tweak: update the current document in place (not a separate undo step).
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.gainDb = gainDb;

        // The same global "Rec Auto" toggle arms every automatable per-track
        // parameter — touch whichever control you want to automate while it's
        // on (see also setTrackPan and setTrackSendLevel).
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::Gain)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), gainDb);
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

void MainComponent::setTrackPan(int index, float pan)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.pan = pan;
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::Pan)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), pan);
    }
    engine_.setTrackPan(index, pan);
}

void MainComponent::setTrackSendLevel(int index, float level)
{
    auto& song = history_.mutableCurrent();
    if (index >= 0 && index < (int) song.tracks.size())
    {
        auto& track = song.tracks[(size_t) index];
        track.sendLevel = level;
        if (recordAutomation_ && engine_.isPlaying())
            track.laneFor(model::TrackParam::SendLevel)
                 .addPoint(uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), level);
    }
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
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    updateMixerStrips(); // refreshes the selection highlight
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateEditingLabel();
}

void MainComponent::refreshFromModel()
{
    // The song's metre drives both tempo maps: the UI's (bar/beat readout,
    // bars-to-beats for pattern lengths) and the engine's (which decides
    // where the metronome's downbeat accent falls). Neither was ever told,
    // so both sat at 4/4 no matter what the document said.
    const auto& song = history_.current();
    uiTempoMap_.setTimeSignature(song.timeSigNumerator, song.timeSigDenominator);
    post(Cmd::SetTimeSignature, (double) song.timeSigNumerator, (double) song.timeSigDenominator);

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
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
    arrangementView_.setSong(history_.current());
    arrangementView_.setSelectedClip(selectedTrackIndex_, selectedClipIndex_);
    updateMixerStrips();
    updateDelayControls();
    updateFilterControls();
    updateReverbControls();
    updateSendBusControls();
    fileBrowser_.setProjectRootFolder(history_.current().projectRootFolder.empty()
                                          ? juce::File{}
                                          : juce::File(history_.current().projectRootFolder));
    updateEditingLabel();
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // Undo and redo refresh the whole UI from the model afterwards, so they
    // are handled apart from the commands that don't.
    if (key == keys::undo || key == keys::redo || key == keys::redoAlt)
    {
        const bool undoing = (key == keys::undo);
        const auto action  = undoing ? history_.undoLabel() : history_.redoLabel();
        const bool did     = undoing ? history_.canUndo() : history_.canRedo();

        if (undoing)
            history_.undo();
        else
            history_.redo();

        refreshFromModel();

        // The menu would have named the action; a keystroke has to say it
        // some other way, or undo is a silent jump the user has to diff.
        if (did)
            showStatus(withAction(undoing ? "Undo" : "Redo", true, action));

        return true;
    }

    // Transport shortcuts trigger the buttons rather than repeating what they
    // do: the button stays the single definition of the action, and it
    // visibly reacts — pressing space and seeing nothing move on screen reads
    // as a dropped keystroke.
    if (key == keys::playPause)  { playPauseButton.triggerClick();     return true; }
    if (key == keys::toStart)    { firstFrameButton.triggerClick();    return true; }
    if (key == keys::toEnd)      { lastFrameButton.triggerClick();     return true; }
    if (key == keys::backOneBar) { previousFrameButton.triggerClick(); return true; }
    if (key == keys::onOneBar)   { nextFrameButton.triggerClick();     return true; }
    if (key == keys::record)     { recordButton.triggerClick();        return true; }
    if (key == keys::loop)       { loopButton.triggerClick();          return true; }

    if (key == keys::newProject) { newProject();       return true; }
    if (key == keys::open)       { openProject();      return true; }
    if (key == keys::save)       { saveProject();      return true; }
    if (key == keys::saveAs)     { saveProjectAs();    return true; }
    if (key == keys::bounce)     { bounceProject();    return true; }

    if (key == keys::copyNotes)  { copyNotes();        return true; }
    if (key == keys::pasteNotes) { pasteNotes();       return true; }
    if (key == keys::copyClip)   { copyClip();         return true; }
    if (key == keys::pasteClip)  { pasteClip();        return true; }
    if (key == keys::duplicate)  { duplicateClip();    return true; }
    if (key == keys::quantize)   { quantizeNotes(0.0); return true; }
    if (key == keys::deleteClip) { deleteSelectedClip(); return true; }

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
        showError("Could not load: " + file.getFileName());
}

void MainComponent::importMidiFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import MIDI file", juce::File{}, "*.mid;*.midi");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        engine::MidiImportResult result;
        history_.edit("Import MIDI", [&file, &result](model::Song& s)
        {
            result = engine::importMidiFile(file, s);
        });

        if (! result.ok)
        {
            showError("Could not import: " + file.getFileName());
            return;
        }

        syncEngineTracks();
        arrangementView_.setSong(history_.current());
        updateMixerStrips();
        updateEditingLabel();

        auto msg = "Imported " + juce::String(result.tracksImported) + " track(s) at "
                 + juce::String(history_.current().bpm, 1) + " BPM";
        if (result.extraTempoEventsIgnored > 0)
            msg += " (" + juce::String(result.extraTempoEventsIgnored) + " further tempo change(s) not imported)";
        showStatus(msg);
    });
}

void MainComponent::exportMidiFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export MIDI file", juce::File{}, "*.mid");
    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
            return;
        file = file.withFileExtension("mid");

        const bool ok = engine::exportMidiFile(file, history_.current());
        if (ok)
            showStatus("Exported: " + file.getFileName());
        else
            showError("MIDI export failed (no instrument track has any notes)");
    });
}

void MainComponent::setProjectRootFolderDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Set project root folder", juce::File{});
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir == juce::File{} || ! dir.isDirectory())
            return;

        const auto path = dir.getFullPathName().toStdString();
        history_.edit("Set project root folder", [path](model::Song& s) { s.projectRootFolder = path; });

        fileBrowser_.setProjectRootFolder(dir);
        showStatus("Project root folder set to: " + dir.getFullPathName());
    });
}

/** Imports an audio file onto a brand-new Audio track (as its one clip, at
    beat 0), so it actually plays back as part of the mix — unlike "Import
    Audio..." above, which only feeds the disconnected global preview player. */
void MainComponent::importAudioToNewTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
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

/** Imports a file as an audio clip starting at @p startBeats — the shared
    machinery behind "Import Audio to Track..." (always beat 0, always a new
    track), and dragging a file from the file-browser pane onto the
    arrangement (beat = wherever it was dropped; @p targetTrackIndex = the
    track lane it landed on, or -1 for empty space below the tracks).

    Dropping onto an existing Audio-type track adds a clip there instead of
    creating a new track — the track keeps its single-clip unbounded window
    if it still only has one clip, or gets real per-clip length gating (see
    AudioFilePlayerNode) the moment it has more than one, exactly like
    instrument clips. Any other drop target (empty space, or a non-Audio
    track) creates a brand-new Audio track instead, as it always has. */
void MainComponent::importAudioFileAtBeat(const juce::File& file, double startBeats, int targetTrackIndex)
{
    const auto& song = history_.current();
    const bool  addToExistingTrack = targetTrackIndex >= 0 && targetTrackIndex < (int) song.tracks.size()
                                   && song.tracks[(size_t) targetTrackIndex].type == model::TrackType::Audio;

    // Size the clip to the file's real duration rather than a fixed guess —
    // display-only for a track's sole clip (unbounded window regardless), but
    // functionally gates playback the moment a track has more than one clip,
    // so guessing wrong there would audibly truncate the clip.
    const double durationSeconds = engine_.probeDurationSeconds(file);
    const double lengthBeats     = durationSeconds > 0.0 ? durationSeconds * song.bpm / 60.0 : 4.0;
    const auto   path            = file.getFullPathName().toStdString();

    if (addToExistingTrack)
    {
        int newClipIndex = -1;
        history_.edit("Add audio clip", [targetTrackIndex, &path, startBeats, lengthBeats, &newClipIndex](model::Song& s)
        {
            auto& track = s.tracks[(size_t) targetTrackIndex];

            model::Clip clip;
            clip.id          = model::allocateId(s);
            clip.type        = model::ClipType::Audio;
            clip.startBeats  = juce::jmax(0.0, startBeats);
            clip.lengthBeats = lengthBeats;
            clip.audioFile   = path;
            track.clips.push_back(clip);

            newClipIndex = (int) track.clips.size() - 1;
        });

        syncEngineTracks();
        selectTrackAndClip(targetTrackIndex, newClipIndex);
        arrangementView_.setSong(history_.current());
        showStatus("Imported: " + file.getFileName() + "  (added clip)");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    int newTrackIndex = -1;
    history_.edit("Import audio track", [&path, &newTrackIndex, startBeats, lengthBeats](model::Song& s)
    {
        const auto name = "Audio " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Audio, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(s);
        clip.type        = model::ClipType::Audio;
        clip.startBeats  = juce::jmax(0.0, startBeats);
        clip.lengthBeats = lengthBeats;
        clip.audioFile   = path;
        s.tracks.back().clips.push_back(clip);

        newTrackIndex = (int) s.tracks.size() - 1;
    });

    selectTrackAndRefreshAll(newTrackIndex);
    showStatus("Imported: " + file.getFileName() + "  (new track)");
}

/** Points the whole UI at a track: every pane that shows per-track state is
    refreshed from it. Used after adding a track and after deleting one, which
    is why it isn't named for either. */
void MainComponent::selectTrackAndRefreshAll(int newTrackIndex)
{
    if (newTrackIndex < 0)
        return;

    selectedTrackIndex_ = newTrackIndex;
    selectedClipIndex_  = 0;
    syncEngineTracks();
    engine_.setArmedTrack(selectedTrackIndex_);
    refreshPianoRollForSelected();
    refreshSynthEditorForSelected();
    refreshDrumsPaneForSelected();
    refreshEffectChainForSelected();
    refreshFretboardForSelected();
    refreshSessionView();
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
            showError("No audio input device available");
            return;
        }

        awaitingRecordedTake_ = true;
        recordButton.setToggleState(true, juce::dontSendNotification); // swaps to the stop square
        recordButton.setTooltip(withShortcut("Stop recording", keys::record));
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopRecording();
        post(Cmd::SetPlaying, 0.0);
        recordButton.setToggleState(false, juce::dontSendNotification); // back to the record disc
        recordButton.setTooltip(withShortcut("Record", keys::record));
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
        showError("Recording was empty (no input captured)");
        return;
    }

    const auto& takeBuffer = engine_.recordedTakeBuffer();
    juce::AudioBuffer<float> trimmed(takeBuffer.getNumChannels(), length);
    for (int ch = 0; ch < takeBuffer.getNumChannels(); ++ch)
        trimmed.copyFrom(ch, 0, takeBuffer, ch, 0, length);

    const auto file = recordingsDirectory().getNonexistentChildFile("Recording", ".wav");
    if (! engine::OfflineRenderer::writeWav(file, trimmed, engine_.sampleRate()))
    {
        showError("Failed to write recording");
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

    selectTrackAndRefreshAll(newTrackIndex);
    showStatus("Recorded: " + file.getFileName());
}

juce::File MainComponent::recordingsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Recordings");
    dir.createDirectory();
    return dir;
}

/** Puts a passing message on screen. Deliberately not routed through any
    pane: a pane can be collapsed or closed, and a report that lands somewhere
    invisible is worse than none — the user reads silence as success. */
void MainComponent::showStatus(const juce::String& message)
{
    status_.show(message, false);
}

/** As showStatus, for the messages that report something didn't work. Held
    longer and marked, since these are the ones worth being sure was seen. */
void MainComponent::showError(const juce::String& message)
{
    status_.show(message, true);
    juce::Logger::writeToLog("Status: " + message);
}

/** True while the document differs from what's on disk. Asks the history for
    the identity of the state it's holding rather than tracking a modified
    flag, so undoing back to the saved state reads as saved again — see
    History::stateId. */
bool MainComponent::hasUnsavedChanges() const
{
    return history_.stateId() != savedStateId_;
}

/** Puts the project's name and an unsaved marker in the title bar, which is
    the only place either is visible. Called every timer tick, so it compares
    before setting: DocumentWindow::setName repaints the frame. */
void MainComponent::updateWindowTitle()
{
    const juce::String name = (projectFile_ == juce::File{})
                                  ? juce::String("Untitled")
                                  : projectFile_.getFileNameWithoutExtension();

    const juce::String title = name + (hasUnsavedChanges() ? " *" : "") + " - Looper-Audio";
    if (title == windowTitle_)
        return;

    windowTitle_ = title;
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName(title);
}

/** The actual write, shared by Save and Save As. Marks the document clean
    against the state that was written — not whatever it becomes later — so an
    edit made while the file chooser was up still counts as unsaved. */
bool MainComponent::writeProjectTo(const juce::File& file)
{
    const auto stateWritten = history_.stateId();
    const std::string text  = model::serialize(history_.current());

    if (! file.replaceWithText(juce::String::fromUTF8(text.c_str())))
    {
        showError("Could not save " + file.getFileName());
        return false;
    }

    projectFile_   = file;
    savedStateId_  = stateWritten;
    updateWindowTitle();
    return true;
}

/** Saves over the project's own file, falling back to Save As the first time.
    @p onDone reports whether the document actually reached disk — the
    discard prompt needs to know, since a cancelled save must cancel whatever
    it was clearing the way for. */
void MainComponent::saveProject(std::function<void(bool)> onDone)
{
    if (projectFile_ == juce::File{})
    {
        saveProjectAs(std::move(onDone));
        return;
    }

    const bool saved = writeProjectTo(projectFile_);
    if (onDone)
        onDone(saved);
}

void MainComponent::saveProjectAs(std::function<void(bool)> onDone)
{
    chooser_ = std::make_unique<juce::FileChooser>("Save project", projectFile_, "*.looper");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync(flags, [this, onDone](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{})
        {
            if (onDone)
                onDone(false); // dismissed the chooser: nothing was saved
            return;
        }

        const bool saved = writeProjectTo(file.withFileExtension("looper"));
        if (onDone)
            onDone(saved);
    });
}

/** Runs @p onProceed once it's safe to throw the current document away,
    asking first if there's anything to lose. Everything that discards the
    document goes through here — New, Open, and quitting — so there is one
    place the question is asked and one place it can be got wrong.

    Cancel, and a Save the user backs out of, both simply drop @p onProceed:
    the destructive action doesn't happen. */
void MainComponent::confirmDiscardChanges(std::function<void()> onProceed)
{
    if (! hasUnsavedChanges())
    {
        if (onProceed)
            onProceed();
        return;
    }

    const juce::String name = (projectFile_ == juce::File{})
                                  ? juce::String("this project")
                                  : projectFile_.getFileName();

    juce::NativeMessageBox::showYesNoCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Unsaved changes",
        "Save changes to " + name + " before closing it?",
        this,
        juce::ModalCallbackFunction::create([self = juce::Component::SafePointer<MainComponent>(this),
                                             onProceed](int result)
        {
            if (self == nullptr)
                return; // the window went away while the box was up

            if (result == 1) // Yes: save first, and only then go ahead
            {
                self->saveProject([onProceed](bool saved) { if (saved && onProceed) onProceed(); });
            }
            else if (result == 2) // No: discard
            {
                if (onProceed)
                    onProceed();
            }
            // Cancel (0): stay exactly where we are.
        }));
}

void MainComponent::openProject()
{
    confirmDiscardChanges([this] { chooseProjectToOpen(); });
}

void MainComponent::chooseProjectToOpen()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open project", projectFile_, "*.looper");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        model::Song song;
        std::string error;
        if (! model::deserialize(file.loadFileAsString().toStdString(), song, &error))
        {
            showError("Could not open " + file.getFileName() + ": " + error);
            return;
        }

        history_.reset(song);
        selectedTrackIndex_ = 0;

        tempoSlider.setValue(song.bpm, juce::dontSendNotification);
        uiTempoMap_.setTempo(song.bpm);
        post(Cmd::SetTempo, song.bpm);
        refreshFromModel();

        projectFile_  = file;
        savedStateId_ = history_.stateId(); // what's on screen is what's on disk
        updateWindowTitle();
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

        // Sample-accurate per-track gain automation: only wire the callback up
        // when at least one track actually has a lane, so a project with none
        // renders through the exact same (untouched) fast path as before this
        // existed — no behaviour change for the common case.
        const bool anyTrackAutomated = std::any_of(song.tracks.begin(), song.tracks.end(),
                                                   [](const model::Track& t) { return t.hasAutomation(); });

        engine::OfflineRenderer::TrackAutomationList automationCurves;
        if (anyTrackAutomated)
            for (const auto& track : song.tracks)
                automationCurves.push_back(toTrackAutomation(track));

        auto buffer = engine::OfflineRenderer::render(patterns, gains, solos, clipStarts, sends,
                                                       song.sendBus.enabled, song.sendBus.roomSize,
                                                       song.sendBus.damping, song.sendBus.returnLevel,
                                                       bpm, sampleRate, 8.0, 512,
                                                       anyTrackAutomated ? &automationCurves : nullptr,
                                                       (int) song.sendBus.effectType,
                                                       song.sendBus.delayTimeMs, song.sendBus.delayFeedback);

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

        if (engine::OfflineRenderer::writeWav(file, buffer, sampleRate))
            showStatus("Bounced: " + file.getFileName());
        else
            showError("Bounce failed");
    });
}

/** Moves the playhead to @p beat, clamped at zero. */
void MainComponent::seekToBeat(double beat)
{
    const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    uiTempoMap_.setSampleRate(sampleRate);
    post(Cmd::Seek, (double) uiTempoMap_.samplesFromPpq(juce::jmax(0.0, beat)));
}

/** Steps the playhead by whole bars — what "previous/next frame" means in a
    DAW, where the musical unit is a bar rather than a video frame. */
void MainComponent::stepByBars(int bars)
{
    const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    uiTempoMap_.setSampleRate(sampleRate);

    const double current = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
    const double perBar  = juce::jmax(1.0, uiTempoMap_.quartersPerBar());

    // Snap to the bar line first, so stepping from mid-bar lands on a bar
    // rather than carrying the offset along.
    const double currentBar = std::floor(current / perBar + 1.0e-9);
    seekToBeat((currentBar + bars) * perBar);
}

/** The end of the song's content — the furthest point any clip reaches. Zero
    for an empty project, so "go to end" is simply "go to start" there. */
/** Stops the transport once it has played everything that was arranged.

    Without this the playhead runs on for ever past the last clip, playing
    silence — the arrangement has an end, so the transport should have one
    too. Looping is left alone: that is the case where running past the last
    clip is the whole point, and the engine wraps it in the audio thread.

    Checked on the UI timer rather than in the engine: 30Hz is a thirtieth of
    a second of overshoot on a transport that is playing silence by then, and
    it costs the audio thread nothing. */
void MainComponent::stopAtEndOfArrangement()
{
    if (! engine_.isPlaying() || loopButton.getToggleState() || awaitingRecordedTake_)
        return;

    const double end = songEndBeats();
    if (end <= 0.0)
        return; // nothing arranged: there is no end to stop at

    const double playhead = uiTempoMap_.ppqFromSamples(engine_.playheadSamples());
    if (playhead < end)
        return;

    post(Cmd::SetPlaying, 0.0);
    showStatus("Reached the end of the arrangement");
}

double MainComponent::songEndBeats() const
{
    double end = 0.0;
    for (const auto& track : history_.current().tracks)
        for (const auto& clip : track.clips)
            end = juce::jmax(end, clip.startBeats + clip.lengthBeats);
    return end;
}

/** The loop runs over what has actually been arranged, rounded up to a bar.

    It used to be a hardcoded four bars whatever the song contained, so
    arranging anything longer than that silently looped only its opening —
    and arranging less looped several bars of nothing. */
void MainComponent::updateLoopRegion()
{
    const double sampleRate = engine_.sampleRate();
    if (sampleRate <= 0.0)
        return;

    uiTempoMap_.setSampleRate(sampleRate);

    const auto endSamples = (int64_t) std::llround(loopEndBeats() * uiTempoMap_.samplesPerBeat());
    post(Cmd::SetLoopRegion, 0.0, (double) endSamples);
}

/** Where the arrangement ends, rounded up to a whole bar — an empty song
    still gets one bar, so the loop is never zero-length. */
double MainComponent::loopEndBeats() const
{
    return engine::loopEndForContent(songEndBeats(), juce::jmax(1.0, uiTempoMap_.quartersPerBar()));
}

void MainComponent::timerCallback()
{
    engine_.pump();
    finishRecordingIfReady();
    updateWindowTitle();
    stopAtEndOfArrangement();

    addTrackButton.setEnabled(trackCount() < engine_.maxTracks());
    addDrumTrackButton_.setEnabled(trackCount() < engine_.maxTracks());
    addClipButton_.setEnabled(selectedTrackIndex_ >= 0 && selectedTrackIndex_ < trackCount());

    const double sampleRate = engine_.sampleRate();
    uiTempoMap_.setSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0);

    const int64_t playhead = engine_.playheadSamples();
    const auto    bb       = uiTempoMap_.barsBeatsFromSamples(playhead);
    const double  seconds  = sampleRate > 0.0 ? (double) playhead / sampleRate : 0.0;

    // "COUNT-IN" rather than "PLAYING" while the click is counting you in —
    // the transport is rolling but nothing is being captured yet, and that
    // distinction is the whole point of the feature.
    const char* transportState = engine_.isCountingIn() ? "COUNT-IN"
                               : engine_.isPlaying()    ? "PLAYING"
                                                        : "STOPPED";
    // The transport also starts and stops from elsewhere (clip launches, the
    // menu), so the glyph follows the engine rather than the last click.
    playPauseButton.setToggleState(engine_.isPlaying(), juce::dontSendNotification);

    positionLabel.setText(juce::String::formatted("Bar %d  Beat %d   |   %.2f s   |   %s",
                                                  bb.bar, bb.beat, seconds, transportState),
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

    // Which session cells are actually sounding comes from the engine, not the
    // document: a launch is pending until the next bar line, so the grid would
    // light the wrong cell if it guessed.
    {
        std::vector<int> playingSlots((size_t) n);
        for (int i = 0; i < n; ++i)
            playingSlots[(size_t) i] = engine_.sessionSlotPlaying(i);
        sessionView_.setPlayingSlots(playingSlots);
    }

    // What the guitar is actually sounding, per string. Read from the engine
    // rather than inferred: a string keeps ringing after its note-off, so the
    // document can't say which notes are live.
    {
        std::array<int, model::kNumGuitarStrings> ringing {};
        for (int s = 0; s < model::kNumGuitarStrings; ++s)
            ringing[(size_t) s] = engine_.guitarNoteOnString(selectedTrackIndex_, s);
        fretboard_.setRingingNotes(ringing);
    }

    // The step grid's playhead walks the pattern's own loop, so it needs the
    // position relative to the open clip's start rather than the song's.
    {
        const auto&  song      = history_.current();
        double       clipStart = 0.0;
        if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size())
        {
            const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
            if (selectedClipIndex_ >= 0 && selectedClipIndex_ < (int) clips.size())
                clipStart = clips[(size_t) selectedClipIndex_].startBeats;
        }
        drumsPane_.setPlayheadBeats(uiTempoMap_.ppqFromSamples(playhead) - clipStart, engine_.isPlaying());
    }

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

        // Per-track automation is *applied* by the engine now (each track
        // ramps its own curves across every block, see InstrumentTrack), so
        // this only moves the controls to follow along. Pushing values from
        // here as well would fight the engine and re-introduce the 30Hz
        // stepping this replaced.
        for (int i = 0; i < n; ++i)
        {
            const auto& track = song.tracks[(size_t) i];

            if (const auto* lane = track.lane(model::TrackParam::Gain))
                trackStrips_[i]->setGainDb(lane->valueAt(beat, track.gainDb));
            if (const auto* lane = track.lane(model::TrackParam::Pan))
                trackStrips_[i]->setPan(lane->valueAt(beat, track.pan));
            if (const auto* lane = track.lane(model::TrackParam::SendLevel))
                trackStrips_[i]->setSendLevel(lane->valueAt(beat, track.sendLevel));
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
    workspace_.setBounds(full); // the workspace lays its own tree out from here

    // Sits over the workspace, against the bottom of the window.
    status_.updateBounds();
}

void MainComponent::layoutLeftPane()
{
    auto area = leftPane_.getLocalBounds().reduced(12);

    auto row1 = area.removeFromTop(30);

    // Taken off the right first, so it stays pinned to the far edge whatever
    // width the pane has.
    collapseTransportButton_.setBounds(row1.removeFromRight(28).reduced(2));
    row1.removeFromRight(8);

    // First / previous / play-pause / next / last, in that order. The
    // frame-step glyphs are wider than tall, play/pause is taller than wide,
    // so they get different widths to keep the drawn glyphs a similar size.
    firstFrameButton.setBounds(row1.removeFromLeft(32).reduced(2));
    previousFrameButton.setBounds(row1.removeFromLeft(26).reduced(2));
    playPauseButton.setBounds(row1.removeFromLeft(30).reduced(3, 1));
    nextFrameButton.setBounds(row1.removeFromLeft(26).reduced(2));
    lastFrameButton.setBounds(row1.removeFromLeft(32).reduced(2));
    row1.removeFromLeft(12);
    loopButton.setBounds(row1.removeFromLeft(60));
    row1.removeFromLeft(12);
    recordButton.setBounds(row1.removeFromLeft(30).reduced(1)); // square: the icon is 25x25
    row1.removeFromLeft(12);
    metronomeButton.setBounds(row1.removeFromLeft(64));
    row1.removeFromLeft(6);
    countInBox_.setBounds(row1.removeFromLeft(110).reduced(0, 2));
    area.removeFromTop(8);

    if (transportCollapsed_)
        return; // nothing below the button row is showing

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
}

/** Shows or hides everything below the transport's button row. The arrow
    points the way the content will go, so it reads the same whichever state
    it's in. */
void MainComponent::applyTransportCollapse()
{
    juce::Component* belowFirstRow[] = { &positionLabel, &clipLabel, &tempoSlider };
    for (auto* c : belowFirstRow)
        c->setVisible(! transportCollapsed_);

    collapseTransportButton_.setButtonText(transportCollapsed_ ? "v" : "^");
    collapseTransportButton_.setTooltip(transportCollapsed_ ? "Show tempo and position"
                                                            : "Hide tempo and position");
    layoutLeftPane();
}

/** Index of @p name in the workspace's panel list — the offset that turns a
    View-menu id back into a panel. Both directions go through
    registeredPanels(), so the mapping can't drift as panes are added. */
int MainComponent::panelMenuIndex(const juce::String& name) const
{
    const auto names = workspace_.registeredPanels();
    for (int i = 0; i < (int) names.size(); ++i)
        if (names[(size_t) i] == name)
            return i;
    return 0;
}

/** Opens, reveals or closes the panel at @p index in the workspace's list. */
void MainComponent::togglePanel(int index)
{
    const auto names = workspace_.registeredPanels();
    if (index < 0 || index >= (int) names.size())
        return;

    const auto& name = names[(size_t) index];

    if (! workspace_.isPanelOpen(name))
        workspace_.openPanel(name);
    else if (workspace_.isPanelActive(name))
        workspace_.closePanel(name);   // already in front: the click means close
    else
        workspace_.revealPanel(name);  // open but buried: bring it forward first

    saveDockLayout();
}

/** The arrangement the app ships with, and what "Reset Layout" restores.
    Built with the same split/add operations a user's drags produce, so there
    is nothing special about it — Files down the left, the arrangement above
    the note editor in the middle, the mixer on the right, and the transport
    plus keyboard across the bottom. Several panels are therefore visible at
    once out of the box; the rest (Synth, Drums) start as tabs alongside the
    ones they relate to. */
void MainComponent::buildDefaultDockLayout()
{
    workspace_.resetToSingleRegion();

    // Splitting `centre` repeatedly is safe: a split moves the region into a
    // deeper node but never moves the DockRegion object itself, so the
    // reference stays good throughout.
    auto& centre = workspace_.rootRegion();
    workspace_.addPanel(centre, "Tracks");

    if (auto* left = workspace_.splitRegion(centre, DropZone::Left, 0.18))
        workspace_.addPanel(*left, "Files");

    if (auto* right = workspace_.splitRegion(centre, DropZone::Right, 0.72))
        workspace_.addPanel(*right, "Mixer");

    if (auto* bottom = workspace_.splitRegion(centre, DropZone::Bottom, 0.45))
    {
        workspace_.addPanel(*bottom, "Keys");
        workspace_.addPanel(*bottom, "Synth");
        workspace_.addPanel(*bottom, "Drums");
        workspace_.addPanel(*bottom, "Track FX");
        workspace_.addPanel(*bottom, "Session");
        workspace_.addPanel(*bottom, "Guitar");
        bottom->showPanel("Keys");

        if (auto* transport = workspace_.splitRegion(*bottom, DropZone::Bottom, 0.68))
        {
            workspace_.addPanel(*transport, "Transport");
            workspace_.addPanel(*transport, "Keyboard");
            transport->showPanel("Transport");
        }
    }
}

void MainComponent::loadDockLayout()
{
    // A saved layout that no longer parses — an older format, or one naming a
    // panel this build doesn't have — falls back to the default rather than
    // leaving a half-built workspace.
    if (! workspace_.restoreLayout(settings_.getValue("dockLayout")))
        buildDefaultDockLayout();
}

void MainComponent::saveDockLayout()
{
    settings_.setValue("dockLayout", workspace_.saveLayout());
    settings_.saveIfNeeded();
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
    auto area   = editTab_.getLocalBounds();
    auto header = area.removeFromTop(22);
    barsBox_.setBounds(header.removeFromRight(56).reduced(2, 0));
    barsLabel_.setBounds(header.removeFromRight(34));
    editingLabel_.setBounds(header.reduced(6, 0));
    pianoRoll_.setBounds(area);
}

void MainComponent::layoutMixerView()
{
    auto area = mixerView_.getLocalBounds().reduced(10);
    if (area.isEmpty())
        return;

    auto toolbar = area.removeFromTop(28);
    addTrackButton.setBounds(toolbar.removeFromLeft(100));
    toolbar.removeFromLeft(6);
    addDrumTrackButton_.setBounds(toolbar.removeFromLeft(100));
    toolbar.removeFromLeft(6);
    addGuitarTrackButton_.setBounds(toolbar.removeFromLeft(100));
    toolbar.removeFromLeft(6);
    toggleMasterPanelButton_.setBounds(toolbar.removeFromRight(110));
    area.removeFromTop(8);

    // The master panel (fader/automation, filter, delay, reverb, send bus,
    // meter) is collapsible — see toggleMasterPanelButton_ — since at
    // narrow widths its fixed 300px was clipping against the track strips.
    // Hidden, it claims no width at all, so every track strip gets more room.
    if (masterPanelVisible_)
    {
        auto masterArea = area.removeFromRight(300);
        area.removeFromRight(12);
        masterPanel_.setBounds(masterArea); // triggers layoutMasterPanel() via onResized
    }

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

void MainComponent::layoutMasterPanel()
{
    auto masterArea = masterPanel_.getLocalBounds();

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
    sendEffectTypeBox_.setBounds(sendRow.removeFromLeft(80));
    sendRow.removeFromLeft(8);
    const int sw           = juce::jmax(50, (sendRow.getWidth() - 16) / 3);
    const auto param1Bounds = sendRow.removeFromLeft(sw);
    sendRow.removeFromLeft(8);
    const auto param2Bounds = sendRow.removeFromLeft(sw);
    sendRow.removeFromLeft(8);
    // Reverb (room/damp) and delay (time/feedback) share the same two slots —
    // only one pair is visible at a time (see updateSendBusEffectVisibility).
    sendRoomSlider.setBounds(param1Bounds);
    sendDampSlider.setBounds(param2Bounds);
    sendDelayTimeSlider.setBounds(param1Bounds);
    sendDelayFbSlider.setBounds(param2Bounds);
    sendReturnSlider.setBounds(sendRow);
    masterArea.removeFromTop(8);

    meter_.setBounds(masterArea.removeFromTop(44));
}

} // namespace looper
