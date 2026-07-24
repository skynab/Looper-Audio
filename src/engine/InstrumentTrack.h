#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SynthInstrumentNode.h"

namespace looper::engine
{
/**
    One instrument track: a synth driven by its own sequencer, with a per-track
    gain and mute.

    Tracks live in a fixed, pre-allocated pool inside the engine, so
    activating/deactivating a track is just an atomic flag — there is no real-time
    graph surgery. To apply per-track gain the synth renders into a scratch buffer
    which is then summed into the mix. The same render() is used by the live
    engine and the offline renderer, so the offline bounce genuinely exercises
    this path (gain included).
*/
struct InstrumentTrack
{
    SynthInstrumentNode      synth;
    Sequencer                sequencer;
    std::atomic<bool>        active { false };
    std::atomic<bool>        muted  { false };
    std::atomic<float>       gainDb { 0.0f };
    juce::MidiBuffer         trackMidi;
    juce::AudioBuffer<float> scratch;

    void prepare(double sampleRate, int blockSize)
    {
        synth.prepare(sampleRate, blockSize);
        trackMidi.ensureSize(2048);
        scratch.setSize(2, juce::jmax(1, blockSize));
    }

    /** Audio thread: render this track (post-gain) additively into @p mix. */
    void render(juce::AudioBuffer<float>& mix, const juce::MidiBuffer& liveMidi,
                const ProcessContext& context, bool receivesLiveMidi)
    {
        trackMidi.clear();
        sequencer.renderBlock(trackMidi, context);

        if (receivesLiveMidi)
            trackMidi.addEvents(liveMidi, 0, context.numSamples, 0);

        if (muted.load(std::memory_order_relaxed))
            return;

        const int numSamples = context.numSamples;

        // No reallocation: scratch was prepared to the maximum block size.
        scratch.setSize(2, juce::jmax(1, numSamples), false, false, true);
        scratch.clear();
        synth.process(scratch, trackMidi, context);

        const float gain     = juce::Decibels::decibelsToGain(gainDb.load(std::memory_order_relaxed));
        const int   channels = juce::jmin(mix.getNumChannels(), scratch.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
            mix.addFrom(ch, 0, scratch, ch, 0, numSamples, gain);
    }
};

} // namespace looper::engine
