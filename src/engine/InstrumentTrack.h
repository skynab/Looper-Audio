#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SynthInstrumentNode.h"

namespace looper::engine
{
/**
    One instrument track: a synth driven by its own sequencer.

    Tracks live in a fixed, pre-allocated pool inside the engine, so
    activating/deactivating a track is just an atomic flag — there is no
    real-time graph surgery. Patterns reach the sequencer through its existing
    lock-free inbox. The same render() is used by the live engine and the offline
    renderer, so the offline bounce genuinely exercises this path.
*/
struct InstrumentTrack
{
    SynthInstrumentNode synth;
    Sequencer           sequencer;
    std::atomic<bool>   active { false };
    std::atomic<bool>   muted  { false };
    juce::MidiBuffer    trackMidi;

    void prepare(double sampleRate, int blockSize)
    {
        synth.prepare(sampleRate, blockSize);
        trackMidi.ensureSize(2048);
    }

    /** Audio thread: render this track additively into @p mix. */
    void render(juce::AudioBuffer<float>& mix, const juce::MidiBuffer& liveMidi,
                const ProcessContext& context, bool receivesLiveMidi)
    {
        trackMidi.clear();
        sequencer.renderBlock(trackMidi, context);

        if (receivesLiveMidi)
            trackMidi.addEvents(liveMidi, 0, context.numSamples, 0);

        if (! muted.load(std::memory_order_relaxed))
            synth.process(mix, trackMidi, context);
    }
};

} // namespace looper::engine
