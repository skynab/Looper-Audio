#pragma once

#include <atomic>
#include <cstdint>

#include <juce_audio_devices/juce_audio_devices.h>

#include "rt/SpscRingBuffer.h"

#include "engine/AudioGraph.h"
#include "engine/EngineCommand.h"
#include "engine/MasterBusNode.h"
#include "engine/OscillatorNode.h"
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
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }

    /** Post a command to the audio thread. Non-blocking; drops if the queue is full. */
    void postCommand(const EngineCommand& command) noexcept { commandQueue_.push(command); }

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

private:
    void drainCommandQueue() noexcept;

    juce::AudioDeviceManager          deviceManager_;
    rt::SpscRingBuffer<EngineCommand> commandQueue_ { 1024 };

    AudioGraph      graph_;
    OscillatorNode* oscillator_ = nullptr; // owned by graph_
    MasterBusNode*  master_     = nullptr; // owned by graph_
    Transport       transport_;

    std::atomic<double> sampleRate_ { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace looper::engine
