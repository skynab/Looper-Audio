#include "MainComponent.h"

#include "ScrollFollow.h"
#include "Shortcuts.h"

#include "Icons.h"

#include "engine/ClipSlot.h"
#include "engine/DefaultContent.h"
#include "engine/DrumSynth.h"
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

/** Colour entries in the per-track gear menu, clear of that menu's own
    fixed items. */
static constexpr int kFirstColourMenuId = 200;

/** How close to the edge the playhead gets before the keys grid pages. Small,
    so almost the whole width is travelled before each jump. */
static constexpr int kKeysFollowMargin = 24;

/** The time signatures offered. A fixed list because these are the ones
    people write in; a free numerator and denominator invites 4/7, which the
    rest of the app would have to have an opinion about. */
struct TimeSignatureOption { int numerator, denominator; };

static constexpr TimeSignatureOption kTimeSignatures[] = {
    { 4, 4 }, { 3, 4 }, { 2, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 12, 8 },
};

static constexpr int kNumTimeSignatures = (int) (sizeof(kTimeSignatures) / sizeof(kTimeSignatures[0]));

using Cmd = engine::EngineCommand::Type;



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

    // ---- document: a starter synth track plus a drum track with a
    // programmed loop and real sounds, so a fresh launch is audible
    // immediately rather than opening on silence ----
    {
        seedFactoryDrumKit();
        history_.reset(makeStarterSong());
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
            // At the end of the arrangement, play starts it again rather than
            // resuming into silence and stopping immediately — which is what
            // it would otherwise do, and would read as a dead button.
            if (engine::shouldRestartFromStart(
                    uiTempoMap_.ppqFromSamples(engine_.playheadSamples()), songEndBeats()))
            {
                seekToBeat(0.0);
            }

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
    loopButton.setTooltip(withShortcut("Loop over what's arranged", keys::loop));
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
    // Time signature. A fixed list rather than two spin boxes: these are the
    // ones anyone actually writes in, and a free numerator invites 4/7.
    for (int i = 0; i < kNumTimeSignatures; ++i)
    {
        const auto& sig = kTimeSignatures[i];
        timeSigBox_.addItem(juce::String(sig.numerator) + "/" + juce::String(sig.denominator), i + 1);
    }
    timeSigBox_.setTooltip("Time signature - sets the bar length, and the grid in the Tracks and Keys panes");
    timeSigBox_.onChange = [this]
    {
        const int index = timeSigBox_.getSelectedId() - 1;
        if (index >= 0 && index < kNumTimeSignatures)
            setTimeSignature(kTimeSignatures[index].numerator, kTimeSignatures[index].denominator);
    };
    leftPane_.addAndMakeVisible(timeSigBox_);
    timeSigLabel_.setText("Time", juce::dontSendNotification);
    timeSigLabel_.attachToComponent(&timeSigBox_, true);

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
        history_.edit(on ? "Enable master filter" : "Disable master filter",
                      [on](model::Song& s) { s.filter.enabled = on; });
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
        history_.edit("Set master filter mode", [mode](model::Song& s) { s.filter.mode = mode; });
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
    wireUndoableSlider(filterCutoffSlider, "Set master filter cutoff",
                       [](const model::Song& s) { return s.filter.cutoff; },
                       [](model::Song& s, float v) { s.filter.cutoff = v; });
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
    wireUndoableSlider(filterResoSlider, "Set master filter resonance",
                       [](const model::Song& s) { return s.filter.resonance; },
                       [](model::Song& s, float v) { s.filter.resonance = v; });
    masterPanel_.addAndMakeVisible(filterResoSlider);

    // ---- master delay (stored in the document, so it saves + restores) ----
    delayButton.onClick = [this]
    {
        const bool on = delayButton.getToggleState();
        history_.edit(on ? "Enable master delay" : "Disable master delay",
                      [on](model::Song& s) { s.delay.enabled = on; });
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
    wireUndoableSlider(delayTimeSlider, "Set master delay time",
                       [](const model::Song& s) { return s.delay.timeMs; },
                       [](model::Song& s, float v) { s.delay.timeMs = v; });
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
    wireUndoableSlider(delayFbSlider, "Set master delay feedback",
                       [](const model::Song& s) { return s.delay.feedback; },
                       [](model::Song& s, float v) { s.delay.feedback = v; });
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
    wireUndoableSlider(delayMixSlider, "Set master delay mix",
                       [](const model::Song& s) { return s.delay.mix; },
                       [](model::Song& s, float v) { s.delay.mix = v; });
    masterPanel_.addAndMakeVisible(delayMixSlider);

    // ---- master reverb (stored in the document) ----
    reverbButton.onClick = [this]
    {
        const bool on = reverbButton.getToggleState();
        history_.edit(on ? "Enable master reverb" : "Disable master reverb",
                      [on](model::Song& s) { s.reverb.enabled = on; });
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
    wireUndoableSlider(reverbRoomSlider, "Set master reverb room size",
                       [](const model::Song& s) { return s.reverb.roomSize; },
                       [](model::Song& s, float v) { s.reverb.roomSize = v; });
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
    wireUndoableSlider(reverbDampSlider, "Set master reverb damping",
                       [](const model::Song& s) { return s.reverb.damping; },
                       [](model::Song& s, float v) { s.reverb.damping = v; });
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
    wireUndoableSlider(reverbMixSlider, "Set master reverb mix",
                       [](const model::Song& s) { return s.reverb.mix; },
                       [](model::Song& s, float v) { s.reverb.mix = v; });
    masterPanel_.addAndMakeVisible(reverbMixSlider);

    // ---- send bus: a shared reverb-or-delay every track can send into (stored in the document) ----
    sendBusButton.onClick = [this]
    {
        const bool on = sendBusButton.getToggleState();
        history_.edit(on ? "Enable send bus" : "Disable send bus",
                      [on](model::Song& s) { s.sendBus.enabled = on; });
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
        history_.edit("Set send bus type", [type](model::Song& s) { s.sendBus.effectType = type; });
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
    wireUndoableSlider(sendRoomSlider, "Set send bus room size",
                       [](const model::Song& s) { return s.sendBus.roomSize; },
                       [](model::Song& s, float v) { s.sendBus.roomSize = v; });
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
    wireUndoableSlider(sendDampSlider, "Set send bus damping",
                       [](const model::Song& s) { return s.sendBus.damping; },
                       [](model::Song& s, float v) { s.sendBus.damping = v; });
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
    wireUndoableSlider(sendDelayTimeSlider, "Set send bus delay time",
                       [](const model::Song& s) { return s.sendBus.delayTimeMs; },
                       [](model::Song& s, float v) { s.sendBus.delayTimeMs = v; });
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
    wireUndoableSlider(sendDelayFbSlider, "Set send bus delay feedback",
                       [](const model::Song& s) { return s.sendBus.delayFeedback; },
                       [](model::Song& s, float v) { s.sendBus.delayFeedback = v; });
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
    wireUndoableSlider(sendReturnSlider, "Set send bus return level",
                       [](const model::Song& s) { return s.sendBus.returnLevel; },
                       [](model::Song& s, float v) { s.sendBus.returnLevel = v; });
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
        strip->onFaderDragStart = [this, i](MixerStrip::Fader f) { beginFaderDrag(i, f); };
        strip->onFaderDragEnd   = [this, i](MixerStrip::Fader f) { endFaderDrag(i, f); };
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
    pianoRoll_.onNotesDeleted = [this](int count)
    {
        // Delete in the keys pane always means notes, so it reports even when
        // nothing was selected — otherwise a user who expected the track to
        // go, or who forgot to select, gets silence and no idea which.
        if (count > 0)
            showStatus("Deleted " + juce::String(count) + (count == 1 ? " note" : " notes"));
        else
            showStatus("Select notes first - shift-click, or shift-drag a box");
    };

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

    // Not added to editTab_ directly: keysViewport_ takes it as its viewed
    // component below, and adding it here as well would reparent it straight
    // back out of the viewport.
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

    // A magnifying glass, a slider and an editable multiplier. The icon says
    // what the control is without spending width on the word; the slider
    // makes the whole range reachable in one gesture; the box shows the exact
    // figure and takes one typed in. Both panes get the same control from one
    // definition — two copies of this would be two things to keep in step.
    setUpZoomControls(arrangeTab_, zoomIcon_, zoomSlider_, zoomBox_,
                      ArrangementView::kMinZoom, ArrangementView::kMaxZoom,
                      withShortcut("Timeline zoom", keys::zoomIn),
                      [this](float zoom) { setTimelineZoom(zoom); });

    setUpZoomControls(editTab_, keysZoomIcon_, keysZoomSlider_, keysZoomBox_,
                      PianoRoll::kMinPitchZoom, PianoRoll::kMaxPitchZoom,
                      "Pitch zoom - how many notes the grid shows (cmd-scroll)",
                      [this](float zoom) { setKeysZoom(zoom); });

    setUpZoomControls(editTab_, keysTimeZoomIcon_, keysTimeZoomSlider_, keysTimeZoomBox_,
                      PianoRoll::kMinTimeZoom, PianoRoll::kMaxTimeZoom,
                      "Time zoom - how wide each step is (shift-scroll)",
                      [this](float zoom) { setKeysTimeZoom(zoom); });

    // The roll scrolls horizontally once it's wider than its pane, exactly as
    // the arrangement does.
    keysViewport_.setViewedComponent(&pianoRoll_, false);
    keysViewport_.setScrollBarsShown(false, true); // horizontal only: rows fill the height
    editTab_.addAndMakeVisible(keysViewport_);

    pianoRoll_.onTimeZoomChanged = [this] { updateKeysTimeZoomControls(); layoutEditTab(); };

    // On by default: a scrolled grid that doesn't follow playback means the
    // playhead simply leaves the screen. Off is for editing one bar while the
    // rest of the pattern plays, where the view jumping is the annoyance.
    keysFollowButton_.setButtonText("Follow");
    keysFollowButton_.setTooltip("Scroll the grid to keep the playhead in view");
    keysFollowButton_.setToggleState(true, juce::dontSendNotification);
    editTab_.addAndMakeVisible(keysFollowButton_);

    // The wheel zooms the roll too, so the control follows it rather than
    // drifting from what's on screen.
    pianoRoll_.onPitchZoomChanged = [this] { updateKeysZoomControls(); };
    addClipButton_.onClick = [this] { addClipToSelectedTrack(); };

    arrangeTab_.addAndMakeVisible(addClipButton_);
    arrangeTab_.onResized = [this] { layoutArrangeTab(); };

    arrangementView_.onSeek = [this](double beat)
    {
        const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
        uiTempoMap_.setSampleRate(sampleRate);
        post(Cmd::Seek, (double) uiTempoMap_.samplesFromPpq(juce::jmax(0.0, beat)));
    };

    // Muting from the tracks pane goes through the same setTrackMuted the
    // mixer strip uses, so the two views can't disagree about a track's state.
    arrangementView_.onTrackMuteToggled = [this](int trackIndex)
    {
        const auto& tracks = history_.current().tracks;
        if (trackIndex < 0 || trackIndex >= (int) tracks.size())
            return;

        // Both read before the change. setTrackMuted is an undoable edit now,
        // and committing one move-assigns the document — which leaves any
        // reference into the old one dangling. Copying the name out first is
        // what keeps this from reading freed memory a line later.
        const bool nowMuted = ! tracks[(size_t) trackIndex].muted;
        const auto name     = juce::String(tracks[(size_t) trackIndex].name);

        setTrackMuted(trackIndex, nowMuted);

        showStatus((nowMuted ? "Muted " : "Unmuted ") + (name.isEmpty()
                       ? "track " + juce::String(trackIndex + 1) : "\"" + name + "\""));
    };

    arrangementView_.onTrackSettingsRequested = [this](int trackIndex)
    {
        showTrackSettingsMenu(trackIndex);
    };

    arrangementView_.onTrackDuplicateRequested = [this](int trackIndex)
    {
        duplicateTrackAt(trackIndex);
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

    synthEditor_.onSettingsChanged   = [this](const model::SynthSettings& s) { setTrackSynthSettings(s); };
    synthEditor_.onSettingsDragStart = [this] { beginSynthSettingsDrag(); };
    synthEditor_.onSettingsDragEnd   = [this] { endSynthSettingsDrag(); };
    synthEditor_.onPresetSelected        = [this](int i) { applyPreset(i); };
    synthEditor_.onSavePresetRequested   = [this] { savePresetDialog(); };
    synthEditor_.onDeletePresetRequested = [this](int i) { deletePresetAt(i); };
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
    fretboard_.onSettingsChanged   = [this](const model::GuitarSettings& s) { setTrackGuitarSettings(s); };
    fretboard_.onSettingsDragStart = [this] { beginGuitarSettingsDrag(); };
    fretboard_.onSettingsDragEnd   = [this] { endGuitarSettingsDrag(); };
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
    effectChain_.onSlotParamsDragStart  = [this](int i) { beginEffectSlotParamsDrag(i); };
    effectChain_.onSlotParamsDragEnd    = [this](int i) { endEffectSlotParamsDrag(i); };
    effectChain_.onScanRequested        = [this] { scanForPlugins(); };

    // A previous scan, so launching doesn't re-probe every plugin on the
    // machine — probing instantiates each one and is slow.
    engine_.pluginHost().restoreScanCache(settings_.getValue("pluginScanCache").toStdString());
    effectChain_.setAvailablePlugins(engine_.pluginHost().knownPlugins());
    effectChain_.onPluginEditorRequested = [this](int slot) { openPluginEditor(slot); };

    seedFactoryPresets();
    refreshPresetList();

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
    updateZoomControls();     // the readouts must say something before the first click
    updateKeysZoomControls();
    updateKeysTimeZoomControls();

    // The document the app opens with counts as saved, so an untouched session
    // doesn't prompt on quit. This has to come *after* all the control setup
    // above: several of those updateXxxControls calls write through
    // mutableCurrent(), which advances the state id by design, so a marker
    // taken any earlier is stale by the time construction finishes.
    savedStateId_ = history_.stateId();

    // Still worth having in a debug build: it fires at launch rather than
    // when someone presses the key. The tests are what cover the release
    // build — see tests/gui/ShortcutsTests.cpp.
    for ([[maybe_unused]] const auto& shortcut : keys::all())
        jassert(shortcut.key.isValid());

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
        menu.addItem(31, "Repair Recorded Clip Lengths...");
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
        menu.addSeparator();
        addItem(menu, 28, "Copy Track", keys::copyTrack);
        addItem(menu, 29, "Paste Track", keys::pasteTrack, trackClipboard_.has_value());
        addItem(menu, 30, "Duplicate Track", keys::duplicateTrack);
        addItem(menu, 25, "Delete Clip", keys::deleteClip);
        menu.addSeparator();
        menu.addItem(26, "Rename Track...");
        // The last track isn't deletable: a song with none has no pane that
        // can do anything, and no obvious way back.
        addItem(menu, 27, "Delete Track", keys::deleteTrack, trackCount() > 1);
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
        case 28: copyTrack(); break;
        case 29: pasteTrack(); break;
        case 30: duplicateTrackAt(selectedTrackIndex_); break;
        case 31: repairRecordedClipLengths(); break;
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

    // Real sounds by default rather than four silent pads — the same reason
    // makeStarterSong() does this for the track a fresh launch begins with.
    // The pattern stays empty, unlike the startup track's: this is a track
    // the user is deliberately adding, so it gets a blank canvas to program
    // rather than a copy of the demo loop.
    const auto kit = defaultDrumKitWithFactorySamples();
    history_.edit("Add drum track", [kit](model::Song& s)
    {
        const auto name = "Drums " + juce::String((int) s.tracks.size() + 1);
        const int  id   = model::addTrack(s, model::TrackType::Drum, name.toStdString()).id;
        s.tracks.back().drumKit = kit;
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
/** Adds already-placed notes to the selected clip and reports it.

    The notes arrive carrying their final positions — planChordStamp works out
    where the bar is, and this only commits. Splitting it that way is what
    makes the placement testable. */
bool MainComponent::commitStampedNotes(const std::vector<engine::Note>& notes,
                                       const juce::String& what, double atBeats)
{
    const int trackIdx = selectedTrackIndex_;
    const int clipIdx  = selectedClipIndex_;

    int added = 0;
    history_.edit("Add chord", [trackIdx, clipIdx, &notes, &added](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;

        auto& clips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) clips.size())
            return;

        auto& pattern = clips[(size_t) clipIdx].pattern;
        for (const auto& note : notes)
        {
            pattern.notes.push_back(note);
            ++added;
        }
    });

    syncEngineTracks();
    refreshPianoRollForSelected();
    refreshFretboardForSelected();

    // Stamping writes notes into the clip; unless the transport happens to be
    // rolling over that bar, nothing moves and nothing sounds. Say what landed
    // and where, or a chord that worked looks exactly like one that didn't.
    if (added > 0)
        showStatus(what + " written at beat " + juce::String(atBeats + 1.0, 2));
    else
        showError("Couldn't write " + what + " — the clip went away");

    return added > 0;
}

/** Stamps one of the open shapes from the chord palette. */
/** Sounds a chord through the selected track's own instrument, struck the way
    it will be written.

    Takes the strummed notes rather than a set of pitches so the preview is
    the same thing that lands in the clip, stagger included. Six notes at one
    instant read as an organ, and the stagger is most of what makes a strum
    sound like a hand — a preview without it would undersell the very control
    the user is reaching for.

    Shared by the palette and by clicking the neck. They had drifted: the neck
    played what it wrote and the palette wrote silently, so with the transport
    stopped the palette buttons looked like they did nothing at all. */
void MainComponent::previewChord(const std::vector<engine::Note>& notes)
{
    const double msPerBeat = 60000.0 / juce::jmax(1.0, history_.current().bpm);

    for (const auto& note : notes)
    {
        const int delayMs = (int) std::lround(juce::jmax(0.0, note.startBeats) * msPerBeat);

        if (delayMs <= 0)
        {
            previewNote(note.noteNumber);
            continue;
        }

        // SafePointer for the same reason previewNote uses one: this fires
        // after the click, and closing the window in between would otherwise
        // run it against a destroyed engine.
        juce::Component::SafePointer<MainComponent> safeThis(this);
        const int noteNumber = note.noteNumber;

        juce::Timer::callAfterDelay(delayMs, [safeThis, noteNumber]
        {
            if (auto* self = safeThis.getComponent())
                self->previewNote(noteNumber);
        });
    }
}

void MainComponent::stampChord(const engine::ChordShape& shape, int fretOffset,
                               const engine::StrumSettings& strum)
{
    // Every guard and the placement arithmetic live in planChordStamp, which
    // is JUCE-free and tested. They were inside this function, where nothing
    // could reach them — which is most of why the reported "the chord buttons
    // don't do anything" took so long to place.
    const auto plan = planChordStamp(history_.current(), selectedTrackIndex_, selectedClipIndex_,
                                     shape, fretOffset, strum,
                                     uiTempoMap_.ppqFromSamples(engine_.playheadSamples()),
                                     beatsPerBar(), chordStampSeed_++ | 1u);

    if (! plan.ok)
    {
        showError(juce::String(plan.problem));
        return;
    }

    // Played as well as written. Stamping puts notes in the clip, which makes
    // no sound unless the transport happens to be rolling over that bar — so
    // without this a button that worked was indistinguishable from one that
    // didn't.
    previewChord(plan.notes);
    commitStampedNotes(plan.notes, juce::String(shape.name) + " chord",
                       plan.atBeats);
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

    const auto struck = engine::GuitarChords::strumRootedChord(
                            shape, track->guitarSettings.tuning.data(), rootString, fret,
                            0.0, beatsPerBar(), history_.current().bpm, strum,
                            (uint32_t) (chordStampSeed_++ | 1u));

    // Sounded through the same preview path a single fret click uses, so the
    // chord is played by the track's own GuitarNode — including its
    // one-note-per-string cut, which is what stops a chord from sounding like
    // six unrelated strings.
    previewChord(struck);

    const juce::String what = juce::String(engine::movableShapeName(shape)) + " on "
                            + juce::String(engine::midiNoteName(notes.front()));

    if (! writeToClip)
    {
        showStatus(what);
        return;
    }

    commitStampedNotes(struck, what, 0.0);
}

/** Same as addTrack(), but a Guitar-type track — six plucked strings in
    standard tuning (see model::GuitarSettings). */
void MainComponent::addGuitarTrack()
{
    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

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
    {
        const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
        fretboard_.setSettings(track.guitarSettings);
        fretboard_.setTrackInfo(track.name, track.colour);
    }
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

    const auto name = song.scenes[(size_t) sceneIndex].name;

    engine_.stopAllSessionSlots();

    history_.edit("Delete scene", [sceneIndex](model::Song& s) { model::removeScene(s, sceneIndex); });

    syncEngineTracks();
    refreshSessionView();

    // Bigger blast radius than a track deletion — every clip on every track
    // in the row — so it earns the same reassurance, not less.
    showStatus("Deleted \"" + juce::String(name) + "\" - undo to bring it back");
}

void MainComponent::addSessionScene()
{
    std::string name;
    history_.edit("Add scene", [&name](model::Song& s)
    {
        name = "Scene " + std::to_string(s.scenes.size() + 1);
        model::addScene(s, name);
    });

    syncEngineTracks();
    refreshSessionView();
    showStatus("Added \"" + juce::String(name) + "\"");
}

/** Clicking an empty cell fills it with a copy of the track's currently open
    clip — the quickest way to get material into the grid without a separate
    "new session clip" flow. Declines if there's nothing to copy. */
void MainComponent::captureClipIntoSession(int trackIndex, int sceneIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& clips = song.tracks[(size_t) trackIndex].clips;
    if (clips.empty())
    {
        showError("Nothing to capture - this track has no clips");
        return;
    }

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
    params.chorusRateHz    = slot.chorus.rateHz;
    params.chorusDepth     = slot.chorus.depth;
    params.chorusMix       = slot.chorus.mix;
    params.wobbleRateBeats    = slot.wobble.rateBeats;
    params.wobbleDepth        = slot.wobble.depth;
    params.wobbleBaseCutoffHz = slot.wobble.baseCutoffHz;
    params.wobbleResonance    = slot.wobble.resonance;
    params.wobbleMix          = slot.wobble.mix;
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

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    const int index = selectedTrackIndex_;
    history_.edit(enabled ? "Enable effect" : "Bypass effect", [index, slotIndex, enabled](model::Song& s)
    {
        s.tracks[(size_t) index].effectChain[(size_t) slotIndex].enabled = enabled;
    });

    const auto& updatedChain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    engine_.setTrackEffectSlotParams(selectedTrackIndex_, slotIndex, toSlotParams(updatedChain[(size_t) slotIndex]));
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
    showBusy("Scanning for plugins...");

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

    // Reachable by a bare key and the most-used delete in the app, so it's
    // the one most likely to be hit by accident — same reasoning as track
    // deletion below.
    showStatus("Deleted clip - undo to bring it back");
}

/** Deletes the selected track and everything on it. Undo covers it, as with
    clips — but the last track isn't deletable, because a song with no tracks
    has no pane that can do anything and no obvious way back. */
void MainComponent::deleteSelectedTrack()
{
    deleteTrackAt(selectedTrackIndex_);
}

/** Deletes one track by index, which is not necessarily the selected one —
    the gear menu acts on the track whose gear was clicked.

    That is why the selection is fixed up through selectionAfterTrackRemoved
    rather than merely clamped: removing a track above the selected one shifts
    it down, and getting that wrong doesn't crash, it quietly leaves a
    different track selected than the one that was highlighted, so the next
    edit lands somewhere the user didn't mean. */
void MainComponent::deleteTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        showError("No track to delete");
        return;
    }

    if (song.tracks.size() <= 1)
    {
        showError("The last track can't be deleted");
        return;
    }

    const auto& track   = song.tracks[(size_t) trackIndex];
    const int   trackId = track.id;

    // Copied before the edit: committing one move-assigns the document, which
    // leaves any reference into the old one dangling.
    const auto name = track.name.empty() ? ("track " + juce::String(trackIndex + 1))
                                         : ("\"" + juce::String(track.name) + "\"");

    history_.edit("Delete track", [trackId](model::Song& s) { model::removeTrack(s, trackId); });

    selectTrackAndRefreshAll(selectionAfterTrackRemoved(selectedTrackIndex_, trackIndex,
                                                        trackCount()));

    // Deleting is reachable by an unmodified key and by one menu click, so it
    // can be hit by accident. Saying what went and that undo will bring it
    // back is the difference between a recoverable slip and a mystery.
    showStatus("Deleted " + name + " - undo to bring it back");
}

/** Renames the selected track. Track names are the only label distinguishing
    one strip, row or tab from the next, and until now they were whatever the
    Add button happened to generate. */
void MainComponent::renameSelectedTrack()
{
    renameTrackAt(selectedTrackIndex_);
}

/** Copies a track and everything on it, putting the copy directly after it.

    The point of duplicating a track here is a second copy of a part to loop
    against the first, so the copy has to be complete — clips, instrument
    settings, effect chain and all — and it has to be genuinely separate,
    which is what model::duplicateTrack's reissuing of every id gives. */
void MainComponent::duplicateTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
    {
        showError("No track to duplicate");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const auto sourceName = juce::String(song.tracks[(size_t) trackIndex].name);

    history_.edit("Duplicate track", [trackIndex](model::Song& s)
    {
        model::duplicateTrack(s, trackIndex);
    });

    selectTrackAndRefreshAll(trackIndex + 1); // the copy, so it can be worked on straight away
    showStatus("Duplicated " + (sourceName.isEmpty() ? juce::String("track") : "\"" + sourceName + "\""));
}

/** Copies the selected track for later pasting. Deliberately its own
    clipboard rather than sharing the clip one: pasting a track when a clip
    was copied, or the reverse, is the kind of guess that loses work. */
void MainComponent::copyTrack()
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
    {
        showError("No track selected");
        return;
    }

    const auto& track = song.tracks[(size_t) selectedTrackIndex_];
    trackClipboard_   = track;

    showStatus("Copied " + (track.name.empty() ? juce::String("track")
                                               : "\"" + juce::String(track.name) + "\""));
}

void MainComponent::pasteTrack()
{
    if (! trackClipboard_.has_value())
    {
        showError("No track copied");
        return;
    }

    if (trackCount() >= engine_.maxTracks())
    {
        showError("Track limit reached");
        return;
    }

    const auto copied = *trackClipboard_;

    // appendTrackCopy reissues every id, so pasting the same buffer twice
    // gives two genuinely separate tracks rather than two the app can't tell
    // apart.
    history_.edit("Paste track", [&copied](model::Song& s) { model::appendTrackCopy(s, copied); });

    selectTrackAndRefreshAll(trackCount() - 1);
    showStatus("Pasted " + (copied.name.empty() ? juce::String("track")
                                                : "\"" + juce::String(copied.name) + "\""));
}

/** The per-track settings menu, opened from the gear in the tracks pane. */
void MainComponent::showTrackSettingsMenu(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track = song.tracks[(size_t) trackIndex];

    juce::PopupMenu colours;
    for (int i = 0; i < kNumTrackColours; ++i)
    {
        const auto& option = kTrackColours[i];
        const bool  chosen = (track.colour == option.argb);

        // Ticked rather than swatched: PopupMenu has no colour-chip item, and
        // a tick at least says which one is in force.
        colours.addItem(kFirstColourMenuId + i, option.name, true, chosen);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader(track.name.empty() ? ("Track " + juce::String(trackIndex + 1))
                                             : juce::String(track.name));
    menu.addSubMenu("Colour", colours);
    menu.addItem(1, "Rename...");
    menu.addSeparator();

    // Greyed rather than absent when it's the last track: an item that isn't
    // there reads as a missing feature, where a disabled one says the rule.
    menu.addItem(2, "Delete Track", song.tracks.size() > 1);

    juce::Component::SafePointer<MainComponent> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [self, trackIndex](int result)
    {
        if (self == nullptr || result == 0)
            return;

        if (result == 1)
        {
            self->renameTrackAt(trackIndex);
            return;
        }

        if (result == 2)
        {
            self->deleteTrackAt(trackIndex);
            return;
        }

        if (const int index = result - kFirstColourMenuId; index >= 0 && index < kNumTrackColours)
            self->setTrackColour(trackIndex, kTrackColours[index].argb);
    });
}

/** Colour is document state, so it's an undoable edit rather than a live
    tweak — unlike mute, which is a performance control you flip while
    listening and would not want filling the undo stack. */
void MainComponent::setTrackColour(int trackIndex, unsigned int argb)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const int trackId = song.tracks[(size_t) trackIndex].id;

    history_.edit("Recolour track", [trackId, argb](model::Song& s)
    {
        if (auto* track = model::findTrack(s, trackId))
            track->colour = argb;
    });

    arrangementView_.setSong(history_.current());
    updateMixerStrips();
}

void MainComponent::renameTrackAt(int trackIndex)
{
    const auto& song = history_.current();
    if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
        return;

    const auto& track   = song.tracks[(size_t) trackIndex];
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

    const auto& song = history_.current();
    if (selectedTrackIndex_ >= (int) song.tracks.size())
        return;
    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return;

    const int  trackIdx  = selectedTrackIndex_;
    const int  clipIdx   = selectedClipIndex_;
    const auto selection = pianoRoll_.selectedNoteIndices();
    const int  affected  = selection.empty()
                              ? (int) clips[(size_t) clipIdx].pattern.notes.size()
                              : (int) selection.size();

    if (affected == 0)
    {
        showStatus("Nothing to " + juce::String(swingAmount > 0.0 ? "swing" : "quantize")
                   + " - this clip has no notes");
        return;
    }

    history_.edit(swingAmount > 0.0 ? "Swing" : "Quantize",
                  [trackIdx, clipIdx, swingAmount, &selection](model::Song& s)
    {
        if (trackIdx < 0 || trackIdx >= (int) s.tracks.size())
            return;
        auto& trackClips = s.tracks[(size_t) trackIdx].clips;
        if (clipIdx < 0 || clipIdx >= (int) trackClips.size())
            return;

        // The grid the editor draws is 16ths, so that's what notes snap to.
        engine::NoteOps::quantizeNotes(trackClips[(size_t) clipIdx].pattern.notes, 0.25, swingAmount, selection);
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

    showStatus((swingAmount > 0.0 ? "Swung " : "Quantized ") + juce::String(affected)
               + (affected == 1 ? " note" : " notes"));
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
            slot.pattern     = clip.pattern;
            slot.startBeats  = clip.startBeats;
            slot.lengthBeats = clip.lengthBeats;
            slots.push_back(slot);
        }
        engine_.setTrackClips(i, slots);

        // Audio clips -> the track's own audio-clip player. Each Audio-type
        // clip becomes one AudioClipSlot, gated to its own
        // [startBeats, startBeats+lengthBeats) window exactly like the
        // instrument clips above. Unconditionally resubmitted every sync,
        // same as instrument clips — cheap, since AudioEngine caches decoded
        // audio by file path (see AudioEngine::setTrackAudioClips), so this
        // never re-decodes a file it's already loaded, even across tracks
        // that share one.
        std::vector<engine::AudioClipSpec> audioSpecs;
        for (const auto& clip : track.clips)
        {
            if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
                continue;

            engine::AudioClipSpec spec;
            spec.file        = juce::File(clip.audioFile);
            spec.startBeats  = clip.startBeats;
            spec.lengthBeats = clip.lengthBeats;
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
                case model::EffectKind::Chorus:     spec.kind = engine::EffectNodeKind::Chorus;     break;
                case model::EffectKind::Wobble:     spec.kind = engine::EffectNodeKind::Wobble;     break;
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

    // The same condition currentPattern() falls back to its shared empty
    // Pattern for — the roll can't otherwise tell "nothing is open" apart
    // from "a real clip that's genuinely empty."
    const auto& song = history_.current();
    const bool  noClipOpen = selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size()
                          || selectedClipIndex_ < 0
                          || selectedClipIndex_ >= (int) song.tracks[(size_t) selectedTrackIndex_].clips.size();
    pianoRoll_.setNoClipSelected(noClipOpen);

    if (! noClipOpen)
    {
        const auto& track = song.tracks[(size_t) selectedTrackIndex_];
        pianoRoll_.setTrackInfo(track.name, track.colour);
    }

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

    const auto& track = history_.current().tracks[(size_t) trackIndex];
    drumsPane_.setKit(track.drumKit.pads, currentPattern());
    drumsPane_.setTrackInfo(track.name, track.colour);
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

    // More destructive than the row disappearing suggests: every hit that
    // played this pad, on every clip on the track, went with it.
    showStatus("Removed pad - undo to bring it back");
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
    {
        const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
        synthEditor_.setSettings(track.synthSettings);
        synthEditor_.setTrackInfo(track.name, track.colour);
    }
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

/** Changes the bar length. Structural document state, so it goes through the
    history like a track's colour rather than being a live tweak like tempo —
    a bar length is part of the piece, not a knob you ride while listening.

    Everything that measures bars has to follow: the engine's metronome and
    count-in, the loop region, and the grids in the tracks and keys panes. */
void MainComponent::setTimeSignature(int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0)
        return;

    const auto& song = history_.current();
    if (song.timeSigNumerator == numerator && song.timeSigDenominator == denominator)
        return;

    history_.edit("Change time signature", [numerator, denominator](model::Song& s)
    {
        s.timeSigNumerator   = numerator;
        s.timeSigDenominator = denominator;
    });

    uiTempoMap_.setTimeSignature(numerator, denominator);
    post(Cmd::SetTimeSignature, (double) numerator, (double) denominator);

    updateTimeSignatureControls();
    updateLoopRegion();               // bars just changed length, so the loop did too
    pianoRoll_.setBeatsPerBar(beatsPerBar());
    arrangementView_.setSong(history_.current());

    showStatus("Time signature: " + juce::String(numerator) + "/" + juce::String(denominator));
}

/** Points the control at whatever the document says, without reporting it
    straight back as a user edit. */
void MainComponent::updateTimeSignatureControls()
{
    const auto& song = history_.current();

    for (int i = 0; i < kNumTimeSignatures; ++i)
    {
        if (kTimeSignatures[i].numerator == song.timeSigNumerator
            && kTimeSignatures[i].denominator == song.timeSigDenominator)
        {
            timeSigBox_.setSelectedId(i + 1, juce::dontSendNotification);
            return;
        }
    }

    // A signature loaded from a project that isn't in the list — show nothing
    // rather than a wrong one.
    timeSigBox_.setSelectedId(0, juce::dontSendNotification);
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

/** Reads the value a fader controls, straight from the document. */
static float readFader(const model::Song& song, int index, MixerStrip::Fader fader)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return 0.0f;

    const auto& track = song.tracks[(size_t) index];
    switch (fader)
    {
        case MixerStrip::Fader::Gain: return track.gainDb;
        case MixerStrip::Fader::Pan:  return track.pan;
        case MixerStrip::Fader::Send: return track.sendLevel;
    }
    return 0.0f;
}

static void writeFader(model::Song& song, int index, MixerStrip::Fader fader, float value)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    auto& track = song.tracks[(size_t) index];
    switch (fader)
    {
        case MixerStrip::Fader::Gain: track.gainDb    = value; break;
        case MixerStrip::Fader::Pan:  track.pan       = value; break;
        case MixerStrip::Fader::Send: track.sendLevel = value; break;
    }
}

static const char* faderName(MixerStrip::Fader fader)
{
    switch (fader)
    {
        case MixerStrip::Fader::Gain: return "Set track gain";
        case MixerStrip::Fader::Pan:  return "Set track pan";
        case MixerStrip::Fader::Send: return "Set track send";
    }
    return "Set track level";
}

/** Remembers where a fader was when it was grabbed. */
void MainComponent::beginFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    faderDragTrack_ = trackIndex;
    faderDragWhich_ = fader;
    faderDragFrom_  = readFader(history_.current(), trackIndex, fader);
    faderDragging_  = true;
}

/** Turns a whole fader drag into one undo step.

    The live changes during the drag go through mutableCurrent, so the audio
    follows the fader without hundreds of snapshots. On release the document is
    rewound to where the drag started and the final value committed as a single
    edit — which is what leaves exactly one step on the stack for the whole
    gesture.

    Mute and solo don't need this: a click is already one edit. A fader is
    hundreds of values, and one step each would bury the last real edit under a
    drag. */
void MainComponent::endFaderDrag(int trackIndex, MixerStrip::Fader fader)
{
    if (! faderDragging_ || faderDragTrack_ != trackIndex || faderDragWhich_ != fader)
        return;

    faderDragging_ = false;

    const float landedOn = readFader(history_.current(), trackIndex, fader);
    commitDrag(history_, faderName(fader), faderDragFrom_, landedOn,
               [trackIndex, fader](model::Song& s, float v) { writeFader(s, trackIndex, fader, v); });
}

/** Remembers an effect slot's parameters before a drag on one of its controls
    started — see EffectChainPanel::onSlotParamsDragStart. */
void MainComponent::beginEffectSlotParamsDrag(int slotIndex)
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    effectSlotDragging_  = true;
    effectSlotDragTrack_ = selectedTrackIndex_;
    effectSlotDragIndex_ = slotIndex;
    effectSlotDragFrom_  = chain[(size_t) slotIndex];
}

/** Commits a whole effect-slot-parameters drag as one undo step, the
    commitStructDrag equivalent of endFaderDrag above — a slot's parameters
    are a struct of several fields changed together, not one number, so
    there's no meaningful tolerance to check against: any real change
    commits, equality is the whole test. */
void MainComponent::endEffectSlotParamsDrag(int slotIndex)
{
    if (! effectSlotDragging_ || effectSlotDragTrack_ != selectedTrackIndex_ || effectSlotDragIndex_ != slotIndex)
        return;

    effectSlotDragging_ = false;

    const auto& chain = history_.current().tracks[(size_t) selectedTrackIndex_].effectChain;
    if (slotIndex < 0 || slotIndex >= (int) chain.size())
        return;

    const auto landedOn   = chain[(size_t) slotIndex];
    const int  trackIndex = selectedTrackIndex_;

    commitStructDrag(history_, "Set effect parameters", effectSlotDragFrom_, landedOn,
                     [trackIndex, slotIndex](model::Song& s, const model::EffectSlot& value)
    {
        auto& c = s.tracks[(size_t) trackIndex].effectChain;
        if (slotIndex >= 0 && slotIndex < (int) c.size())
            c[(size_t) slotIndex] = value;
    });
}

/** Remembers a track's synth settings before a drag on one of the Synth
    pane's controls started — see SynthEditor::onSettingsDragStart. */
void MainComponent::beginSynthSettingsDrag()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    synthSettingsDragging_  = true;
    synthSettingsDragTrack_ = selectedTrackIndex_;
    synthSettingsDragFrom_  = history_.current().tracks[(size_t) selectedTrackIndex_].synthSettings;
}

/** Commits a whole synth-settings drag as one undo step — the
    commitStructDrag equivalent of endEffectSlotParamsDrag above. */
void MainComponent::endSynthSettingsDrag()
{
    if (! synthSettingsDragging_ || synthSettingsDragTrack_ != selectedTrackIndex_)
        return;

    synthSettingsDragging_ = false;

    const int  trackIndex = selectedTrackIndex_;
    const auto landedOn   = history_.current().tracks[(size_t) trackIndex].synthSettings;

    commitStructDrag(history_, "Set synth settings", synthSettingsDragFrom_, landedOn,
                     [trackIndex](model::Song& s, const model::SynthSettings& value)
    {
        s.tracks[(size_t) trackIndex].synthSettings = value;
    });
}

/** Remembers a track's guitar settings before a drag on one of the
    fretboard's controls started — see FretboardPane::onSettingsDragStart. */
void MainComponent::beginGuitarSettingsDrag()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;

    guitarSettingsDragging_  = true;
    guitarSettingsDragTrack_ = selectedTrackIndex_;
    guitarSettingsDragFrom_  = history_.current().tracks[(size_t) selectedTrackIndex_].guitarSettings;
}

/** Commits a whole guitar-settings drag as one undo step — the
    commitStructDrag equivalent of endSynthSettingsDrag above. */
void MainComponent::endGuitarSettingsDrag()
{
    if (! guitarSettingsDragging_ || guitarSettingsDragTrack_ != selectedTrackIndex_)
        return;

    guitarSettingsDragging_ = false;

    const int  trackIndex = selectedTrackIndex_;
    const auto landedOn   = history_.current().tracks[(size_t) trackIndex].guitarSettings;

    commitStructDrag(history_, "Set guitar settings", guitarSettingsDragFrom_, landedOn,
                     [trackIndex](model::Song& s, const model::GuitarSettings& value)
    {
        s.tracks[(size_t) trackIndex].guitarSettings = value;
    });
}

/** Mutes or unmutes a track, as an undoable edit.

    Mute and solo go through the history where gain, pan and send level do
    not, and the difference is that these two are discrete. A click is one
    edit, so it makes one undo step. A fader is a drag of hundreds of values,
    and putting each on the stack would bury the last real edit under a
    hundred nudges — those stay live tweaks until there is somewhere to
    coalesce a whole drag into a single step.

    Undo reaches the audio as well as the document: refreshFromModel runs
    syncEngineTracks, which pushes every track's mute and solo back to the
    engine. Without that an undone mute would restore the checkbox and leave
    the track silent. */
void MainComponent::setTrackMuted(int index, bool muted)
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    if (song.tracks[(size_t) index].muted == muted)
        return; // nothing changed, so nothing worth an undo step

    history_.edit(muted ? "Mute track" : "Unmute track", [index, muted](model::Song& s)
    {
        s.tracks[(size_t) index].muted = muted;
    });

    engine_.setTrackMuted(index, muted);

    // Both views show mute, and either can set it, so both are refreshed from
    // the document here rather than by whichever one happened to be clicked.
    // Without this the tracks pane muted the audio and left its own icon
    // unchanged — indistinguishable from a button that does nothing.
    // MixerStrip::setMuted uses dontSendNotification, so this can't echo back.
    arrangementView_.setSong(history_.current());
    updateMixerStrips();
}

/** Solos or unsolos a track. Undoable for the same reason as mute — see
    setTrackMuted, which explains why the continuous controls are not. */
void MainComponent::setTrackSolo(int index, bool solo)
{
    const auto& song = history_.current();
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    if (song.tracks[(size_t) index].solo == solo)
        return;

    history_.edit(solo ? "Solo track" : "Unsolo track", [index, solo](model::Song& s)
    {
        s.tracks[(size_t) index].solo = solo;
    });

    engine_.setTrackSolo(index, solo);
    updateMixerStrips();
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
    updateTimeSignatureControls();
    pianoRoll_.setBeatsPerBar(beatsPerBar());

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

    if (key == keys::copyTrack)      { copyTrack();  return true; }
    if (key == keys::pasteTrack)     { pasteTrack(); return true; }
    if (key == keys::duplicateTrack) { duplicateTrackAt(selectedTrackIndex_); return true; }

    // Unmodified, so a focused text field consumes it first and this can't
    // interrupt typing.
    if (key == keys::deleteTrack || key == keys::deleteTrackAlt)
    {
        deleteSelectedTrack();
        return true;
    }

    if (key == keys::zoomIn)  { setTimelineZoom(arrangementView_.zoom() * 1.25f); return true; }
    if (key == keys::zoomOut) { setTimelineZoom(arrangementView_.zoom() / 1.25f); return true; }

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

/** One-time fix-up for projects saved while recorded takes were always given
    a flat four beats regardless of how long the take actually ran (see
    finishRecordingIfReady). Re-measures every audio clip against its file and
    corrects any that disagree — see ClipLengthRepair.h for why "disagrees
    with its file" rather than "is exactly four beats" is the right test. */
void MainComponent::repairRecordedClipLengths()
{
    auto probe = [this](const std::string& path) -> double
    {
        const juce::File file(path);
        return file.existsAsFile() ? engine_.probeDurationSeconds(file) : 0.0;
    };

    // A dry run on a copy first, so nothing is added to the undo stack when
    // there is nothing to fix.
    auto        dryRun = history_.current();
    const auto  fixes  = repairAudioClipLengths(dryRun, probe);

    if (fixes.empty())
    {
        showStatus("No recorded clips needed a length fix");
        return;
    }

    history_.edit("Repair recorded clip lengths", [probe](model::Song& s)
    {
        repairAudioClipLengths(s, probe);
    });

    refreshFromModel();
    showStatus(juce::String((int) fixes.size())
               + (fixes.size() == 1 ? " clip length was fixed" : " clip lengths were fixed"));
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
    if (durationSeconds <= 0.0)
    {
        // The file couldn't be decoded at all — corrupt, truncated, or an
        // unsupported format. Falling through to a fabricated 4-beat clip
        // pointing at a file the engine can't play would create a track (or
        // clip) that just sits there silent with nothing to say why.
        showError("Could not import: " + file.getFileName());
        return;
    }
    const double measured        = engine::beatsForSeconds(durationSeconds, song.bpm);
    const double lengthBeats     = measured > 0.0 ? measured : 4.0;
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

        // The transport runs free for the length of a take. Looping would wrap
        // it at the end of what is already arranged, which is precisely where
        // a recording needs to keep going — you are recording the part that
        // isn't there yet. The button's own state is left alone and restored
        // when the take ends, so the user's setting survives.
        post(Cmd::SetLooping, 0.0);
        post(Cmd::SetPlaying, 1.0);
    }
    else
    {
        engine_.stopRecording();
        post(Cmd::SetPlaying, 0.0);
        post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0); // whatever it was before
        recordButton.setToggleState(false, juce::dontSendNotification); // back to the record disc
        recordButton.setTooltip(withShortcut("Record", keys::record));
    }
}

void MainComponent::finishRecordingIfReady()
{
    if (! awaitingRecordedTake_ || ! engine_.isRecordingFinished())
        return;
    awaitingRecordedTake_ = false;

    // Every ending passes through here — the stop button, play/pause during a
    // take, or the engine finishing on its own — so this is where looping is
    // put back. Restoring it only in the stop button's handler would leave
    // loop silently off after any other route out, including an empty take
    // that returns just below.
    post(Cmd::SetLooping, loopButton.getToggleState() ? 1.0 : 0.0);

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

    // The take's real duration, not a fixed guess. This used to be a flat four
    // beats however long the recording was, and that one number was the whole
    // of two separate faults: songEndBeats came back as four beats, so the
    // loop region collapsed to a bar and the transport wrapped seconds into
    // playback — while the audio kept going, because a track's sole audio clip
    // gets an unbounded window regardless. The result was a recording that
    // jumped back to its start shortly after beginning, and a transport that
    // couldn't run past the end of a take it had just made.
    const double takeSeconds = engine_.sampleRate() > 0.0
                                 ? (double) length / engine_.sampleRate() : 0.0;

    history_.edit("Record audio", [&path, &newTrackIndex, takeSeconds](model::Song& s)
    {
        const auto name = "Recording " + juce::String((int) s.tracks.size() + 1);
        model::addTrack(s, model::TrackType::Audio, name.toStdString());

        model::Clip clip;
        clip.id          = model::allocateId(s);
        clip.type        = model::ClipType::Audio;
        clip.startBeats  = 0.0;
        const double measured = engine::beatsForSeconds(takeSeconds, s.bpm);
        clip.lengthBeats = measured > 0.0 ? measured : 4.0;
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

/** Where synth presets live — one file per preset, listed by directory scan
    rather than through any index, the same "no bookkeeping beyond the
    filesystem itself" choice recordingsDirectory() already makes. */
juce::File MainComponent::presetsDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Presets");
    dir.createDirectory();
    return dir;
}

/** Rescans presetsDirectory() and pushes the names into the Synth pane's
    list. Called whenever the set of saved presets can have changed (save,
    delete, startup) — the list is never mutated in place, only rebuilt,
    since a directory scan is cheap and a project with a handful of presets
    is the expected case, not hundreds. */
void MainComponent::refreshPresetList()
{
    presetFiles_.clear();
    for (const auto& entry : juce::RangedDirectoryIterator(presetsDirectory(), false, "*.looperpreset",
                                                            juce::File::findFiles))
        presetFiles_.push_back(entry.getFile());

    std::sort(presetFiles_.begin(), presetFiles_.end(),
             [](const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });

    juce::StringArray names;
    for (const auto& file : presetFiles_)
    {
        model::SynthPreset preset;
        // An unreadable preset (hand-edited, half-written) is still listed
        // by filename rather than silently vanishing — invisible is worse
        // than ugly for something the user put there on purpose.
        names.add(model::deserializePreset(file.loadFileAsString().toStdString(), preset)
                     ? (preset.name.empty() ? file.getFileNameWithoutExtension() : juce::String(preset.name))
                     : file.getFileNameWithoutExtension());
    }
    synthEditor_.setPresetNames(names);
}

/** Prompts for a name and saves the selected track's synth settings and
    whole effect chain (built-ins and any hosted distortion plugin alike) as
    a new preset file. */
void MainComponent::savePresetDialog()
{
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    const auto& track = history_.current().tracks[(size_t) selectedTrackIndex_];
    if (track.type != model::TrackType::Instrument)
        return;

    auto* window = new juce::AlertWindow("Save Preset", "Name this preset:", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("name", track.name.empty() ? "My Preset" : (juce::String(track.name) + " Preset"));
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<MainComponent>(this), window,
             trackIndex = selectedTrackIndex_](int result)
            {
                if (self == nullptr || result != 1)
                    return;

                const auto name = window->getTextEditorContents("name").trim();
                if (name.isEmpty())
                    return;

                const auto& song = self->history_.current();
                if (trackIndex < 0 || trackIndex >= (int) song.tracks.size())
                    return;
                const auto& savedTrack = song.tracks[(size_t) trackIndex];

                model::SynthPreset preset;
                preset.name        = name.toStdString();
                preset.synth       = savedTrack.synthSettings;
                preset.effectChain = savedTrack.effectChain;

                const auto file = self->presetsDirectory()
                                      .getNonexistentChildFile(juce::File::createLegalFileName(name), ".looperpreset");
                if (file.replaceWithText(juce::String(model::serializePreset(preset))))
                {
                    self->refreshPresetList();
                    self->showStatus("Saved preset: " + name);
                }
                else
                {
                    self->showError("Could not save preset: " + name);
                }
            }),
        true);
}

/** Loads a preset onto the selected Instrument track, replacing its synth
    settings and whole effect chain as one undo step — a preset is one
    thing, not two separate edits a user would have to undo twice. */
void MainComponent::applyPreset(int index)
{
    if (index < 0 || index >= (int) presetFiles_.size())
        return;
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= trackCount())
        return;
    if (history_.current().tracks[(size_t) selectedTrackIndex_].type != model::TrackType::Instrument)
        return;

    model::SynthPreset preset;
    std::string        error;
    if (! model::deserializePreset(presetFiles_[(size_t) index].loadFileAsString().toStdString(), preset, &error))
    {
        showError("Could not load preset: " + juce::String(error));
        return;
    }

    const int trackIndex = selectedTrackIndex_;
    history_.edit("Load preset \"" + preset.name + "\"", [trackIndex, preset](model::Song& s)
    {
        auto& track = s.tracks[(size_t) trackIndex];
        track.synthSettings = preset.synth;
        track.effectChain   = preset.effectChain;
    });

    syncEngineTracks();
    refreshSynthEditorForSelected();
    refreshEffectChainForSelected();
    showStatus("Loaded preset: " + juce::String(preset.name));
}

/** Deletes a preset file and refreshes the list — no confirmation dialog,
    matching how this app treats every other delete (undo is the safety net
    for document edits, but a preset file is outside the document, so this
    one really is final; the list refresh and status message are the
    acknowledgment). */
void MainComponent::deletePresetAt(int index)
{
    if (index < 0 || index >= (int) presetFiles_.size())
        return;

    const auto file = presetFiles_[(size_t) index];
    const auto name = file.getFileNameWithoutExtension();

    if (file.deleteFile())
    {
        refreshPresetList();
        showStatus("Deleted preset: " + name);
    }
    else
    {
        showError("Could not delete preset: " + name);
    }
}

/** Populates an empty presets directory with a handful of starting points on
    first run, so the feature isn't an empty list the first time anyone
    opens it. Never touches a directory that already has anything in it —
    including a user who deleted every factory preset on purpose. */
void MainComponent::seedFactoryPresets()
{
    const auto dir = presetsDirectory();
    if (dir.getNumberOfChildFiles(juce::File::findFiles, "*.looperpreset") > 0)
        return;

    auto drive = [](float amount, float tone, float level, bool hardClip)
    {
        model::EffectSlot slot;
        slot.kind             = model::EffectKind::Drive;
        slot.enabled          = true;
        slot.drive.enabled    = true;
        slot.drive.drive      = amount;
        slot.drive.tone       = tone;
        slot.drive.level      = level;
        slot.drive.hardClip   = hardClip;
        slot.drive.cabinet    = true;
        return slot;
    };

    std::vector<model::SynthPreset> factory;

    {
        model::SynthPreset p;
        p.name             = "Warm Pad";
        p.synth.waveform   = 3; // triangle
        p.synth.attackMs   = 400.0f;
        p.synth.decayMs    = 600.0f;
        p.synth.sustain    = 0.8f;
        p.synth.releaseMs  = 1200.0f;
        p.synth.filterEnabled   = true;
        p.synth.filterMode      = 0;
        p.synth.filterCutoff    = 1800.0f;
        p.synth.filterResonance = 0.6f;

        model::EffectSlot chorus;
        chorus.kind          = model::EffectKind::Chorus;
        chorus.enabled       = true;
        chorus.chorus.enabled = true;
        chorus.chorus.rateHz = 0.4f;
        chorus.chorus.depth  = 0.6f;
        chorus.chorus.mix    = 0.5f;
        p.effectChain = { chorus };
        factory.push_back(std::move(p));
    }
    {
        model::SynthPreset p;
        p.name             = "Aggressive Bass";
        p.synth.waveform   = 1; // saw
        p.synth.attackMs   = 2.0f;
        p.synth.decayMs    = 80.0f;
        p.synth.sustain    = 0.9f;
        p.synth.releaseMs  = 60.0f;
        p.synth.filterEnabled   = true;
        p.synth.filterMode      = 0;
        p.synth.filterCutoff    = 500.0f;
        p.synth.filterResonance = 1.4f;

        model::EffectSlot compressor;
        compressor.kind                    = model::EffectKind::Compressor;
        compressor.enabled                 = true;
        compressor.compressor.enabled      = true;
        compressor.compressor.thresholdDb  = -20.0f;
        compressor.compressor.ratio        = 6.0f;
        p.effectChain = { drive(16.0f, 0.4f, 0.8f, false), compressor };
        factory.push_back(std::move(p));
    }
    {
        model::SynthPreset p;
        p.name             = "Dubstep Wobble Bass";
        p.synth.waveform   = 1; // saw
        p.synth.attackMs   = 1.0f;
        p.synth.decayMs    = 50.0f;
        p.synth.sustain    = 1.0f;
        p.synth.releaseMs  = 40.0f;

        model::EffectSlot wobble;
        wobble.kind                = model::EffectKind::Wobble;
        wobble.enabled             = true;
        wobble.wobble.enabled      = true;
        wobble.wobble.rateBeats    = 0.25f;
        wobble.wobble.depth        = 0.85f;
        wobble.wobble.baseCutoffHz = 150.0f;
        wobble.wobble.resonance    = 1.6f;
        p.effectChain = { drive(20.0f, 0.5f, 0.7f, true), wobble };
        factory.push_back(std::move(p));
    }
    {
        model::SynthPreset p;
        p.name             = "Bright Pluck";
        p.synth.waveform   = 2; // square
        p.synth.attackMs   = 1.0f;
        p.synth.decayMs    = 220.0f;
        p.synth.sustain    = 0.0f;
        p.synth.releaseMs  = 80.0f;

        model::EffectSlot tremolo;
        tremolo.kind            = model::EffectKind::Tremolo;
        tremolo.enabled         = true;
        tremolo.tremolo.enabled = true;
        tremolo.tremolo.rateHz  = 6.0f;
        tremolo.tremolo.depth   = 0.3f;
        p.effectChain = { tremolo };
        factory.push_back(std::move(p));
    }

    for (const auto& preset : factory)
    {
        const auto file = dir.getNonexistentChildFile(juce::File::createLegalFileName(preset.name), ".looperpreset");
        file.replaceWithText(juce::String(model::serializePreset(preset)));
    }
}

/** Where the procedurally-generated starter drum sounds live — same
    "own directory, listed by scanning it" pattern as recordingsDirectory()
    and presetsDirectory(). */
juce::File MainComponent::factoryDrumKitDirectory() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("Looper-Audio Factory Kit");
    dir.createDirectory();
    return dir;
}

/** Writes twelve one-shot .wav files (three kicks, three snares, three
    closed hats, three claps — see engine::DrumSynth) into
    factoryDrumKitDirectory(), if it's empty. Never touches a directory that
    already has anything in it, same reasoning as seedFactoryPresets(): a
    user who removed or replaced these on purpose keeps that choice. */
void MainComponent::seedFactoryDrumKit()
{
    const auto dir = factoryDrumKitDirectory();
    if (dir.getNumberOfChildFiles(juce::File::findFiles, "*.wav") > 0)
        return;

    // Fixed rather than the live device rate: these are rendered once, to
    // disk, and reused across sessions — re-rendering at whatever rate the
    // audio device happens to be running would make two machines' factory
    // kits differ for no reason, and AudioFilePlayerNode already resamples
    // whatever a file's own rate is.
    const double sampleRate = 48000.0;

    auto writeOneShot = [&](const juce::String& name, std::vector<float> samples)
    {
        engine::normalizePeak(samples);
        juce::AudioBuffer<float> buffer(1, (int) samples.size());
        std::copy(samples.begin(), samples.end(), buffer.getWritePointer(0));
        engine::OfflineRenderer::writeWav(dir.getChildFile(name + ".wav"), buffer, sampleRate);
    };

    // Three variants each, so "generic" doesn't mean "one option" — a user
    // who doesn't like the default can swap to another via the existing
    // per-pad Load... button without leaving the app.
    writeOneShot("Kick Tight",  engine::synthesizeKick(sampleRate, 180.0f, 55.0f, 15.0f, 150.0f, 2.0f));
    writeOneShot("Kick Punchy", engine::synthesizeKick(sampleRate, 160.0f, 45.0f, 25.0f, 250.0f, 3.0f));
    writeOneShot("Kick Deep",   engine::synthesizeKick(sampleRate, 120.0f, 35.0f, 40.0f, 400.0f, 1.5f));

    writeOneShot("Snare Crisp", engine::synthesizeSnare(sampleRate, 200.0f, 0.25f, 140.0f, 3000.0f, 201u));
    writeOneShot("Snare Fat",   engine::synthesizeSnare(sampleRate, 180.0f, 0.4f,  220.0f, 1800.0f, 202u));
    writeOneShot("Snare Tight", engine::synthesizeSnare(sampleRate, 220.0f, 0.2f,  100.0f, 2500.0f, 203u));

    writeOneShot("Hat Closed", engine::synthesizeHat(sampleRate, 7000.0f, 60.0f, 301u));
    writeOneShot("Hat Tight",  engine::synthesizeHat(sampleRate, 9000.0f, 35.0f, 302u));
    writeOneShot("Hat Bright", engine::synthesizeHat(sampleRate, 11000.0f, 90.0f, 303u));

    writeOneShot("Clap Classic", engine::synthesizeClap(sampleRate, 1500.0f, 120.0f, 401u));
    writeOneShot("Clap Tight",   engine::synthesizeClap(sampleRate, 1800.0f, 80.0f, 402u));
    writeOneShot("Clap Roomy",   engine::synthesizeClap(sampleRate, 1200.0f, 200.0f, 403u));
}

/** model::makeDefaultDrumKit()'s four pads (Kick/Snare/Hat/Other), pointed
    at one factory sound each — the "Tight"/"Crisp"/"Closed"/"Classic"
    variant of each, arbitrarily chosen as the one that plays if nobody
    picks. The other two variants of each still exist in
    factoryDrumKitDirectory() for anyone who wants to swap. Can't live in
    model::makeDefaultDrumKit() itself: a file path is exactly the kind of
    thing the model layer doesn't know about. */
model::DrumKit MainComponent::defaultDrumKitWithFactorySamples() const
{
    auto       kit = model::makeDefaultDrumKit();
    const auto dir = factoryDrumKitDirectory();

    kit.pads[0].samplePath = dir.getChildFile("Kick Tight.wav").getFullPathName().toStdString();
    kit.pads[1].samplePath = dir.getChildFile("Snare Crisp.wav").getFullPathName().toStdString();
    kit.pads[2].samplePath = dir.getChildFile("Hat Closed.wav").getFullPathName().toStdString();
    kit.pads[3].samplePath = dir.getChildFile("Clap Classic.wav").getFullPathName().toStdString();
    return kit;
}

/** The project that exists the moment the app opens — deliberately not also
    what "New Project" resets to (createEmptyProject() stays a blank synth
    track with an empty clip, on purpose: a user who explicitly asks for a
    new project most likely wants a clean canvas, not a demo). A drum track
    with a programmed loop and real sounds is what makes a *fresh launch*
    audible immediately rather than opening on silence twice over — an
    empty synth clip and a kit with nothing assigned to it. */
model::Song MainComponent::makeStarterSong() const
{
    model::Song song;

    const int synthId = model::addTrack(song, model::TrackType::Instrument, "Synth 1").id;
    model::Clip synthClip;
    synthClip.type        = model::ClipType::Instrument;
    synthClip.pattern     = pianoRoll_.pattern();
    synthClip.lengthBeats = synthClip.pattern.lengthBeats;
    model::addClip(song, synthId, synthClip);

    const int drumId = model::addTrack(song, model::TrackType::Drum, "Drums 1").id;
    song.tracks.back().drumKit = defaultDrumKitWithFactorySamples();
    model::Clip drumClip;
    drumClip.type        = model::ClipType::Instrument;
    drumClip.pattern     = engine::makeDefaultDrumLoopPattern();
    drumClip.lengthBeats = drumClip.pattern.lengthBeats;
    model::addClip(song, drumId, drumClip);

    return song;
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

/** As showStatus, but forces the message on screen before returning — for
    the handful of actions (bounce, plugin scan) that then block the message
    thread for real work. A plain showStatus() only marks the banner dirty;
    without a peer repaint forced here, that paint request would just sit
    queued behind the very call that's about to freeze the UI, and the
    message would never be seen until after the freeze was already over. */
void MainComponent::showBusy(const juce::String& message)
{
    status_.show(message, false);
    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();
}

void MainComponent::wireUndoableSlider(juce::Slider& slider, juce::String label,
                                       std::function<float(const model::Song&)> read,
                                       std::function<void(model::Song&, float)> write)
{
    // Shared, not a member: this is called once per slider (there are over a
    // dozen in the master panel alone), and a dedicated member per slider is
    // exactly the per-control bookkeeping this helper exists to avoid.
    auto dragFrom = std::make_shared<float>(0.0f);

    slider.onDragStart = [this, dragFrom, read] { *dragFrom = read(history_.current()); };
    slider.onDragEnd = [this, dragFrom, label, read, write]
    {
        const float landedOn = read(history_.current());
        commitDrag(history_, label.toStdString(), *dragFrom, landedOn, write);
    };
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
        showBusy("Rendering to WAV...");

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
    addGuitarTrackButton_.setEnabled(trackCount() < engine_.maxTracks());
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
        const double intoClip = uiTempoMap_.ppqFromSamples(playhead) - clipStart;
        drumsPane_.setPlayheadBeats(intoClip, engine_.isPlaying());

        // Wrapped into the pattern, because a clip loops: the engine wraps
        // playback within the pattern length, so an unwrapped position would
        // walk off the right of the grid on the first repeat and never
        // return. Only shown while the clip is actually under the playhead.
        const double patternBeats = currentPattern().lengthBeats;
        const bool   inClip       = intoClip >= 0.0 && engine_.isPlaying();
        pianoRoll_.setPlayheadBeats(patternBeats > 0.0 ? engine::wrapPositive(intoClip, patternBeats)
                                                       : 0.0,
                                    inClip);
        followKeysPlayhead();
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
    area.removeFromTop(6);

    // Below tempo, sharing its label gutter: they are the two things that
    // decide what a bar is.
    timeSigBox_.setBounds(area.removeFromTop(24).withTrimmedLeft(64).removeFromLeft(90));
}

/** Shows or hides everything below the transport's button row. The arrow
    points the way the content will go, so it reads the same whichever state
    it's in. */
void MainComponent::applyTransportCollapse()
{
    juce::Component* belowFirstRow[] = { &positionLabel, &clipLabel, &tempoSlider,
                                        &timeSigBox_, &timeSigLabel_ };
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
        workspace_.addPanel(*bottom, "Session");
        workspace_.addPanel(*bottom, "Guitar");
        bottom->showPanel("Keys");

        // Track FX gets its own region rather than joining the tab group
        // above: it applies to every track type, but a distortion plugin
        // living in it was otherwise invisible while tweaking a synth's
        // oscillator on the "Synth" tab right next to it — a click away
        // rather than in view.
        if (auto* fx = workspace_.splitRegion(*bottom, DropZone::Right, 0.62))
            workspace_.addPanel(*fx, "Track FX");

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

/** Builds one of the zoom controls: icon, slider and editable multiplier.

    Shared by the tracks and keys panes. They zoom different axes — time in
    one, pitch in the other — but the control is the same thing and reads the
    same way, so it is built in one place. */
void MainComponent::setUpZoomControls(juce::Component& parent, juce::DrawableButton& icon,
                                      juce::Slider& slider, juce::Slider& box,
                                      double minZoom, double maxZoom,
                                      const juce::String& tooltip,
                                      std::function<void(float)> onZoom)
{
    // A DrawableButton in ImageFitted mode, as every other SVG in this app
    // uses. Clicks are switched off: this labels the slider, it isn't a
    // control.
    auto magnifier = icons::fromSvg(icons::kMagnifier);
    icon.setImages(magnifier.get());
    icon.setInterceptsMouseClicks(false, false);
    icon.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    parent.addAndMakeVisible(icon);

    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setRange(minZoom, maxZoom, 0.0);
    // Zoom is multiplicative, so a linear track would put x1 a fifth of the
    // way along and give most of the travel to zooming in. Skewing about the
    // midpoint puts x1 in the middle, where it belongs.
    slider.setSkewFactorFromMidPoint(1.0);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setTooltip(tooltip);
    slider.onValueChange = [&slider, onZoom] { onZoom((float) slider.getValue()); };
    parent.addAndMakeVisible(slider);

    box.setSliderStyle(juce::Slider::LinearBar); // a text field with a drag, not a track
    box.setRange(minZoom, maxZoom, 0.0);
    box.setSkewFactorFromMidPoint(1.0);
    box.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 20);
    // "x1.00" rather than "1.00 x": JUCE's suffix appends, and a multiplier
    // reads as a multiplier only with the x in front.
    box.textFromValueFunction = [](double value) { return "x" + juce::String(value, 2); };
    box.valueFromTextFunction = [](const juce::String& text)
    {
        return text.retainCharacters("0123456789.").getDoubleValue();
    };
    box.setTooltip(tooltip + " - type a multiplier, or drag");
    box.onValueChange = [&box, onZoom] { onZoom((float) box.getValue()); };
    parent.addAndMakeVisible(box);
}

/** Applies a new pitch zoom to the keys pane and keeps its controls
    describing it. The roll stores a row count, so the zoom lands on the
    nearest achievable window and the controls are set from where it landed
    rather than from what was asked for. */
void MainComponent::setKeysZoom(float zoom)
{
    pianoRoll_.setPitchZoom(zoom);
    updateKeysZoomControls();
}

void MainComponent::updateKeysZoomControls()
{
    const double zoom = pianoRoll_.pitchZoom();
    keysZoomSlider_.setValue(zoom, juce::dontSendNotification);
    keysZoomBox_.setValue(zoom, juce::dontSendNotification);
}

/** Keeps the playhead in view while the keys pane is scrolled.

    Only while playing: scrolling the view out from under someone who is
    editing a stopped pattern would be worse than the problem it solves. The
    paging rule itself is scrollToFollow, which is JUCE-free and tested — it
    pages rather than centring, so the grid stays still while the playhead
    crosses it instead of sliding continuously under a fixed line. */
void MainComponent::followKeysPlayhead()
{
    if (! keysFollowButton_.getToggleState() || ! engine_.isPlaying())
        return;

    const int viewportWidth = keysViewport_.getMaximumVisibleWidth();
    const int contentWidth  = pianoRoll_.getWidth();
    if (viewportWidth <= 0 || contentWidth <= viewportWidth)
        return; // nothing to scroll

    const int current = keysViewport_.getViewPositionX();
    const int wanted  = scrollToFollow((int) pianoRoll_.playheadX(), current,
                                       viewportWidth, contentWidth, kKeysFollowMargin);

    if (wanted != current)
        keysViewport_.setViewPosition(wanted, keysViewport_.getViewPositionY());
}

/** Widens the grid and lets the viewport scroll it. Unlike pitch zoom, which
    the roll stores as a row count and snaps, this is continuous — the roll
    simply draws to whatever width it's given. */
void MainComponent::setKeysTimeZoom(float zoom)
{
    pianoRoll_.setTimeZoom(zoom);
    updateKeysTimeZoomControls();
    layoutEditTab();
}

void MainComponent::updateKeysTimeZoomControls()
{
    const double zoom = pianoRoll_.timeZoom();
    keysTimeZoomSlider_.setValue(zoom, juce::dontSendNotification);
    keysTimeZoomBox_.setValue(zoom, juce::dontSendNotification);
}

/** Applies a new timeline zoom and keeps the controls describing it. */
void MainComponent::setTimelineZoom(float zoom)
{
    arrangementView_.setZoom(zoom);
    updateZoomControls();
}

/** Mirrors the current zoom into both controls without either of them
    reporting it straight back as a user edit — they set each other, and the
    keyboard shortcuts set both. */
void MainComponent::updateZoomControls()
{
    const double zoom = arrangementView_.zoom();
    zoomSlider_.setValue(zoom, juce::dontSendNotification);
    zoomBox_.setValue(zoom, juce::dontSendNotification);
}

void MainComponent::layoutArrangeTab()
{
    auto area = arrangeTab_.getLocalBounds();

    auto toolbar = area.removeFromTop(28).reduced(4, 2);

    zoomIcon_.setBounds(toolbar.removeFromLeft(24));
    toolbar.removeFromLeft(2);
    zoomSlider_.setBounds(toolbar.removeFromLeft(120));
    toolbar.removeFromLeft(6);
    zoomBox_.setBounds(toolbar.removeFromLeft(56));
    toolbar.removeFromLeft(12);
    addClipButton_.setBounds(toolbar.removeFromLeft(90));

    arrangementViewport_.setBounds(area);
}

void MainComponent::layoutEditTab()
{
    auto area   = editTab_.getLocalBounds();
    auto header = area.removeFromTop(24);

    barsBox_.setBounds(header.removeFromRight(56).reduced(2, 0));
    barsLabel_.setBounds(header.removeFromRight(34));

    header.removeFromRight(10);
    keysZoomBox_.setBounds(header.removeFromRight(52).reduced(0, 2));
    keysZoomSlider_.setBounds(header.removeFromRight(80).reduced(2, 1));
    keysZoomIcon_.setBounds(header.removeFromRight(22).reduced(0, 1));

    header.removeFromRight(10);
    keysTimeZoomBox_.setBounds(header.removeFromRight(52).reduced(0, 2));
    keysTimeZoomSlider_.setBounds(header.removeFromRight(80).reduced(2, 1));
    keysTimeZoomIcon_.setBounds(header.removeFromRight(22).reduced(0, 1));

    header.removeFromRight(8);
    keysFollowButton_.setBounds(header.removeFromRight(72).reduced(0, 2));

    editingLabel_.setBounds(header.reduced(6, 0));

    keysViewport_.setBounds(area);

    // The roll is as tall as the pane — rows fill it, and pitch zoom decides
    // how many — and as wide as the time zoom asks for, which is what the
    // viewport then scrolls.
    const int visibleWidth = juce::jmax(1, keysViewport_.getMaximumVisibleWidth());
    pianoRoll_.setSize(juce::jmax(visibleWidth, pianoRoll_.preferredWidth(visibleWidth)),
                       juce::jmax(1, keysViewport_.getMaximumVisibleHeight()));
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
