#pragma once

#include <atomic>
#include <cstdint>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "rt/SpscRingBuffer.h"

#include "engine/AudioFilePlayerNode.h"
#include "engine/AudioGraph.h"
#include "engine/EngineCommand.h"
#include "engine/MasterBusNode.h"
#include "engine/OscillatorNode.h"
#include "engine/SynthInstrumentNode.h"
#include "engine/Transport.h"

namespace looper::engine
{
/**
    The headless audio engine. It owns the audio device and is the device
    callback; the UI never touches it except by:
      - posting commands (lock-free, message thread → audio thread), and
      - reading published atomics (playhead, meters — audio thread → UI).

    Deliberately not a juce::Component, so the whole engine can be driven and
    tested without any UI.
*/
class AudioEngine final : public juce::AudioIODeviceCallback,
                          public juce::MidiInputCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }

    /** Shared with the on-screen keyboard so UI notes reach the synth. */
    juce::MidiKeyboardState& keyboardState() noexcept { return keyboardState_; }

    /** Post a command to the audio thread. Non-blocking; drops if the queue is full. */
    void postCommand(const EngineCommand& command) noexcept { commandQueue_.push(command); }

    /** Decode an audio file into RAM and hand it to the file-player node. Message thread. */
    bool loadAudioFile(const juce::File& file);

    /** Housekeeping to run periodically on the message thread (frees retired clips). */
    void pump() noexcept;

    juce::String loadedClipName() const           { return loadedClipName_; }
    double       loadedClipSeconds() const noexcept { return loadedClipSeconds_; }

    // ---- lock-free UI readouts ----
    bool    isPlaying() const noexcept       { return transport_.playingForUI(); }
    int64_t playheadSamples() const noexcept { return transport_.playheadForUI(); }
    double  sampleRate() const noexcept      { return sampleRate_.load(std::memory_order_relaxed); }
    float   masterPeak(int channel) const noexcept { return master_ != nullptr ? master_->peak(channel) : 0.0f; }

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

    AudioGraph           graph_;
    SynthInstrumentNode* synth_      = nullptr; // owned by graph_
    OscillatorNode*      oscillator_ = nullptr; // owned by graph_
    AudioFilePlayerNode* filePlayer_ = nullptr; // owned by graph_
    MasterBusNode*       master_     = nullptr; // owned by graph_
    Transport            transport_;

    std::atomic<double> sampleRate_ { 0.0 };

    juce::String loadedClipName_;
    double       loadedClipSeconds_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace looper::engine
