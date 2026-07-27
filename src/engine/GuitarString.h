#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace looper::engine
{
/**
    One plucked string, as a digital waveguide (extended Karplus-Strong).

    A delay line whose length sets the pitch, fed back through a damping filter,
    excited by a shaped noise burst. Cheap — a handful of multiplies per sample —
    and it models the things that actually make a string sound like a string:
    the pitch-dependent decay, the brightness that fades as the note rings, and
    the comb colouring of where along the string it was plucked.

    JUCE-free so the tuning and decay can be measured headlessly, which is the
    whole point of doing this layer first: a string that is a few cents sharp
    up the neck is the kind of wrong that is silent in code review and obvious
    the moment anyone plays it.

    **Tuning is why this doesn't reuse DelayLine.** That one takes an integer
    delay, and rounding the loop length quantises pitch badly as notes rise —
    at 48kHz the 24th fret of the high E lands about 19 cents sharp. The loop
    here is fractional, read through an allpass interpolator, which delays
    without adding damping of its own (linear interpolation would lowpass the
    loop and muddle decay with tuning).

    Not thread-safe and not meant to be: one string belongs to one voice on the
    audio thread. Everything is pre-allocated in prepare().
*/
class GuitarString
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // Sized for the lowest note anything is likely to ask for (~30Hz, well
        // below a 7-string's low B), so setFrequency never has to allocate.
        buffer_.assign((size_t) std::ceil(sampleRate_ / 30.0) + 4, 0.0f);
        reset();
        setFrequency(frequency_);
    }

    void reset() noexcept
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_     = 0;
        lastFilterIn_   = 0.0f;
        allpassLastIn_  = 0.0f;
        allpassLastOut_ = 0.0f;
        energy_         = 0.0f;
    }

    /** Sets the pitch. Changing this *without* plucking is a hammer-on or a
        slide — the string keeps ringing at the new length. */
    void setFrequency(double hz) noexcept
    {
        frequency_ = std::clamp(hz, 20.0, sampleRate_ * 0.25);
        updateLoopLength();
        updateLoopGain();
    }

    /** How long the note takes to fall 60dB, in seconds, measured at the
        fundamental. Specified in *time* rather than as a filter coefficient so
        it holds across the range: a fixed coefficient makes high notes die far
        too fast, because they go round the loop more often per second. */
    void setDecaySeconds(double seconds) noexcept
    {
        decaySeconds_ = std::clamp(seconds, 0.05, 30.0);
        updateLoopGain();
    }

    /** 0 = dull, 1 = bright. Sets how much the loop filter rolls off each pass,
        which is what makes the tail darken as it decays. */
    void setBrightness(float brightness) noexcept
    {
        // Mapped away from both extremes: at 0.5 the filter is a two-point
        // average (maximum damping of the top), at 0 it's a pure delay and the
        // string never darkens at all.
        damping_ = 0.5f - 0.45f * std::clamp(brightness, 0.0f, 1.0f);
        updateLoopLength(); // the filter's own delay is part of the loop
    }

    /** Where along the string it's plucked: 0 = at the bridge (thin, nasal),
        0.5 = the middle (round and full). A comb notch, which is most of the
        difference between a bridge pickup and a soundhole. */
    void setPickPosition(float position) noexcept
    {
        pickPosition_ = std::clamp(position, 0.02f, 0.5f);
    }

    /** 0 = soft/fingertip, 1 = hard/plectrum. Shapes the excitation burst. */
    void setPickHardness(float hardness) noexcept
    {
        pickHardness_ = std::clamp(hardness, 0.0f, 1.0f);
    }

    /** Excites the string. Replaces whatever was ringing, which is what a
        second pluck on the same string does in life. */
    void pluck(float velocity) noexcept
    {
        const int size = (int) buffer_.size();
        if (size < 4)
            return;

        // The excitation has to land where the loop will actually read it. The
        // buffer is sized for the lowest note this could ever play, so it's far
        // longer than the current loop — filling from index 0 would put the
        // burst outside the span the read pointer visits and the string would
        // sound silence for its first pass.
        const int length     = std::clamp(integerDelay_, 2, size - 2);
        const int combOffset = std::max(1, (int) std::lround(pickPosition_ * (float) length));

        // A soft pluck excites fewer partials: lowpass the noise more.
        const float smoothing = 0.85f - 0.75f * pickHardness_;

        // Built into scratch first so the comb can read earlier samples of the
        // *excitation*, not of whatever the loop happened to contain.
        excitation_.resize((size_t) length);
        float smoothed = 0.0f;
        for (int i = 0; i < length; ++i)
        {
            const float white = nextNoise();
            smoothed = smoothing * smoothed + (1.0f - smoothing) * white;
            excitation_[(size_t) i] = smoothed;
        }

        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_ = 0;

        // Pick-position comb: the string can't move at the point it's held, so
        // the excitation cancels with a copy of itself delayed by how far along
        // it was plucked. Written backwards from the write pointer, so the
        // first sample read is the start of the burst.
        for (int i = 0; i < length; ++i)
        {
            const int   earlier = i - combOffset;
            const float delayed = earlier >= 0 ? excitation_[(size_t) earlier] : 0.0f;

            int index = writeIndex_ - length + i;
            while (index < 0)
                index += size;
            buffer_[(size_t) index] = (excitation_[(size_t) i] - delayed) * velocity;
        }

        lastFilterIn_   = 0.0f;
        allpassLastIn_  = 0.0f;
        allpassLastOut_ = 0.0f;
        energy_         = velocity;
    }

    /** Damps the string — palm muting, or a hand laid across it. 0 = open,
        1 = fully stopped. */
    void mute(float amount) noexcept
    {
        muteFactor_ = 1.0f - std::clamp(amount, 0.0f, 1.0f);
        updateLoopGain();
    }

    /** True while the string is still audibly moving. Lets a caller skip
        silent strings rather than running six loops for one note. */
    bool isRinging() const noexcept { return energy_ > 1.0e-5f; }

    /** One sample. */
    float process() noexcept
    {
        const int size = (int) buffer_.size();
        if (size < 4)
            return 0.0f;

        int readIndex = writeIndex_ - integerDelay_;
        while (readIndex < 0)
            readIndex += size;

        const float delayed = buffer_[(size_t) readIndex];

        // Allpass interpolation for the fractional part of the loop: it delays
        // without attenuating, so tuning and damping stay independent.
        const float interpolated = allpassCoeff_ * (delayed - allpassLastOut_) + allpassLastIn_;
        allpassLastIn_  = delayed;
        allpassLastOut_ = interpolated;

        // One-zero lowpass, unity gain at DC, so the decay rate is set by
        // loopGain_ alone and the filter only shapes the tail's brightness.
        const float filtered = loopGain_ * ((1.0f - damping_) * interpolated + damping_ * lastFilterIn_);
        lastFilterIn_ = interpolated;

        buffer_[(size_t) writeIndex_] = filtered;
        writeIndex_ = (writeIndex_ + 1) % size;

        // Cheap envelope follower, only so isRinging() can retire the voice.
        energy_ += 0.001f * (std::abs(filtered) - energy_);
        return interpolated;
    }

private:
    int loopLengthSamples() const noexcept
    {
        return (int) std::lround(sampleRate_ / frequency_);
    }

    /** Splits the required loop period across the integer delay, the allpass
        fraction and the loop filter's own phase delay — all three add up, so
        the filter's contribution has to come out of the delay line or the
        string plays sharp. */
    void updateLoopLength() noexcept
    {
        if (buffer_.size() < 4)
            return;

        const double totalPeriod = sampleRate_ / frequency_;

        // Phase delay of the one-zero loop filter at the fundamental, derived
        // rather than approximated as `damping_`: the approximation is fine low
        // down and drifts sharp as the pitch rises, which is exactly where
        // tuning errors are most audible.
        const double omega = 2.0 * M_PI * frequency_ / sampleRate_;
        const double b     = damping_;
        const double real  = (1.0 - b) + b * std::cos(omega);
        const double imag  = -b * std::sin(omega);
        const double filterDelay = omega > 1.0e-9 ? -std::atan2(imag, real) / omega : b;

        double lineDelay = totalPeriod - filterDelay;

        // The allpass is well behaved for fractions around 0.5 and misbehaves
        // near zero, so borrow a whole sample from the integer part.
        int    integerPart = (int) std::floor(lineDelay);
        double fraction    = lineDelay - integerPart;
        if (fraction < 0.1)
        {
            integerPart -= 1;
            fraction    += 1.0;
        }

        integerDelay_  = std::clamp(integerPart, 1, (int) buffer_.size() - 2);
        allpassCoeff_  = (float) ((1.0 - fraction) / (1.0 + fraction));
    }

    /** Loop gain for the requested T60. A string goes round its loop f0 times a
        second, so the per-pass gain that reaches -60dB in t seconds depends on
        pitch — this is the pitch compensation the plan calls for. Always < 1,
        so the loop cannot self-oscillate. */
    void updateLoopGain() noexcept
    {
        const double passes = std::max(1.0, decaySeconds_ * frequency_);
        loopGain_ = (float) std::exp(std::log(0.001) / passes) * muteFactor_;
        loopGain_ = std::min(loopGain_, 0.99999f);
    }

    /** Deterministic noise: a plucked string wants a burst, and a fixed
        sequence makes the tests repeatable. */
    float nextNoise() noexcept
    {
        noiseState_ = noiseState_ * 1664525u + 1013904223u;
        return (float) ((int32_t) noiseState_) * (1.0f / 2147483648.0f);
    }

    double sampleRate_    = 48000.0;
    double frequency_     = 110.0;
    double decaySeconds_  = 2.0;

    std::vector<float> buffer_;
    std::vector<float> excitation_;
    int                writeIndex_   = 0;
    int                integerDelay_ = 100;

    float allpassCoeff_   = 0.0f;
    float allpassLastIn_  = 0.0f;
    float allpassLastOut_ = 0.0f;
    float lastFilterIn_   = 0.0f;

    float damping_      = 0.15f;
    float loopGain_     = 0.999f;
    float muteFactor_   = 1.0f;
    float pickPosition_ = 0.25f;
    float pickHardness_ = 0.6f;
    float energy_       = 0.0f;

    uint32_t noiseState_ = 22222u;
};

} // namespace looper::engine
