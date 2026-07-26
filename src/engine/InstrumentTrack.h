#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/AudioFilePlayerNode.h"
#include "engine/DelayEffect.h"
#include "engine/DrumKitNode.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"
#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SynthInstrumentNode.h"

namespace looper::engine
{
/**
    One mixer channel: a synth (or drum kit — see isDrumTrack) driven by its
    own sequencer, *and* an audio-clip player, both summed into the same
    per-track gain, mute, solo, pre-fader send, and post-gain peak metering.
    A track only uses whichever of these it's been given content for — an
    Instrument-type track gets a pattern for the synth, an Audio-type track
    gets a decoded clip via audioPlayer, a Drum-type track gets a pattern for
    the drum kit instead of the synth — but every node always exists on every
    pool slot, so there's no track-type branching in most of the engine.

    isDrumTrack is the one exception: unlike audioPlayer (which naturally
    stays silent with no clip submitted), the synth always produces *some*
    sound for any note it receives, so a Drum track's notes must be routed to
    drumKit instead of synth, not merely left for content-gating to sort out.

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

    sendLevel sends a copy of the raw (pre-fader) synth output into the caller's
    shared send bus, independent of the track's own gainDb — so a track can be
    faded down in the main mix while still reaching the send bus at a fixed level
    (the usual "aux send" behaviour), or vice versa.
*/
struct InstrumentTrack
{
    SynthInstrumentNode      synth;
    DrumKitNode              drumKit;
    Sequencer                sequencer;
    AudioFilePlayerNode      audioPlayer;

    // This track's insert chain, in fixed order, applied to its own output
    // before the fader (and therefore before the send too, so a send carries
    // the processed signal — the usual behaviour). Each passes audio through
    // untouched while disabled, which is how they all start, so a track with
    // no inserts configured costs three branch-and-returns per block.
    FilterEffect             insertFilter;
    DelayEffect              insertDelay;
    ReverbEffect             insertReverb;
    std::atomic<bool>        active      { false };
    std::atomic<bool>        muted       { false };
    std::atomic<bool>        solo        { false };
    std::atomic<bool>        isDrumTrack { false }; // true: notes drive drumKit, not synth
    std::atomic<float>       gainDb      { 0.0f };
    std::atomic<float>       sendLevel   { 0.0f }; // 0..1, pre-fader
    juce::MidiBuffer         trackMidi;
    juce::AudioBuffer<float> scratch;
    std::atomic<float>       channelPeak_[2] {};

    void prepare(double sampleRate, int blockSize)
    {
        synth.prepare(sampleRate, blockSize);
        drumKit.prepare(sampleRate, blockSize);
        audioPlayer.prepare(sampleRate, blockSize);
        insertFilter.prepare(sampleRate, blockSize);
        insertDelay.prepare(sampleRate, blockSize);
        insertReverb.prepare(sampleRate, blockSize);
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

    /** Audio thread: render this track (post-gain) additively into @p mix, and
        its pre-fader send additively into @p sendBus. */
    void render(juce::AudioBuffer<float>& mix, juce::AudioBuffer<float>& sendBus,
                const juce::MidiBuffer& liveMidi,
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
        if (isDrumTrack.load(std::memory_order_relaxed))
            drumKit.process(scratch, trackMidi, context);
        else
            synth.process(scratch, trackMidi, context);
        audioPlayer.process(scratch, trackMidi, context); // adds in; midi is ignored

        // Inserts run on the summed track output, before gain and before the
        // send is taken — so lowering the fader doesn't change the effect, and
        // the send carries the processed sound.
        insertFilter.process(scratch);
        insertDelay.process(scratch);
        insertReverb.process(scratch);

        const float gain     = juce::Decibels::decibelsToGain(gainDb.load(std::memory_order_relaxed));
        const float send     = sendLevel.load(std::memory_order_relaxed);
        const int   channels = juce::jmin(mix.getNumChannels(), scratch.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
        {
            mix.addFrom(ch, 0, scratch, ch, 0, numSamples, gain);

            if (send > 0.0f && ch < sendBus.getNumChannels())
                sendBus.addFrom(ch, 0, scratch, ch, 0, numSamples, send);

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
