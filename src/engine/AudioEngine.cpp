#include "engine/AudioEngine.h"

#include "rt/RealtimeGuard.h"

#include <limits>
#include <memory>

namespace looper::engine
{
AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();

    auto oscillator = std::make_unique<OscillatorNode>();
    oscillator_ = oscillator.get();
    graph_.addSource(std::move(oscillator));

    auto filePlayer = std::make_unique<AudioFilePlayerNode>();
    filePlayer_ = filePlayer.get();
    graph_.addSource(std::move(filePlayer));

    auto master = std::make_unique<MasterBusNode>();
    master_ = master.get();
    graph_.setMaster(std::move(master));

    deviceManager_.initialiseWithDefaultDevices(0, 2);
    deviceManager_.addAudioCallback(this);
}

AudioEngine::~AudioEngine()
{
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
}

bool AudioEngine::loadAudioFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr)
        return false;

    const int length = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                     (juce::int64) std::numeric_limits<int>::max());
    if (length <= 0)
        return false;

    const int numChannels = juce::jmax(1, (int) reader->numChannels);

    auto clip = std::make_unique<ClipData>();
    clip->audio.setSize(numChannels, length);
    reader->read(&clip->audio, 0, length, 0, true, true);
    clip->sourceSampleRate = reader->sampleRate;
    clip->numChannels      = numChannels;
    clip->lengthSamples    = length;

    loadedClipName_    = file.getFileName();
    loadedClipSeconds_ = reader->sampleRate > 0.0 ? (double) length / reader->sampleRate : 0.0;

    filePlayer_->collectRetiredClips();      // free any previous clip first
    filePlayer_->submitClip(clip.release()); // hand ownership to the audio thread
    return true;
}

void AudioEngine::pump() noexcept
{
    if (filePlayer_ != nullptr)
        filePlayer_->collectRetiredClips();
}

void AudioEngine::drainCommandQueue() noexcept
{
    EngineCommand command;
    while (commandQueue_.pop(command))
    {
        switch (command.type)
        {
            case EngineCommand::Type::SetPlaying:         transport_.setPlaying(command.a != 0.0); break;
            case EngineCommand::Type::SetLooping:         transport_.setLooping(command.a != 0.0); break;
            case EngineCommand::Type::Seek:               transport_.seek((int64_t) command.a); break;
            case EngineCommand::Type::SetTempo:           transport_.setTempo(command.a); break;
            case EngineCommand::Type::SetLoopRegion:      transport_.setLoopRegion((int64_t) command.a, (int64_t) command.b); break;
            case EngineCommand::Type::SetSourceEnabled:   oscillator_->setEnabled(command.a != 0.0); break;
            case EngineCommand::Type::SetSourceFrequency: oscillator_->setFrequency((float) command.a); break;
            case EngineCommand::Type::SetSourceGainDb:    oscillator_->setGainDb((float) command.a); break;
            case EngineCommand::Type::SetMasterGainDb:    master_->setGainDb((float) command.a); break;
        }
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* /*inputChannelData*/,
                                                   int /*numInputChannels*/,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& /*context*/)
{
    rt::markCurrentThreadAsAudioThread();
    drainCommandQueue();

    juce::AudioBuffer<float> output(outputChannelData, numOutputChannels, numSamples);

    ProcessContext context;
    context.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    context.numSamples = numSamples;
    context.transport  = transport_.snapshot();

    graph_.process(output, context);

    transport_.advance(numSamples);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int    blockSize  = device->getCurrentBufferSizeSamples();

    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    transport_.prepare(sampleRate);
    graph_.prepare(sampleRate, blockSize);
}

void AudioEngine::audioDeviceStopped()
{
    graph_.release();
}

} // namespace looper::engine
