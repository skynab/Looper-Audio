#pragma once

#include <atomic>

#include "engine/Node.h"
#include "engine/SynthVoice.h"

namespace looper::engine
{
/**
    A polyphonic instrument node driven by the per-block MIDI buffer. Wraps
    juce::Synthesiser, which dispatches note on/off from the MIDI at sample
    accuracy and renders the active voices additively into the output.

    Every voice shares one timbre — the track's model::SynthSettings, mirrored
    here as atomics settable from the message thread (see the setters below,
    same shape as AudioEngine's other per-track setters e.g. setTrackGainDb).
    refreshVoiceSettings() copies them into every voice once per block, the
    same "coefficients refreshed once per block" convention FilterEffect uses
    for the master filter — cheap enough not to need a dirty flag.
*/
class SynthInstrumentNode final : public Node
{
public:
    SynthInstrumentNode()
    {
        synth_.addSound(new SynthSound());

        const juce::ADSR::Parameters params { 0.005f, 0.12f, 0.7f, 0.25f };
        for (int i = 0; i < numVoices_; ++i)
        {
            auto* voice = new SynthVoice();
            voice->setADSR(params);
            synth_.addVoice(voice);
        }
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        synth_.setCurrentPlaybackSampleRate(sampleRate);
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const ProcessContext& /*context*/) override
    {
        refreshVoiceSettings();
        synth_.renderNextBlock(buffer, midi, 0, buffer.getNumSamples());
    }

    // ---- message thread (mirrors AudioEngine's other per-track setters) ----
    void setWaveform(int waveform)         { waveform_.store(waveform, std::memory_order_relaxed); }
    void setAttackMs(float ms)             { attackMs_.store(ms, std::memory_order_relaxed); }
    void setDecayMs(float ms)              { decayMs_.store(ms, std::memory_order_relaxed); }
    void setSustain(float level)           { sustain_.store(level, std::memory_order_relaxed); }
    void setReleaseMs(float ms)            { releaseMs_.store(ms, std::memory_order_relaxed); }
    void setFilterEnabled(bool enabled)    { filterEnabled_.store(enabled, std::memory_order_relaxed); }
    void setFilterMode(int mode)           { filterMode_.store(mode, std::memory_order_relaxed); }
    void setFilterCutoff(float hz)         { filterCutoff_.store(hz, std::memory_order_relaxed); }
    void setFilterResonance(float q)       { filterResonance_.store(q, std::memory_order_relaxed); }
    void setGainDb(float db)               { gainDb_.store(db, std::memory_order_relaxed); }

private:
    void refreshVoiceSettings()
    {
        const auto waveform = (SynthVoice::Waveform) waveform_.load(std::memory_order_relaxed);
        const juce::ADSR::Parameters adsr { attackMs_.load(std::memory_order_relaxed) / 1000.0f,
                                            decayMs_.load(std::memory_order_relaxed) / 1000.0f,
                                            sustain_.load(std::memory_order_relaxed),
                                            releaseMs_.load(std::memory_order_relaxed) / 1000.0f };
        const bool  filterEnabled   = filterEnabled_.load(std::memory_order_relaxed);
        const int   filterMode      = filterMode_.load(std::memory_order_relaxed);
        const float filterCutoff    = filterCutoff_.load(std::memory_order_relaxed);
        const float filterResonance = filterResonance_.load(std::memory_order_relaxed);
        const float outputGain      = juce::Decibels::decibelsToGain(gainDb_.load(std::memory_order_relaxed));

        for (int i = 0; i < synth_.getNumVoices(); ++i)
            if (auto* voice = dynamic_cast<SynthVoice*>(synth_.getVoice(i)))
                voice->applySettings(waveform, adsr, filterEnabled, filterMode, filterCutoff, filterResonance, outputGain);
    }

    static constexpr int numVoices_ = 16;
    juce::Synthesiser    synth_;

    std::atomic<int>   waveform_ { 0 }; // SynthVoice::Waveform, stored as int for atomic use
    std::atomic<float> attackMs_ { 5.0f };
    std::atomic<float> decayMs_  { 120.0f };
    std::atomic<float> sustain_  { 0.7f };
    std::atomic<float> releaseMs_ { 250.0f };
    std::atomic<bool>  filterEnabled_ { false };
    std::atomic<int>   filterMode_ { 0 };
    std::atomic<float> filterCutoff_ { 1000.0f };
    std::atomic<float> filterResonance_ { 0.707f };
    std::atomic<float> gainDb_ { 0.0f };
};

} // namespace looper::engine
