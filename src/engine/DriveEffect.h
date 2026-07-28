#pragma once

#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Waveshaper.h"

namespace looper::engine
{
/**
    An overdrive/distortion pedal: gain into a clipper, through a speaker.

    Signal path, and the reason for the order:

        pre-emphasis -> drive -> shaper -> cabinet -> tilt -> level

    Pre-emphasis sits *before* the shaper because a tone control before
    clipping decides which frequencies get distorted, while one after only
    shapes what came out. Both are real pedal designs and they sound
    different; tilting the input toward the mids is what stops a chord from
    turning to mush, since a loud low string otherwise dominates the clipper
    and takes the rest of the chord down with it.

    The cabinet is not optional and not a later refinement. Clipping puts
    enormous energy above a guitar speaker's ~5kHz corner, and that energy is
    heard as fizz. Shipping the drive without it would sound wrong, and the
    wrongness would be blamed on the drive.

    Parameters are atomics set from the message thread and read per block, the
    same arrangement FilterEffect and DelayEffect use: a knob turn must not
    rebuild the chain, since that would reset every tail in it.
*/
class DriveEffect
{
public:
    void prepare(double sampleRate, int /*blockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        for (auto& channel : channels_)
        {
            channel.shaper.reset();
            channel.cab.prepare(sampleRate_);
            channel.preState  = 0.0f;
            channel.tiltState = 0.0f;
        }

        // ~700Hz: above the low strings' fundamentals, below where the pick
        // attack lives, which is the split that keeps chords defined.
        preCoeff_  = onePoleCoeff(700.0f);
        tiltCoeff_ = onePoleCoeff(900.0f);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void setDrive(float drive)    { drive_.store(drive, std::memory_order_relaxed); }
    void setTone(float tone)      { tone_.store(tone, std::memory_order_relaxed); }
    void setLevel(float level)    { level_.store(level, std::memory_order_relaxed); }
    void setHardClip(bool hard)   { hard_.store(hard, std::memory_order_relaxed); }
    void setCabinet(bool on)      { cabinet_.store(on, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer)
    {
        if (! enabled_.load(std::memory_order_relaxed))
            return;

        const float drive   = juce::jlimit(0.1f, 100.0f, drive_.load(std::memory_order_relaxed));
        const float tone    = juce::jlimit(0.0f, 1.0f, tone_.load(std::memory_order_relaxed));
        const float level   = juce::jlimit(0.0f, 2.0f, level_.load(std::memory_order_relaxed));
        const bool  hard    = hard_.load(std::memory_order_relaxed);
        const bool  cabinet = cabinet_.load(std::memory_order_relaxed);

        // Loud settings would otherwise just get louder: a drive pedal that
        // doubles as a volume control is unusable, so the make-up gain falls
        // as the drive rises. Not exact loudness matching — just enough that
        // sweeping the knob auditions the *tone* rather than the level.
        const float makeUp = level / std::sqrt(juce::jmax(1.0f, drive));

        const int numChannels = juce::jmin(buffer.getNumChannels(), (int) kMaxChannels);

        for (int channelIndex = 0; channelIndex < numChannels; ++channelIndex)
        {
            auto& state  = channels_[(size_t) channelIndex];
            auto* samples = buffer.getWritePointer(channelIndex);

            state.shaper.setKind(hard ? Waveshaper::Kind::Hard : Waveshaper::Kind::Soft);
            state.shaper.setDrive(drive);

            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                float x = samples[n];

                // Pre-emphasis: lift what's above the corner relative to what
                // is below it, so the clipper is driven by the mids.
                state.preState += preCoeff_ * (x - state.preState);
                x = x - 0.6f * state.preState;

                x = state.shaper.processSample(x);

                if (cabinet)
                    x = state.cab.processSample(x);

                // Post tilt: the pedal's tone knob, sweeping between darker
                // and brighter around a fixed corner.
                state.tiltState += tiltCoeff_ * (x - state.tiltState);
                const float high = x - state.tiltState;
                x = state.tiltState + high * (0.25f + 1.75f * tone);

                samples[n] = x * makeUp;
            }
        }
    }

private:
    static constexpr size_t kMaxChannels = 2;

    struct ChannelState
    {
        Waveshaper shaper;
        CabinetSim cab;
        float      preState  = 0.0f;
        float      tiltState = 0.0f;
    };

    float onePoleCoeff(float hz) const
    {
        const float x = (float) (6.2831853 * (double) hz / sampleRate_);
        return juce::jlimit(0.0f, 1.0f, x / (1.0f + x));
    }

    double sampleRate_ = 48000.0;
    float  preCoeff_   = 0.1f;
    float  tiltCoeff_  = 0.1f;

    std::array<ChannelState, kMaxChannels> channels_;

    std::atomic<bool>  enabled_ { false };
    std::atomic<float> drive_   { 4.0f };
    std::atomic<float> tone_    { 0.5f };
    std::atomic<float> level_   { 0.7f };
    std::atomic<bool>  hard_    { false };
    std::atomic<bool>  cabinet_ { true };
};

} // namespace looper::engine
