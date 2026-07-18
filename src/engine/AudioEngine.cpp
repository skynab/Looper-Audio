#include "engine/AudioEngine.h"

#include "rt/RealtimeGuard.h"

#include <limits>
#include <memory>

namespace looper::engine
{
AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();

    auto synth = std::make_unique<SynthInstrumentNode>();
    synth_ = synth.get();
    graph_.addSource(std::move(synth));

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

    // Route every available MIDI input into the collector.
    for (const auto& input : juce::MidiInput::getAvailableDevices())
    {
        deviceManager_.setMidiInputDeviceEnabled(input.identifier, true);
        deviceManager_.addMidiInputDeviceCallback(input.identifier, this);
    }
}

AudioEngine::~AudioEngine()
{
    for (const auto& input : juce::MidiInput::getAvailableDevices())
        deviceManager_.removeMidiInputDeviceCallback(input.identifier, this);

    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    // Called on the MIDI thread; the collector timestamps and hands off to audio.
    midiCollector_.addMessageToQueue(message);
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

void AudioEngine::setPattern(const Pattern& pattern)
{
    sequencer_.submitPattern(new Pattern(pattern));
}

void AudioEngine::pump() noexcept
{
    if (filePlayer_ != nullptr)
        filePlayer_->collectRetiredClips();

    sequencer_.collectRetired();
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

    // Gather this block's MIDI: external inputs (via the collector) merged with
    // the on-screen keyboard (via the shared keyboard state).
    incomingMidi_.clear();
    midiCollector_.removeNextBlockOfMessages(incomingMidi_, numSamples);
    keyboardState_.processNextMidiBuffer(incomingMidi_, 0, numSamples, true);

    ProcessContext context;
    context.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    context.numSamples = numSamples;
    context.transport  = transport_.snapshot();

    // Sequenced notes are added to the same buffer, so the synth plays them
    // alongside anything live.
    sequencer_.renderBlock(incomingMidi_, context);

    graph_.process(output, incomingMidi_, context);

    transport_.advance(numSamples);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int    blockSize  = device->getCurrentBufferSizeSamples();

    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    midiCollector_.reset(sampleRate);
    incomingMidi_.ensureSize(2048); // preallocate so the audio thread doesn't grow it
    transport_.prepare(sampleRate);
    graph_.prepare(sampleRate, blockSize);
}

void AudioEngine::audioDeviceStopped()
{
    graph_.release();
}

} // namespace looper::engine
