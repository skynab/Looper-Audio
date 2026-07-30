#pragma once

#include <array>
#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/PedalDsp.h"

namespace looper::engine
{
/**
    The compressor pedal as a chain node.

    One detector drives both channels. Compressing them independently makes a
    hard-panned note pull the stereo image across as it ducks, which is not
    what a pedal does — so the detector is fed the loudest channel and the
    resulting gain is applied to all of them.

    Parameters are atomics read once per block, matching FilterEffect and
    DriveEffect: turning a knob must not rebuild the chain, since that would
    reset every tail in it.
*/
class CompressorEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/)
    {
        compressor_.prepare(sampleRate);
    }

    void setEnabled(bool enabled)     { enabled_.store(enabled, std::memory_order_relaxed); }
    void setThresholdDb(float db)     { thresholdDb_.store(db, std::memory_order_relaxed); }
    void setRatio(float ratio)        { ratio_.store(ratio, std::memory_order_relaxed); }
    void setAttackMs(float ms)        { attackMs_.store(ms, std::memory_order_relaxed); }
    void setReleaseMs(float ms)       { releaseMs_.store(ms, std::memory_order_relaxed); }
    void setMakeUpDb(float db)        { makeUpDb_.store(db, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        compressor_.setThresholdDb(thresholdDb_.load(std::memory_order_relaxed));
        compressor_.setRatio(ratio_.load(std::memory_order_relaxed));
        compressor_.setAttackMs(attackMs_.load(std::memory_order_relaxed));
        compressor_.setReleaseMs(releaseMs_.load(std::memory_order_relaxed));

        const float makeUp = juce::Decibels::decibelsToGain(
            juce::jlimit(-12.0f, 24.0f, makeUpDb_.load(std::memory_order_relaxed)));

        const int numChannels = buffer.getNumChannels();
        const int numSamples  = buffer.getNumSamples();

        for (int n = 0; n < numSamples; ++n)
        {
            // The detector sees the loudest channel, so the pair ducks
            // together on whichever one is actually loud.
            float peak = 0.0f;
            for (int channel = 0; channel < numChannels; ++channel)
                peak = juce::jmax(peak, std::abs(buffer.getSample(channel, n)));

            const float gain = compressor_.gainFor(peak) * makeUp;

            for (int channel = 0; channel < numChannels; ++channel)
                buffer.setSample(channel, n, buffer.getSample(channel, n) * gain);
        }
    }

private:
    Compressor compressor_;

    std::atomic<bool>  enabled_     { false };
    std::atomic<float> thresholdDb_ { -18.0f };
    std::atomic<float> ratio_       { 4.0f };
    std::atomic<float> attackMs_    { 10.0f };
    std::atomic<float> releaseMs_   { 120.0f };
    std::atomic<float> makeUpDb_    { 0.0f };
};

/** The tremolo pedal as a chain node. One LFO for every channel — a tremolo
    whose channels drifted apart would be an auto-panner. */
class TremoloEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/) { tremolo_.prepare(sampleRate); }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setRateHz(float hz)      { rateHz_.store(hz, std::memory_order_relaxed); }
    void setDepth(float depth)    { depth_.store(depth, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        tremolo_.setRateHz(rateHz_.load(std::memory_order_relaxed));
        tremolo_.setDepth(depth_.load(std::memory_order_relaxed));

        const int numChannels = buffer.getNumChannels();

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float gain = tremolo_.nextGain();
            for (int channel = 0; channel < numChannels; ++channel)
                buffer.setSample(channel, n, buffer.getSample(channel, n) * gain);
        }
    }

private:
    Tremolo tremolo_;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> rateHz_  { 5.0f };
    std::atomic<float> depth_   { 0.5f };
};

/** The chorus pedal as a chain node. One LFO phase per channel, offset so the
    two sides drift apart — a chorus with both channels identical is a mono
    effect played twice, and the width is most of why one is used. */
class ChorusEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/)
    {
        for (auto& channel : channels_)
            channel.prepare(sampleRate);

        // Half a cycle apart, so the left drifts sharp as the right drifts
        // flat. Applied once here rather than per block: nudging it every
        // block would make the offset depend on block size.
        if (channels_.size() > 1)
            for (int n = 0; n < kStereoOffsetSamples; ++n)
                channels_[1].processSample(0.0f);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setRateHz(float hz)      { rateHz_.store(hz, std::memory_order_relaxed); }
    void setDepth(float depth)    { depth_.store(depth, std::memory_order_relaxed); }
    void setMix(float mix)        { mix_.store(mix, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float rate  = rateHz_.load(std::memory_order_relaxed);
        const float depth = depth_.load(std::memory_order_relaxed);
        const float mix   = mix_.load(std::memory_order_relaxed);

        const int numChannels = juce::jmin(buffer.getNumChannels(), (int) channels_.size());

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& chorus = channels_[(size_t) channel];
            chorus.setRateHz(rate);
            chorus.setDepth(depth);
            chorus.setMix(mix);

            auto* samples = buffer.getWritePointer(channel);
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                samples[n] = chorus.processSample(samples[n]);
        }
    }

private:
    // A quarter of a second at 48kHz — enough that the two channels' LFOs are
    // audibly apart whatever the rate.
    static constexpr int kStereoOffsetSamples = 12000;

    std::array<Chorus, 2> channels_;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> rateHz_  { 0.6f };
    std::atomic<float> depth_   { 0.5f };
    std::atomic<float> mix_     { 0.5f };
};

} // namespace looper::engine
