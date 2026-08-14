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
}

AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();

    // Started once and left running for the engine's lifetime. It idles when
    // nothing is recording, and spinning a thread up at the instant the user
    // hits record is exactly the wrong moment to be doing it.
    recordWriterThread_.startThread(juce::Thread::Priority::normal);

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
        slots->push_back({ decoded, spec.startBeats, spec.lengthBeats,
                           juce::Decibels::decibelsToGain(spec.gainDb) });
    }

    auto& track = tracks_[(size_t) index];
    track.audioPlayer.collectRetiredClips();
    track.audioPlayer.submitClips(slots);
    return allOk;
}

void AudioEngine::setTrackInstrument(int index, TrackInstrument instrument)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].instrument.store(instrument, std::memory_order_relaxed);
}

void AudioEngine::setTrackGuitarSettings(int index, const model::GuitarSettings& settings)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& guitar = tracks_[(size_t) index].guitar;
    guitar.setDecaySeconds(settings.decaySeconds);
    guitar.setBrightness(settings.brightness);
    guitar.setPickPosition(settings.pickPosition);
    guitar.setPickHardness(settings.pickHardness);
    guitar.setMuteOnNoteOff(settings.muteOnNoteOff);
    guitar.setPickupResonanceHz(settings.pickupResonanceHz);
    guitar.setPickupQ(settings.pickupQ);
}

void AudioEngine::setTrackGuitarTuning(int index, const std::array<int, kNumGuitarStrings>& tuning)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    for (int s = 0; s < kNumGuitarStrings; ++s)
        tracks_[(size_t) index].guitar.setOpenNote(s, tuning[(size_t) s]);
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

bool AudioEngine::beginRecording(const juce::File& destination)
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

    // Opening the file is part of arming: a take that was never going to be
    // written should fail before the user plays it, not after.
    return recorder_.arm(destination, recordWriterThread_, leadIn);
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

void AudioEngine::setTrackPan(int index, float pan)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].pan.store(juce::jlimit(-1.0f, 1.0f, pan), std::memory_order_relaxed);
}

void AudioEngine::setTrackSendLevel(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].sendLevel.store(level, std::memory_order_relaxed);
}

void AudioEngine::setTrackSessionSlots(int index, const std::vector<SessionSlotData>& slots)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& track = tracks_[(size_t) index];
    track.session.collectRetired();
    track.session.submitSlots(new SessionPlayer::SlotList(slots));
}

void AudioEngine::launchSessionSlot(int index, int sceneIndex)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].session.requestLaunch(sceneIndex);
}

void AudioEngine::stopSessionSlot(int index)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].session.requestStop();
}

void AudioEngine::launchScene(int sceneIndex)
{
    // Every track is told something, including the ones with nothing in this
    // scene: a scene says what the whole grid should be playing, so a track
    // with an empty slot falls silent rather than keeping its previous clip.
    for (auto& track : tracks_)
        track.session.requestLaunch(sceneIndex);
}

void AudioEngine::stopAllSessionSlots()
{
    for (auto& track : tracks_)
        track.session.requestStop();
}

void AudioEngine::setTrackAutomation(int index, const TrackAutomation& curves)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& track = tracks_[(size_t) index];
    track.collectRetiredAutomation();
    track.setAutomation(new TrackAutomation(curves));
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

void AudioEngine::setTrackSynthFilterEnvAmount(int index, float hz)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvAmount(hz);
}

void AudioEngine::setTrackSynthFilterEnvAttackMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvAttackMs(ms);
}

void AudioEngine::setTrackSynthFilterEnvDecayMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvDecayMs(ms);
}

void AudioEngine::setTrackSynthFilterEnvSustain(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvSustain(level);
}

void AudioEngine::setTrackSynthFilterEnvReleaseMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvReleaseMs(ms);
}

void AudioEngine::setTrackSynthSubOscEnabled(int index, bool enabled)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setSubOscEnabled(enabled);
}

void AudioEngine::setTrackSynthSubOscLevel(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setSubOscLevel(level);
}

void AudioEngine::setTrackSynthUnisonVoices(int index, int voices)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setUnisonVoices(voices);
}

void AudioEngine::setTrackSynthUnisonDetuneCents(int index, float cents)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setUnisonDetuneCents(cents);
}

void AudioEngine::rebuildTrackEffectChain(int index)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    const double rateForPlugins = sampleRate_.load(std::memory_order_relaxed);

    auto chain = std::make_unique<EffectChain>();
    for (const auto& spec : chainStructure_[(size_t) index])
    {
        switch (spec.kind)
        {
            case EffectNodeKind::Filter: chain->add(std::make_unique<FilterNode>()); break;
            case EffectNodeKind::Delay:  chain->add(std::make_unique<DelayNode>());  break;
            case EffectNodeKind::Reverb: chain->add(std::make_unique<ReverbNode>()); break;
            case EffectNodeKind::Drive:  chain->add(std::make_unique<DriveNode>());  break;
            case EffectNodeKind::Compressor: chain->add(std::make_unique<CompressorNode>()); break;
            case EffectNodeKind::Tremolo:    chain->add(std::make_unique<TremoloNode>());    break;
            case EffectNodeKind::Chorus:     chain->add(std::make_unique<ChorusNode>());     break;
            case EffectNodeKind::Wobble:     chain->add(std::make_unique<WobbleNode>());     break;
            case EffectNodeKind::Gate:       chain->add(std::make_unique<GateNode>());       break;
            case EffectNodeKind::Eq:         chain->add(std::make_unique<EqNode>());         break;

            case EffectNodeKind::Plugin:
            {
                // Instantiated here, on the message thread: loading a binary
                // and running third-party initialisation must never happen
                // under the audio thread. A plugin this machine doesn't have
                // simply leaves a gap in the chain rather than failing the
                // load — the document still remembers which one it wanted.
                std::string error;
                auto instance = pluginHost_.createInstance(spec.pluginFormat, spec.pluginIdentifier,
                                                           rateForPlugins > 0.0 ? rateForPlugins : 48000.0,
                                                           currentBlockSize_, &error);
                if (instance == nullptr)
                {
                    DBG("plugin unavailable: " << spec.pluginIdentifier.c_str() << " (" << error.c_str() << ")");
                    break;
                }

                auto node = std::make_unique<PluginNode>(std::move(instance));
                node->restoreState(spec.pluginState);
                chain->add(std::move(node));
                break;
            }
        }
    }

    // Prepared here, on the message thread, where allocating a delay line is
    // allowed. A rate of zero means the device hasn't started yet; the rebuild
    // in audioDeviceAboutToStart covers that case.
    const double rate = sampleRate_.load(std::memory_order_relaxed);
    if (rate > 0.0)
        chain->prepare(rate, currentBlockSize_);

    auto& track = tracks_[(size_t) index];
    track.collectRetiredEffectChain();

    submittedChain_[(size_t) index] = chain.get();
    track.setEffectChain(chain.release());
}

bool AudioEngine::setTrackEffectChain(int index, const std::vector<EffectSlotSpec>& slots)
{
    if (index < 0 || index >= kMaxTracks)
        return false;

    // Rebuilding resets every tail in the chain — and reinstantiates every
    // plugin — so only do it when the shape actually changed. A changed
    // preset or parameter is not a shape change.
    const auto& existing = chainStructure_[(size_t) index];
    bool sameShape = existing.size() == slots.size() && submittedChain_[(size_t) index] != nullptr;
    for (size_t i = 0; sameShape && i < slots.size(); ++i)
        sameShape = existing[i].sameShapeAs(slots[i]);

    if (sameShape)
        return false;

    chainStructure_[(size_t) index] = slots;
    rebuildTrackEffectChain(index);
    return true;
}

void AudioEngine::setTrackEffectSlotParams(int index, int slotIndex, const EffectSlotParams& params)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return;

    if (auto* chain = submittedChain_[(size_t) index])
        chain->applyParams((size_t) slotIndex, params);
}

PluginNode* AudioEngine::trackPluginNode(int index, int slotIndex)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return nullptr;

    auto* chain = submittedChain_[(size_t) index];
    return chain != nullptr ? dynamic_cast<PluginNode*>(chain->nodeAt((size_t) slotIndex)) : nullptr;
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
        track.collectRetiredAutomation();
        track.session.collectRetired();
        track.collectRetiredEffectChain();
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

    recorder_.process(inputChannelData, numInputChannels, numSamples,
                      context.transport.playing, context.transport.playheadSamples);

    processBlock(output, incomingMidi_, context);

    // Dry input monitoring, mixed in *after* the master bus for the same
    // reasons the metronome is: it stays out of the meter, out of the master
    // effects, and — because renderOffline only ever calls processBlock — it
    // can never end up in an exported file.
    mixInputMonitoring(output, inputChannelData, numInputChannels, numSamples);

    // After the master bus deliberately: the click bypasses the master
    // effects and gain, stays off the meter, and can never be exported (it
    // sits outside processBlock, which is what renderOffline renders).
    // `force` sounds it through a count-in even when it's otherwise off.
    metronome_.process(output, context, isCountingIn());
}

void AudioEngine::mixInputMonitoring(juce::AudioBuffer<float>& output,
                                     const float* const* inputChannelData,
                                     int numInputChannels, int numSamples) noexcept
{
    const float target = inputMonitoring_.load(std::memory_order_relaxed)
                             ? juce::jlimit(0.0f, 2.0f, inputMonitorGain_.load(std::memory_order_relaxed))
                             : 0.0f;

    // Nothing to do, and nothing to ramp down from.
    if (target <= 0.0f && monitorGainRamp_ <= 0.0f)
        return;

    if (inputChannelData == nullptr || numInputChannels <= 0 || numSamples <= 0)
    {
        monitorGainRamp_ = target;
        return;
    }

    // Ramped across the block rather than switched: toggling monitoring
    // mid-take would otherwise put a step in the output, which through
    // headphones at tracking level is unpleasant.
    const float startGain = monitorGainRamp_;
    const float step      = (target - startGain) / (float) numSamples;

    const int channels = output.getNumChannels();
    for (int ch = 0; ch < channels; ++ch)
    {
        // A mono source is heard on both sides rather than only the left.
        const int   source = juce::jmin(ch, numInputChannels - 1);
        const auto* input  = inputChannelData[source];
        if (input == nullptr)
            continue;

        auto* out = output.getWritePointer(ch);
        float gain = startGain;

        for (int n = 0; n < numSamples; ++n)
        {
            out[n] += input[n] * gain;
            gain += step;
        }
    }

    monitorGainRamp_ = target;
}

void AudioEngine::processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi,
                               const ProcessContext& context) noexcept
{
    const int numSamples = context.numSamples;

    bool anySolo = false;
    for (auto& track : tracks_)
        anySolo |= track.solo.load(std::memory_order_relaxed);

    sendBus_.setSize(2, numSamples, false, false, true);
    sendBus_.clear();

    // Launch quantization is expressed in beats and converted here, once per
    // block, from the tempo actually in force.
    const double samplesPerBeat       = context.transport.bpm > 0.0
                                          ? context.sampleRate * 60.0 / context.transport.bpm : 0.0;
    const double launchQuantumSamples = samplesPerBeat * launchQuantumBeats_.load(std::memory_order_relaxed);

    const int armed = armedTrack_.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (tracks_[(size_t) i].active.load(std::memory_order_relaxed))
            tracks_[(size_t) i].render(output, sendBus_, midi, context, i == armed, anySolo,
                                       launchQuantumSamples);
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
    filePlayer_.process(output, midi, context);
    masterFilter_.process(output);
    masterDelay_.process(output);
    masterReverb_.process(output);
    masterEq_.process(output);
    // The mastering rack sits between the master EQ and the output node, so
    // its limiter is the last thing to touch level before the meter reads it
    // — a ceiling that something after it could exceed wouldn't be one.
    mastering_.process(output);
    master_.process(output, midi, context);

    transport_.advance(numSamples);
}

juce::AudioBuffer<float> AudioEngine::renderOffline(double startBeats, double lengthBeats,
                                                    double renderSampleRate, int blockSize)
{
    const double deviceRate = sampleRate_.load(std::memory_order_relaxed);
    const double sampleRate = renderSampleRate > 0.0 ? renderSampleRate : deviceRate;
    const double bpm        = transport_.tempoMap().tempo();

    juce::AudioBuffer<float> output(2, 0);
    if (deviceRate <= 0.0 || sampleRate <= 0.0 || bpm <= 0.0 || lengthBeats <= 0.0)
        return output;

    blockSize = juce::jmax(1, blockSize);

    const double  samplesPerBeat = sampleRate * 60.0 / bpm;
    const int64_t startSample    = (int64_t) std::llround(startBeats * samplesPerBeat);
    const int     totalSamples   = (int) std::llround(lengthBeats * samplesPerBeat);
    if (totalSamples <= 0)
        return output;

    // Suspending the device is what makes this safe rather than a race: the
    // mixer's state has exactly one writer by design, and the device thread
    // is otherwise inside processBlock at the same time as this loop.
    deviceManager_.removeAudioCallback(this);

    // Only the device callback drains this, so with the callback removed
    // anything still queued would sit unapplied for the whole render — a
    // tempo change made just before hitting Bounce would silently not be in
    // the file. Drained here, while this thread is the only one running.
    drainCommandQueue();

    const bool    wasPlaying  = transport_.isPlaying();
    const bool    wasLooping  = transport_.isLooping();
    const int64_t wasPlayhead = transport_.playhead();

    // Fresh state, so a render is reproducible instead of inheriting whatever
    // tails happened to be ringing when the user hit Export - and prepared for
    // the *render* rate, which is what makes exporting at 44.1 from a 48k
    // device a native render rather than a resample.
    prepareAll(sampleRate, blockSize);

    // Looping off for the duration: a loop region set for auditioning would
    // otherwise wrap the playhead mid-export and repeat a section.
    transport_.setLooping(false);
    transport_.seek(startSample);
    transport_.setPlaying(true);

    output.setSize(2, totalSamples);
    output.clear();

    juce::MidiBuffer noLiveMidi;

    for (int pos = 0; pos < totalSamples; pos += blockSize)
    {
        const int n = juce::jmin(blockSize, totalSamples - pos);

        ProcessContext context;
        context.sampleRate = sampleRate;
        context.numSamples = n;
        context.transport  = transport_.snapshot();

        juce::AudioBuffer<float> view(output.getArrayOfWritePointers(), 2, pos, n);
        noLiveMidi.clear();
        processBlock(view, noLiveMidi, context);
    }

    // Back to the *device* rate, not the render rate: everything above was
    // prepared for the export, and leaving it that way would have live
    // playback running at the wrong rate the moment the callback returns.
    // Also again on the way out at all, so playback doesn't start up holding
    // the render's delay and reverb tails.
    prepareAll(deviceRate, blockSize);

    // After that prepare, because wasPlayhead is a sample count at the device
    // rate and prepareAll is what puts the transport's tempo map back on that
    // rate. Restoring it first would place the playhead using the render's
    // clock.
    transport_.setPlaying(wasPlaying);
    transport_.setLooping(wasLooping);
    transport_.seek(wasPlayhead);

    deviceManager_.addAudioCallback(this);
    return output;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int    blockSize  = device->getCurrentBufferSizeSamples();

    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    midiCollector_.reset(sampleRate);
    incomingMidi_.ensureSize(2048);

    prepareAll(sampleRate, blockSize);
}

void AudioEngine::prepareAll(double sampleRate, int blockSize)
{
    transport_.prepare(sampleRate);

    currentBlockSize_ = blockSize;

    for (auto& track : tracks_)
        track.prepare(sampleRate, blockSize);

    // A chain must be prepared for the rate it will actually run at, and one
    // submitted before the device started (or before a rate change) was
    // prepared for the wrong rate, or not at all. Rebuilding here guarantees
    // it; device starts are rare enough that the cost doesn't matter.
    for (int i = 0; i < kMaxTracks; ++i)
        rebuildTrackEffectChain(i);

    filePlayer_.prepare(sampleRate, blockSize);
    masterFilter_.prepare(sampleRate, blockSize);
    masterDelay_.prepare(sampleRate, blockSize);
    masterReverb_.prepare(sampleRate, blockSize);
    masterEq_.prepare(sampleRate, blockSize);
    mastering_.prepare(sampleRate, blockSize);
    master_.prepare(sampleRate, blockSize);

    sendBus_.setSize(2, blockSize);
    sendBusReverb_.prepare(sampleRate, blockSize);
    sendBusReverb_.setEnabled(true); // always on internally; sendBusEnabled_ gates the mix-back
    sendBusReverb_.setMix(1.0f);     // a return bus is always fully wet

    sendBusDelay_.prepare(sampleRate, blockSize);
    sendBusDelay_.setEnabled(true); // same convention as sendBusReverb_ above
    sendBusDelay_.setMix(1.0f);     // a return bus is always fully wet

    recorder_.prepare(sampleRate, 2);
    metronome_.prepare(sampleRate);
}

juce::String AudioEngine::systemDefaultOutputName()
{
    auto* type = deviceManager_.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return {};

    // Without a rescan the list is whatever it was when the type was created,
    // so a device that has just been plugged in isn't in it yet — which is
    // precisely the moment this gets asked.
    type->scanForDevices();

    const auto names = type->getDeviceNames(false); // false = outputs
    const int  index = type->getDefaultDeviceIndex(false);

    return juce::isPositiveAndBelow(index, names.size()) ? names[index] : juce::String {};
}

juce::String AudioEngine::followSystemDefaultOutput()
{
    const auto defaultName = systemDefaultOutputName();
    if (defaultName.isEmpty())
        return {};

    if (auto* current = deviceManager_.getCurrentAudioDevice())
        if (current->getName() == defaultName)
            return {}; // already on it

    auto setup = deviceManager_.getAudioDeviceSetup();
    setup.outputDeviceName         = defaultName;
    setup.useDefaultOutputChannels = true;

    // treatAsChosenDevice = false: this is the app following the system, not
    // the user picking something, so it shouldn't be written back as a
    // remembered preference.
    const auto error = deviceManager_.setAudioDeviceSetup(setup, false);
    if (error.isNotEmpty())
        return {};

    return defaultName;
}

void AudioEngine::audioDeviceStopped()
{
    filePlayer_.release();
}

} // namespace looper::engine
