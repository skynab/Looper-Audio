#pragma once

#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/MidiNote.h"

namespace looper::engine
{
/** The sound every SineVoice can play. */
struct SineSound final : juce::SynthesiserSound
{
    bool appliesToNote(int) override    { return true; }
    bool appliesToChannel(int) override { return true; }
};

/**
    A single polyphonic voice: a sine oscillator shaped by an ADSR envelope.
    juce::Synthesiser owns a pool of these and handles note allocation, voice
    stealing, and sample-accurate MIDI dispatch.
*/
class SineVoice final : public juce::SynthesiserVoice
{
public:
    void setADSR(const juce::ADSR::Parameters& params) { adsr_.setParameters(params); }

    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SineSound*>(sound) != nullptr;
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
            adsr_.setSampleRate(newRate);
    }

    void renderNextBlock(juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr_.isActive())
            return;

        const double increment = juce::MathConstants<double>::twoPi * frequency_ / getSampleRate();

        for (int i = 0; i < numSamples; ++i)
        {
            const float env    = adsr_.getNextSample();
            const float sample = (float) std::sin(phase_) * env * level_ * 0.3f;

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
    juce::ADSR adsr_;
    double     phase_     = 0.0;
    double     frequency_ = 440.0;
    float      level_     = 0.0f;
};

} // namespace looper::engine
