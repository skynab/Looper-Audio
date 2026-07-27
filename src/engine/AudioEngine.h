#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "rt/SpscRingBuffer.h"

#include "engine/AudioClipSlot.h"
#include "engine/AudioFilePlayerNode.h"
#include "engine/DrumKitNode.h"
#include "engine/AudioRecorder.h"
#include "engine/ClipSlot.h"
#include "engine/DelayEffect.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"
#include "engine/EngineCommand.h"
#include "engine/InstrumentTrack.h"
#include "engine/MasterBusNode.h"
#include "engine/Metronome.h"
#include "engine/PluginHost.h"
#include "engine/PluginNode.h"
#include "engine/Pattern.h"
#include "engine/Transport.h"

namespace looper::engine
{
/** One audio clip to load onto a track: a file plus its
    [startBeats, startBeats + lengthBeats) window — the AudioEngine-facing
    equivalent of ClipSlot, taking a file instead of already-decoded data.
    See AudioEngine::setTrackAudioClips. */
struct AudioClipSpec
{
    juce::File file;
    double     startBeats  = 0.0;
    double     lengthBeats = 0.0;
};

/** One drum pad to load onto a track: a note number, the file to play when
    it's triggered (File{} = no sample assigned, pad stays silent), and that
    pad's mix settings. `muted` is the *effective* mute — the caller resolves
    the kit's solo state into it (see MainComponent::syncEngineTracks), the
    same "solo overrides, mute always wins" rule tracks use, so the audio
    thread never has to scan the other pads. Defaults are a no-op, so a spec
    built without touching them behaves as it did before these existed. See
    AudioEngine::setTrackDrumKit. */
struct DrumPadSpec
{
    int        noteNumber = -1;
    juce::File file;

    float gainDb         = 0.0f;
    float pan            = 0.0f;
    float pitchSemitones = 0.0f;
    bool  muted          = false;
};

/**
    The headless audio engine. It owns the audio device and is the device
    callback. Instrument tracks live in a fixed pre-allocated pool, so the UI
    changes the "song" by activating slots and submitting clip lists — no
    real-time graph editing. The UI interacts only by posting commands, calling
    the thread-safe control methods (which use lock-free FIFOs / atomics), and
    reading published atomics.
*/
class AudioEngine final : public juce::AudioIODeviceCallback,
                          public juce::MidiInputCallback
{
public:
    static constexpr int kMaxTracks = 8;

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }
    juce::MidiKeyboardState&  keyboardState() noexcept { return keyboardState_; }

    void postCommand(const EngineCommand& command) noexcept { commandQueue_.push(command); }

    /** Decode an audio file into RAM and hand it to the global preview player
        (used by the File > Import Audio quick-preview). Message thread. */
    bool loadAudioFile(const juce::File& file);

    /** Reads just @p file's header to get its duration — cheap (no sample
        decode), unlike loadAudioFile/setTrackAudioClips. Returns 0.0 if the
        file can't be read. Used to size a new clip to its actual duration
        rather than a fixed guess. Message thread. */
    double probeDurationSeconds(const juce::File& file);

    /** Replaces a track's whole audio-clip list, decoding any file not
        already cached (see decodeOrGetCached — decoded audio is cached by
        path, so calling this again with the same files, even on other
        tracks, never re-decodes them). Each clip plays only within its own
        [startBeats, startBeats + lengthBeats) window, same rule as
        setTrackClips (MIDI); give a single-clip track an effectively
        unbounded lengthBeats for the original "plays once from its start, no
        other gating" behaviour. Message thread. Returns false if any clip's
        file couldn't be read (the others still load). */
    bool setTrackAudioClips(int index, const std::vector<AudioClipSpec>& clips);

    /** Marks a track as driving its drum kit instead of its synth for any
        notes it receives (see InstrumentTrack::isDrumTrack) — unlike audio
        clips, the synth doesn't naturally stay silent without content, so
        this routing has to be explicit. Message thread. */
    void setTrackIsDrum(int index, bool isDrum);

    /** Replaces a track's whole drum-kit pad→sample mapping, decoding any
        file not already cached (see decodeOrGetCached — same cache
        setTrackAudioClips uses, so a sample shared across pads or tracks is
        never decoded twice). A pad with no file (or one that fails to
        decode) stays silent. Message thread. */
    void setTrackDrumKit(int index, const std::vector<DrumPadSpec>& pads);

    // Metronome (thread-safe atomics). Summed in after the master chain, so
    // it never passes through the master effects or reaches the meter — and
    // the offline renderer has none at all, so it can't reach a bounce.
    void setMetronomeEnabled(bool enabled) { metronome_.setEnabled(enabled); }
    bool isMetronomeEnabled() const        { return metronome_.isEnabled(); }
    void setMetronomeLevel(float level)    { metronome_.setLevel(level); }

    /** Bars of count-in before a recording starts capturing (0 = none). The
        click sounds through the count-in whether or not the metronome is
        otherwise switched on. */
    void setCountInBars(int bars) { countInBars_ = juce::jmax(0, bars); }
    int  countInBars() const noexcept { return countInBars_; }

    // ---- recording (message thread) ----
    /** Arms the recorder. Returns false (and arms nothing) if the current
        audio device has no active input channels. Capturing only actually
        happens while the transport is playing, and only after any count-in
        (see setCountInBars) has elapsed. */
    bool beginRecording();

    /** True while a count-in is still running — the transport is rolling but
        nothing is being captured yet. */
    bool isCountingIn() const noexcept { return recorder_.leadInRemaining() > 0; }
    /** Stops capturing; the take becomes readable once isRecordingFinished(). */
    void stopRecording() { recorder_.disarm(); }
    bool isRecordingFinished() const noexcept { return recorder_.isFinished(); }
    int  recordedSampleCount() const noexcept { return recorder_.recordedSampleCount(); }
    /** Valid only after isRecordingFinished() is observed true. */
    const juce::AudioBuffer<float>& recordedTakeBuffer() const noexcept { return recorder_.takeBuffer(); }
    int recordedTakeLength() const noexcept { return recorder_.takeLength(); }

    // ---- multi-track control (message thread) ----
    int  maxTracks() const noexcept { return kMaxTracks; }
    void setActiveTrackCount(int count);
    /** Replaces a track's whole clip list. Each clip plays only within its own
        [startBeats, startBeats + lengthBeats) window; give a single-clip track
        an effectively unbounded lengthBeats to keep it looping indefinitely. */
    void setTrackClips(int index, const std::vector<ClipSlot>& clips);
    void setTrackMuted(int index, bool muted);
    void setTrackSolo(int index, bool solo);
    void setTrackGainDb(int index, float gainDb);
    void setTrackPan(int index, float pan);
    void setTrackSendLevel(int index, float level);

    // ---- session view (message thread) ----
    /** Replaces a track's session column. Slot index is the scene. */
    void setTrackSessionSlots(int index, const std::vector<SessionSlotData>& slots);

    /** Asks a track to start @p sceneIndex at the next launch boundary. */
    void launchSessionSlot(int index, int sceneIndex);

    /** Asks a track to stop whatever session clip it's playing, handing it
        back to the arrangement. */
    void stopSessionSlot(int index);

    /** Launches a whole scene across every active track — a track with an
        empty slot in that scene stops rather than carrying on, so a scene is a
        complete statement of what should be playing. */
    void launchScene(int sceneIndex);

    /** Stops every track's session clip. */
    void stopAllSessionSlots();

    /** Which session slot a track is currently playing, or -1. Lock-free
        readout for the session grid. */
    int sessionSlotPlaying(int index) const noexcept
    {
        return (index >= 0 && index < kMaxTracks)
                   ? tracks_[(size_t) index].session.playingSlotForUI() : -1;
    }

    /** How long a launch boundary is, in beats. 0 launches immediately;
        the default of one bar is what makes launching musical. */
    void setLaunchQuantumBeats(double beats) { launchQuantumBeats_.store(beats, std::memory_order_relaxed); }
    double launchQuantumBeats() const noexcept { return launchQuantumBeats_.load(std::memory_order_relaxed); }

    /** Replaces a track's automation curves. Sample-accurate: the track
        ramps them across each block itself rather than the UI poking a
        value in every 33ms. Pass nullptr-equivalent (an empty set) to
        clear. Message thread. */
    void setTrackAutomation(int index, const TrackAutomation& curves);
    void setArmedTrack(int index);

    // Per-track synth timbre (see model::SynthSettings / SynthInstrumentNode)
    // — meaningless for a Drum track, but harmless to set regardless since
    // it's simply not read while isDrumTrack routes notes to the drum kit.
    void setTrackSynthWaveform(int index, int waveform);
    void setTrackSynthAttackMs(int index, float ms);
    void setTrackSynthDecayMs(int index, float ms);
    void setTrackSynthSustain(int index, float level);
    void setTrackSynthReleaseMs(int index, float ms);
    void setTrackSynthFilterEnabled(int index, bool enabled);
    void setTrackSynthFilterMode(int index, int mode);
    void setTrackSynthFilterCutoff(int index, float hz);
    void setTrackSynthFilterResonance(int index, float q);
    void setTrackSynthGainDb(int index, float db);

    /** Replaces a track's insert chain with nodes of these kinds, in order.
        Structural only: rebuilding allocates (on this thread) and resets every
        tail in the chain, so parameter changes must go through the setters
        below instead. A no-op when the structure already matches, which is
        what keeps an unrelated document edit from glitching a delay tail. */
    /** Returns true if the chain was actually rebuilt — which destroys the
        old nodes, hosted plugins included. The caller must close anything
        pointing at them first (see MainComponent::closePluginEditors): an
        editor outliving its processor is a crash, not a glitch. */
    bool setTrackEffectChain(int index, const std::vector<EffectSlotSpec>& slots);

    /** The plugin host, for the UI's browser and scan. Message thread. */
    PluginHost& pluginHost() noexcept { return pluginHost_; }

    /** Applies one chain slot's parameters, addressed by position. Safe from
        the message thread: these are atomics inside nodes it built and still
        holds a pointer to. Addressed by index rather than by kind so a chain
        with two filters is editable at all. */
    void setTrackEffectSlotParams(int index, int slotIndex, const EffectSlotParams& params);

    /** A hosted plugin in a track's chain, for opening its editor. nullptr if
        that slot isn't a plugin (or the chain is a rebuild behind). Message
        thread. */
    PluginNode* trackPluginNode(int index, int slotIndex);

    // Master effects (thread-safe atomics; safe to call from the message thread).
    void setMasterFilterEnabled(bool enabled)  { masterFilter_.setEnabled(enabled); }
    void setMasterFilterMode(int mode)         { masterFilter_.setMode(mode); }
    void setMasterFilterCutoff(float hz)       { masterFilter_.setCutoff(hz); }
    void setMasterFilterResonance(float q)     { masterFilter_.setResonance(q); }

    void setMasterDelayEnabled(bool enabled)   { masterDelay_.setEnabled(enabled); }
    void setMasterDelayTimeMs(float ms)        { masterDelay_.setTimeMs(ms); }
    void setMasterDelayFeedback(float amount)  { masterDelay_.setFeedback(amount); }
    void setMasterDelayMix(float amount)       { masterDelay_.setMix(amount); }

    void setMasterReverbEnabled(bool enabled)  { masterReverb_.setEnabled(enabled); }
    void setMasterReverbRoomSize(float v)      { masterReverb_.setRoomSize(v); }
    void setMasterReverbDamping(float v)       { masterReverb_.setDamping(v); }
    void setMasterReverbMix(float v)           { masterReverb_.setMix(v); }

    // Shared send bus: every track can send a pre-fader portion of its signal
    // into one always-fully-wet effect — reverb or delay, chosen by
    // setSendBusEffectType — which mixes back into the master before the
    // master's own effects chain (thread-safe atomics).
    void setSendBusEnabled(bool enabled)  { sendBusEnabled_.store(enabled, std::memory_order_relaxed); }
    /** 0 = reverb, 1 = delay. */
    void setSendBusEffectType(int type)   { sendBusEffectType_.store(type, std::memory_order_relaxed); }
    void setSendBusRoomSize(float v)      { sendBusReverb_.setRoomSize(v); }
    void setSendBusDamping(float v)       { sendBusReverb_.setDamping(v); }
    void setSendBusDelayTimeMs(float ms)  { sendBusDelay_.setTimeMs(ms); }
    void setSendBusDelayFeedback(float v) { sendBusDelay_.setFeedback(v); }
    void setSendBusReturnLevel(float v)   { sendReturnGain_.store(v, std::memory_order_relaxed); }

    /** Housekeeping to run periodically on the message thread (frees retired clips/patterns). */
    void pump() noexcept;

    juce::String loadedClipName() const             { return loadedClipName_; }
    double       loadedClipSeconds() const noexcept { return loadedClipSeconds_; }

    // ---- lock-free UI readouts ----
    bool    isPlaying() const noexcept       { return transport_.playingForUI(); }
    int64_t playheadSamples() const noexcept { return transport_.playheadForUI(); }
    double  sampleRate() const noexcept      { return sampleRate_.load(std::memory_order_relaxed); }
    float   masterPeak(int channel) const noexcept { return master_.peak(channel); }
    float   trackPeak(int index, int channel) const noexcept
    {
        return (index >= 0 && index < kMaxTracks) ? tracks_[(size_t) index].peak(channel) : 0.0f;
    }

    // ---- juce::AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // ---- juce::MidiInputCallback ----
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

private:
    void drainCommandQueue() noexcept;
    /** Decodes @p file fully into RAM. Returns nullptr if it can't be read. Message thread. */
    std::unique_ptr<ClipData> decodeAudioFile(const juce::File& file);
    /** As above, but cached by absolute path — repeated calls (even from
        different tracks) reuse the same decoded ClipData instead of
        re-reading the file. Message thread only; the cache is never touched
        from the audio thread. */
    std::shared_ptr<ClipData> decodeOrGetCached(const juce::File& file);

    juce::AudioDeviceManager          deviceManager_;
    juce::AudioFormatManager          formatManager_;
    juce::MidiMessageCollector        midiCollector_;
    juce::MidiKeyboardState           keyboardState_;
    juce::MidiBuffer                  incomingMidi_;
    rt::SpscRingBuffer<EngineCommand> commandQueue_ { 1024 };

    std::array<InstrumentTrack, kMaxTracks> tracks_;
    std::atomic<int>                        armedTrack_ { 0 };

    AudioFilePlayerNode filePlayer_;
    FilterEffect        masterFilter_;
    DelayEffect         masterDelay_;
    ReverbEffect        masterReverb_;
    MasterBusNode       master_;
    Transport           transport_;

    // Send bus: accumulated from every track's pre-fader send, passed through
    // one always-fully-wet effect (sendBusEffectType_: 0 = reverb, 1 =
    // delay), and mixed back into the main output before the master effects
    // chain. Both effect instances stay prepared/configured regardless of
    // which is selected, so switching types takes effect immediately.
    juce::AudioBuffer<float> sendBus_;
    ReverbEffect             sendBusReverb_;
    DelayEffect              sendBusDelay_;
    std::atomic<bool>        sendBusEnabled_    { false };
    std::atomic<int>         sendBusEffectType_ { 0 };
    std::atomic<float>       sendReturnGain_    { 0.0f };

    std::atomic<double> sampleRate_ { 0.0 };

    AudioRecorder recorder_;
    Metronome     metronome_;
    int           countInBars_ = 0; // message thread only; read when arming
    std::atomic<double> launchQuantumBeats_ { 4.0 }; // one bar of 4/4

    /** Rebuilds and submits a track's chain from chainStructure_. Message
        thread. Also called when the device (re)starts, since a chain must be
        prepared for the sample rate it will actually run at. */
    void rebuildTrackEffectChain(int index);

    // Message-thread view of each track's chain: the structure it was built
    // from, and a pointer to the chain last submitted. The pointer is how
    // parameter setters reach live nodes — safe because the newest chain is
    // never the one being reclaimed.
    std::array<std::vector<EffectSlotSpec>, kMaxTracks> chainStructure_;
    PluginHost                                         pluginHost_;
    std::array<EffectChain*, kMaxTracks>                submittedChain_ {};
    int                                                 currentBlockSize_ = 512;

    juce::String loadedClipName_;
    double       loadedClipSeconds_ = 0.0;

    // Decoded-audio cache, keyed by absolute path (message thread only) — see
    // decodeOrGetCached.
    std::map<juce::String, std::shared_ptr<ClipData>> audioDecodeCache_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace looper::engine
