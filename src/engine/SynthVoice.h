#pragma once

#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/MidiNote.h"
#include "engine/StateVariableFilter.h"

namespace looper::engine
{
/** The sound every SynthVoice can play. */
struct SynthSound final : juce::SynthesiserSound
{
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

/**
    A single polyphonic voice: an oscillator (selectable waveform) shaped by an
    ADSR envelope, optionally through its own state-variable filter — the
    per-track timbre a SynthInstrumentNode's voices all share (see
    SynthInstrumentNode::refreshVoiceSettings, which pushes the owning track's
    model::SynthSettings into every voice once per block, the same
    "refreshed once per block" convention FilterEffect uses for the master
    filter). juce::Synthesiser owns a pool of these and handles note
    allocation, voice stealing, and sample-accurate MIDI dispatch.
*/
class SynthVoice final : public juce::SynthesiserVoice
{
public:
    enum class Waveform { Sine, Saw, Square, Triangle };

    void setADSR(const juce::ADSR::Parameters& params) { adsr_.setParameters(params); }

    /** Pushes this track's current timbre into the voice. Cheap (a handful of
        field copies, no allocation) — called every block regardless of
        whether anything changed, same as FilterEffect::process. */
    void applySettings(Waveform waveform, const juce::ADSR::Parameters& adsrParams,
                       bool filterEnabled, int filterMode, float filterCutoff, float filterResonance,
                       float outputGain) noexcept
    {
        waveform_      = waveform;
        adsr_.setParameters(adsrParams);
        filterEnabled_ = filterEnabled;
        filter_.setMode((StateVariableFilter::Mode) filterMode);
        filter_.setCutoff(filterCutoff);
        filter_.setResonance(filterResonance);
        outputGain_    = outputGain;
    }

    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SynthSound*>(sound) != nullptr;
    }

    void startNote(int midiNote, float velocity, juce::SynthesiserSound*, int /*pitchWheel*/) override
    {
        phase_     = 0.0;
        level_     = velocity;
        frequency_ = midiNoteToHertz(midiNote);
        adsr_.noteOn();
    }

    void stopNote(float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr_.noteOff();
        }
        else
        {
            adsr_.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void setCurrentPlaybackSampleRate(double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate(newRate);
        if (newRate > 0.0)
        {
            adsr_.setSampleRate(newRate);
            filter_.prepare(newRate);
        }
    }

    void renderNextBlock(juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr_.isActive())
            return;

        const double increment = juce::MathConstants<double>::twoPi * frequency_ / getSampleRate();

        for (int i = 0; i < numSamples; ++i)
        {
            const float env = adsr_.getNextSample();
            float       sample = renderWaveform() * env * level_ * 0.3f * outputGain_;
            if (filterEnabled_)
                sample = filter_.processSample(sample);

            phase_ += increment;
            if (phase_ >= juce::MathConstants<double>::twoPi)
                phase_ -= juce::MathConstants<double>::twoPi;

            for (int ch = output.getNumChannels(); --ch >= 0;)
                output.addSample(ch, startSample + i, sample);

            if (! adsr_.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    /** Naive (non-band-limited) waveform generation from the current phase —
        matches this engine's existing phase-1 style (see OscillatorNode's
        plain std::sin); anti-aliasing can follow later without changing this
        voice's shape. */
    float renderWaveform() const noexcept
    {
        const double norm = phase_ / juce::MathConstants<double>::twoPi; // 0..1

        switch (waveform_)
        {
            case Waveform::Sine:     return (float) std::sin(phase_);
            case Waveform::Saw:      return (float) (2.0 * norm - 1.0);
            case Waveform::Square:   return norm < 0.5 ? 1.0f : -1.0f;
            case Waveform::Triangle: return (float) (4.0 * std::abs(norm - 0.5) - 1.0);
        }
        return 0.0f;
    }

    juce::ADSR adsr_;
    double     phase_     = 0.0;
    double     frequency_ = 440.0;
    float      level_     = 0.0f;

    Waveform             waveform_      = Waveform::Sine;
    bool                 filterEnabled_ = false;
    StateVariableFilter  filter_;
    float                outputGain_    = 1.0f;
};

} // namespace looper::engine
