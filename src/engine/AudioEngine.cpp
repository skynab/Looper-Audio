#include "engine/AudioEngine.h"

#include "rt/RealtimeGuard.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace looper::engine
{
AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();

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

    filePlayer_.collectRetiredClips();
    filePlayer_.submitClip(clip.release());
    return true;
}

void AudioEngine::setActiveTrackCount(int count)
{
    const int clamped = juce::jlimit(0, kMaxTracks, count);
    for (int i = 0; i < kMaxTracks; ++i)
        tracks_[(size_t) i].active.store(i < clamped, std::memory_order_relaxed);
}

void AudioEngine::setTrackPattern(int index, const Pattern& pattern)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].sequencer.submitPattern(new Pattern(pattern));
}

void AudioEngine::setTrackMuted(int index, bool muted)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].muted.store(muted, std::memory_order_relaxed);
}

void AudioEngine::setTrackSolo(int index, bool solo)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].solo.store(solo, std::memory_order_relaxed);
}

void AudioEngine::setTrackGainDb(int index, float gainDb)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].gainDb.store(gainDb, std::memory_order_relaxed);
}

void AudioEngine::setArmedTrack(int index)
{
    armedTrack_.store(juce::jlimit(0, kMaxTracks - 1, index), std::memory_order_relaxed);
}

void AudioEngine::pump() noexcept
{
    for (auto& track : tracks_)
        track.sequencer.collectRetired();

    filePlayer_.collectRetiredClips();
}

void AudioEngine::drainCommandQueue() noexcept
{
    EngineCommand command;
    while (commandQueue_.pop(command))
    {
        switch (command.type)
        {
            case EngineCommand::Type::SetPlaying:      transport_.setPlaying(command.a != 0.0); break;
            case EngineCommand::Type::SetLooping:      transport_.setLooping(command.a != 0.0); break;
            case EngineCommand::Type::Seek:            transport_.seek((int64_t) command.a); break;
            case EngineCommand::Type::SetTempo:        transport_.setTempo(command.a); break;
            case EngineCommand::Type::SetLoopRegion:   transport_.setLoopRegion((int64_t) command.a, (int64_t) command.b); break;
            case EngineCommand::Type::SetMasterGainDb: master_.setGainDb((float) command.a); break;
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
    output.clear();

    incomingMidi_.clear();
    midiCollector_.removeNextBlockOfMessages(incomingMidi_, numSamples);
    keyboardState_.processNextMidiBuffer(incomingMidi_, 0, numSamples, true);

    ProcessContext context;
    context.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    context.numSamples = numSamples;
    context.transport  = transport_.snapshot();

    bool anySolo = false;
    for (auto& track : tracks_)
        anySolo |= track.solo.load(std::memory_order_relaxed);

    const int armed = armedTrack_.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (tracks_[(size_t) i].active.load(std::memory_order_relaxed))
            tracks_[(size_t) i].render(output, incomingMidi_, context, i == armed, anySolo);
    }

    // The file player and master ignore the MIDI buffer.
    filePlayer_.process(output, incomingMidi_, context);
    masterFilter_.process(output);
    masterDelay_.process(output);
    masterReverb_.process(output);
    master_.process(output, incomingMidi_, context);

    transport_.advance(numSamples);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int    blockSize  = device->getCurrentBufferSizeSamples();

    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    midiCollector_.reset(sampleRate);
    incomingMidi_.ensureSize(2048);
    transport_.prepare(sampleRate);

    for (auto& track : tracks_)
        track.prepare(sampleRate, blockSize);

    filePlayer_.prepare(sampleRate, blockSize);
    masterFilter_.prepare(sampleRate, blockSize);
    masterDelay_.prepare(sampleRate, blockSize);
    masterReverb_.prepare(sampleRate, blockSize);
    master_.prepare(sampleRate, blockSize);
}

void AudioEngine::audioDeviceStopped()
{
    filePlayer_.release();
}

} // namespace looper::engine
