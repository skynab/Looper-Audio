#include "engine/AudioEngine.h"

#include "rt/RealtimeGuard.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace looper::engine
{
namespace
{
    // Recorder buffer capacity: a v1 limit (RAM-only, no disk streaming yet).
    constexpr double kMaxRecordSeconds = 180.0;
}

AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();

    // Request up to 2 input channels too (for recording); JUCE falls back to
    // however many the device actually has, including zero.
    deviceManager_.initialiseWithDefaultDevices(2, 2);
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

std::unique_ptr<ClipData> AudioEngine::decodeAudioFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr)
        return nullptr;

    const int length = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                     (juce::int64) std::numeric_limits<int>::max());
    if (length <= 0)
        return nullptr;

    const int numChannels = juce::jmax(1, (int) reader->numChannels);

    auto clip = std::make_unique<ClipData>();
    clip->audio.setSize(numChannels, length);
    reader->read(&clip->audio, 0, length, 0, true, true);
    clip->sourceSampleRate = reader->sampleRate;
    clip->numChannels      = numChannels;
    clip->lengthSamples    = length;
    return clip;
}

double AudioEngine::probeDurationSeconds(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return 0.0;
    return (double) reader->lengthInSamples / reader->sampleRate;
}

bool AudioEngine::loadAudioFile(const juce::File& file)
{
    auto clip = decodeAudioFile(file);
    if (clip == nullptr)
        return false;

    loadedClipName_    = file.getFileName();
    loadedClipSeconds_ = clip->sourceSampleRate > 0.0 ? (double) clip->lengthSamples / clip->sourceSampleRate : 0.0;

    filePlayer_.collectRetiredClips();
    filePlayer_.submitSingleClip(clip.release());
    return true;
}

std::shared_ptr<ClipData> AudioEngine::decodeOrGetCached(const juce::File& file)
{
    const auto path = file.getFullPathName();
    auto       it   = audioDecodeCache_.find(path);
    if (it != audioDecodeCache_.end())
        return it->second;

    auto decoded = decodeAudioFile(file);
    if (decoded == nullptr)
        return nullptr;

    std::shared_ptr<ClipData> shared(decoded.release());
    audioDecodeCache_[path] = shared;
    return shared;
}

bool AudioEngine::setTrackAudioClips(int index, const std::vector<AudioClipSpec>& clips)
{
    if (index < 0 || index >= kMaxTracks)
        return false;

    auto* slots = new std::vector<AudioClipSlot>();
    slots->reserve(clips.size());
    bool allOk = true;

    for (const auto& spec : clips)
    {
        auto decoded = decodeOrGetCached(spec.file);
        if (decoded == nullptr)
        {
            allOk = false;
            continue; // skip this clip; the others still load
        }
        slots->push_back({ decoded, spec.startBeats, spec.lengthBeats });
    }

    auto& track = tracks_[(size_t) index];
    track.audioPlayer.collectRetiredClips();
    track.audioPlayer.submitClips(slots);
    return allOk;
}

void AudioEngine::setTrackIsDrum(int index, bool isDrum)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].isDrumTrack.store(isDrum, std::memory_order_relaxed);
}

void AudioEngine::setTrackDrumKit(int index, const std::vector<DrumPadSpec>& pads)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto* map = new DrumPadMap();
    map->reserve(pads.size());

    for (const auto& pad : pads)
    {
        std::shared_ptr<ClipData> clip;
        if (pad.file != juce::File{})
            clip = decodeOrGetCached(pad.file); // nullptr on failure — the pad just stays silent

        DrumPadAssignment assignment;
        assignment.noteNumber = pad.noteNumber;
        assignment.clipData   = std::move(clip);
        assignment.gain       = juce::Decibels::decibelsToGain(pad.gainDb);
        assignment.pan        = juce::jlimit(-1.0f, 1.0f, pad.pan);
        assignment.pitchRatio = std::pow(2.0f, pad.pitchSemitones / 12.0f);
        assignment.muted      = pad.muted;
        map->push_back(std::move(assignment));
    }

    auto& track = tracks_[(size_t) index];
    track.drumKit.collectRetired();
    track.drumKit.setPadMap(map);
}

bool AudioEngine::beginRecording()
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
        return false;

    // The count-in is expressed in samples here, on the message thread, from
    // the tempo in force when recording starts — the audio thread only ever
    // counts it down (see AudioRecorder::process).
    const auto&  tempoMap      = transport_.tempoMap();
    const double samplesPerBar = tempoMap.samplesPerBeat() * tempoMap.quartersPerBar();
    const auto   leadIn        = (int64_t) std::llround(samplesPerBar * (double) countInBars_);

    recorder_.arm(leadIn);
    return true;
}

void AudioEngine::setActiveTrackCount(int count)
{
    const int clamped = juce::jlimit(0, kMaxTracks, count);
    for (int i = 0; i < kMaxTracks; ++i)
        tracks_[(size_t) i].active.store(i < clamped, std::memory_order_relaxed);
}

void AudioEngine::setTrackClips(int index, const std::vector<ClipSlot>& clips)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].sequencer.submitClips(new std::vector<ClipSlot>(clips));
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

void AudioEngine::setTrackSendLevel(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].sendLevel.store(level, std::memory_order_relaxed);
}

void AudioEngine::setTrackSynthWaveform(int index, int waveform)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setWaveform(waveform);
}

void AudioEngine::setTrackSynthAttackMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setAttackMs(ms);
}

void AudioEngine::setTrackSynthDecayMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setDecayMs(ms);
}

void AudioEngine::setTrackSynthSustain(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setSustain(level);
}

void AudioEngine::setTrackSynthReleaseMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setReleaseMs(ms);
}

void AudioEngine::setTrackSynthFilterEnabled(int index, bool enabled)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnabled(enabled);
}

void AudioEngine::setTrackSynthFilterMode(int index, int mode)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterMode(mode);
}

void AudioEngine::setTrackSynthFilterCutoff(int index, float hz)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterCutoff(hz);
}

void AudioEngine::setTrackSynthFilterResonance(int index, float q)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterResonance(q);
}

void AudioEngine::setTrackSynthGainDb(int index, float db)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setGainDb(db);
}

void AudioEngine::setArmedTrack(int index)
{
    armedTrack_.store(juce::jlimit(0, kMaxTracks - 1, index), std::memory_order_relaxed);
}

void AudioEngine::pump() noexcept
{
    for (auto& track : tracks_)
    {
        track.sequencer.collectRetired();
        track.audioPlayer.collectRetiredClips();
        track.drumKit.collectRetired();
    }

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
            case EngineCommand::Type::SetTimeSignature:
                transport_.tempoMap().setTimeSignature((int) command.a, (int) command.b);
                break;
        }
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
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

    recorder_.process(inputChannelData, numInputChannels, numSamples, context.transport.playing);

    bool anySolo = false;
    for (auto& track : tracks_)
        anySolo |= track.solo.load(std::memory_order_relaxed);

    sendBus_.setSize(2, numSamples, false, false, true);
    sendBus_.clear();

    const int armed = armedTrack_.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (tracks_[(size_t) i].active.load(std::memory_order_relaxed))
            tracks_[(size_t) i].render(output, sendBus_, incomingMidi_, context, i == armed, anySolo);
    }

    if (sendBusEnabled_.load(std::memory_order_relaxed))
    {
        if (sendBusEffectType_.load(std::memory_order_relaxed) == 1) // 1 = delay, 0 = reverb
            sendBusDelay_.process(sendBus_);
        else
            sendBusReverb_.process(sendBus_);

        const float returnGain = sendReturnGain_.load(std::memory_order_relaxed);
        const int   channels   = juce::jmin(output.getNumChannels(), sendBus_.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
            output.addFrom(ch, 0, sendBus_, ch, 0, numSamples, returnGain);
    }

    // The file player and master ignore the MIDI buffer.
    filePlayer_.process(output, incomingMidi_, context);
    masterFilter_.process(output);
    masterDelay_.process(output);
    masterReverb_.process(output);
    master_.process(output, incomingMidi_, context);

    // After the master bus deliberately: the click bypasses the master
    // effects and gain, stays off the meter, and can never be exported (the
    // offline renderer has no metronome). `force` sounds it through a
    // count-in even when it's otherwise switched off.
    metronome_.process(output, context, isCountingIn());

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

    sendBus_.setSize(2, blockSize);
    sendBusReverb_.prepare(sampleRate, blockSize);
    sendBusReverb_.setEnabled(true); // always on internally; sendBusEnabled_ gates the mix-back
    sendBusReverb_.setMix(1.0f);     // a return bus is always fully wet

    sendBusDelay_.prepare(sampleRate, blockSize);
    sendBusDelay_.setEnabled(true); // same convention as sendBusReverb_ above
    sendBusDelay_.setMix(1.0f);     // a return bus is always fully wet

    recorder_.prepare(sampleRate, 2, kMaxRecordSeconds);
    metronome_.prepare(sampleRate);
}

void AudioEngine::audioDeviceStopped()
{
    filePlayer_.release();
}

} // namespace looper::engine
