#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SynthInstrumentNode.h"

namespace looper::engine
{
/**
    One instrument track: a synth driven by its own sequencer, with a per-track
    gain, mute, solo, and post-gain peak metering.

    Tracks live in a fixed, pre-allocated pool inside the engine, so
    activating/deactivating a track is just an atomic flag — there is no real-time
    graph surgery. To apply per-track gain the synth renders into a scratch buffer
    which is then summed into the mix. The same render() is used by the live
    engine and the offline renderer, so the offline bounce genuinely exercises
    this path (gain included).

    Solo is resolved by the caller: it passes in whether *any* track in the pool
    is currently soloed (a single scan of atomics, done once per block), and this
    track goes silent if it's muted, or if some other track is soloed and this one
    isn't — the standard "solo overrides, mute always wins" behaviour.
*/
struct InstrumentTrack
{
    SynthInstrumentNode      synth;
    Sequencer                sequencer;
    std::atomic<bool>        active { false };
    std::atomic<bool>        muted  { false };
    std::atomic<bool>        solo   { false };
    std::atomic<float>       gainDb { 0.0f };
    juce::MidiBuffer         trackMidi;
    juce::AudioBuffer<float> scratch;
    std::atomic<float>       channelPeak_[2] {};

    void prepare(double sampleRate, int blockSize)
    {
        synth.prepare(sampleRate, blockSize);
        trackMidi.ensureSize(2048);
        scratch.setSize(2, juce::jmax(1, blockSize));
    }

    /** Read by the UI thread for the mixer strip's meter. */
    float peak(int channel) const noexcept
    {
        return (channel >= 0 && channel < 2)
            ? channelPeak_[channel].load(std::memory_order_relaxed)
            : 0.0f;
    }

    /** Audio thread: render this track (post-gain) additively into @p mix. */
    void render(juce::AudioBuffer<float>& mix, const juce::MidiBuffer& liveMidi,
                const ProcessContext& context, bool receivesLiveMidi, bool anySoloActive)
    {
        trackMidi.clear();
        sequencer.renderBlock(trackMidi, context);

        if (receivesLiveMidi)
            trackMidi.addEvents(liveMidi, 0, context.numSamples, 0);

        const bool audible = ! muted.load(std::memory_order_relaxed)
                           && (! anySoloActive || solo.load(std::memory_order_relaxed));

        if (! audible)
        {
            channelPeak_[0].store(0.0f, std::memory_order_relaxed);
            channelPeak_[1].store(0.0f, std::memory_order_relaxed);
            return;
        }

        const int numSamples = context.numSamples;

        // No reallocation: scratch was prepared to the maximum block size.
        scratch.setSize(2, juce::jmax(1, numSamples), false, false, true);
        scratch.clear();
        synth.process(scratch, trackMidi, context);

        const float gain     = juce::Decibels::decibelsToGain(gainDb.load(std::memory_order_relaxed));
        const int   channels = juce::jmin(mix.getNumChannels(), scratch.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
        {
            mix.addFrom(ch, 0, scratch, ch, 0, numSamples, gain);

            if (ch < 2)
            {
                float peak = 0.0f;
                const float* data = scratch.getReadPointer(ch);
                for (int i = 0; i < numSamples; ++i)
                    peak = std::max(peak, std::abs(data[i]) * gain);
                channelPeak_[ch].store(peak, std::memory_order_relaxed);
            }
        }
    }
};

} // namespace looper::engine
