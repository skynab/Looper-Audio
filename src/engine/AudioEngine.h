#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "rt/SpscRingBuffer.h"

#include "engine/AudioFilePlayerNode.h"
#include "engine/EngineCommand.h"
#include "engine/InstrumentTrack.h"
#include "engine/MasterBusNode.h"
#include "engine/Pattern.h"
#include "engine/Transport.h"

namespace looper::engine
{
/**
    The headless audio engine. It owns the audio device and is the device
    callback. Instrument tracks live in a fixed pre-allocated pool, so the UI
    changes the "song" by activating slots and submitting patterns — no real-time
    graph editing. The UI interacts only by posting commands, calling the
    thread-safe control methods (which use lock-free FIFOs / atomics), and reading
    published atomics.
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

    /** Decode an audio file into RAM and hand it to the file-player node. Message thread. */
    bool loadAudioFile(const juce::File& file);

    // ---- multi-track control (message thread) ----
    int  maxTracks() const noexcept { return kMaxTracks; }
    void setActiveTrackCount(int count);
    void setTrackPattern(int index, const Pattern& pattern);
    void setTrackMuted(int index, bool muted);
    void setTrackGainDb(int index, float gainDb);
    void setArmedTrack(int index);

    /** Housekeeping to run periodically on the message thread (frees retired clips/patterns). */
    void pump() noexcept;

    juce::String loadedClipName() const             { return loadedClipName_; }
    double       loadedClipSeconds() const noexcept { return loadedClipSeconds_; }

    // ---- lock-free UI readouts ----
    bool    isPlaying() const noexcept       { return transport_.playingForUI(); }
    int64_t playheadSamples() const noexcept { return transport_.playheadForUI(); }
    double  sampleRate() const noexcept      { return sampleRate_.load(std::memory_order_relaxed); }
    float   masterPeak(int channel) const noexcept { return master_.peak(channel); }

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

    juce::AudioDeviceManager          deviceManager_;
    juce::AudioFormatManager          formatManager_;
    juce::MidiMessageCollector        midiCollector_;
    juce::MidiKeyboardState           keyboardState_;
    juce::MidiBuffer                  incomingMidi_;
    rt::SpscRingBuffer<EngineCommand> commandQueue_ { 1024 };

    std::array<InstrumentTrack, kMaxTracks> tracks_;
    std::atomic<int>                        armedTrack_ { 0 };

    AudioFilePlayerNode filePlayer_;
    MasterBusNode       master_;
    Transport           transport_;

    std::atomic<double> sampleRate_ { 0.0 };

    juce::String loadedClipName_;
    double       loadedClipSeconds_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace looper::engine
