#pragma once

#include "engine/Node.h"
#include "engine/SineVoice.h"

namespace looper::engine
{
/**
    A polyphonic instrument node driven by the per-block MIDI buffer. Wraps
    juce::Synthesiser, which dispatches note on/off from the MIDI at sample
    accuracy and renders the active voices additively into the output.
*/
class SynthInstrumentNode final : public Node
{
public:
    SynthInstrumentNode()
    {
        synth_.addSound(new SineSound());

        const juce::ADSR::Parameters params { 0.005f, 0.12f, 0.7f, 0.25f };
        for (int i = 0; i < numVoices_; ++i)
        {
            auto* voice = new SineVoice();
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
        synth_.renderNextBlock(buffer, midi, 0, buffer.getNumSamples());
    }

private:
    static constexpr int numVoices_ = 16;
    juce::Synthesiser    synth_;
};

} // namespace looper::engine
