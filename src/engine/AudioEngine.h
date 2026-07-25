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
#include "engine/AudioRecorder.h"
#include "engine/ClipSlot.h"
#include "engine/DelayEffect.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"
#include "engine/EngineCommand.h"
#include "engine/InstrumentTrack.h"
#include "engine/MasterBusNode.h"
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

    // ---- recording (message thread) ----
    /** Arms the recorder. Returns false (and arms nothing) if the current
        audio device has no active input channels. Capturing only actually
        happens while the transport is playing. */
    bool beginRecording();
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
    void setTrackSendLevel(int index, float level);
    void setArmedTrack(int index);

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
    // into this always-fully-wet reverb, which mixes back into the master
    // before the master's own effects chain (thread-safe atomics).
    void setSendBusEnabled(bool enabled)  { sendBusEnabled_.store(enabled, std::memory_order_relaxed); }
    void setSendBusRoomSize(float v)      { sendBusReverb_.setRoomSize(v); }
    void setSendBusDamping(float v)       { sendBusReverb_.setDamping(v); }
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

    // Send bus: accumulated from every track's pre-fader send, reverberated,
    // and mixed back into the main output before the master effects chain.
    juce::AudioBuffer<float> sendBus_;
    ReverbEffect             sendBusReverb_;
    std::atomic<bool>        sendBusEnabled_ { false };
    std::atomic<float>       sendReturnGain_ { 0.0f };

    std::atomic<double> sampleRate_ { 0.0 };

    AudioRecorder recorder_;

    juce::String loadedClipName_;
    double       loadedClipSeconds_ = 0.0;

    // Decoded-audio cache, keyed by absolute path (message thread only) — see
    // decodeOrGetCached.
    std::map<juce::String, std::shared_ptr<ClipData>> audioDecodeCache_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace looper::engine
