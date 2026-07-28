#pragma once

#include <algorithm>
#include <cmath>

namespace looper::engine
{
/**
    A compressor pedal.

    Squash pedals are on more guitar boards than anything except tuners: they
    even out picking, add sustain to a clean part, and are what makes a funk
    or country line sit still. They also matter *before* a drive — a
    compressor into an overdrive is a different, more even distortion than an
    overdrive alone, which is exactly the kind of thing the chain's ordering
    exists to let you try.

    Feed-forward, and the smoothing is applied to the **gain reduction**
    rather than to the level detector. Smoothing the detector makes attack and
    release interact with signal level, so a stated 10ms attack isn't 10ms for
    a quiet note; smoothing the reduction makes them mean what they say, which
    is the only way the numbers on the control are worth showing.

    JUCE-free so the timing claims are measurable headlessly: an attack time
    that is quietly wrong is inaudible one note at a time and wrong on every
    note at once.
*/
class Compressor
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        updateCoefficients();
        reset();
    }

    void reset() noexcept { reductionDb_ = 0.0f; }

    void setThresholdDb(float db) noexcept { thresholdDb_ = db; }
    void setRatio(float ratio) noexcept    { ratio_ = std::max(1.0f, ratio); }

    void setAttackMs(float ms) noexcept
    {
        attackMs_ = std::max(0.1f, ms);
        updateCoefficients();
    }

    void setReleaseMs(float ms) noexcept
    {
        releaseMs_ = std::max(1.0f, ms);
        updateCoefficients();
    }

    /** The linear gain to apply to this sample, advancing the internal state.
        Returned rather than applied so a stereo pair can share one detector —
        compressing channels independently makes a hard-panned note pull the
        image across, which is not what a pedal does. */
    float gainFor(float detectorInput) noexcept
    {
        const float level   = std::abs(detectorInput);
        const float levelDb = 20.0f * std::log10(std::max(level, 1.0e-9f));
        const float overDb  = levelDb - thresholdDb_;

        // Above the threshold, keep 1/ratio of the excess: at 4:1, 12dB over
        // becomes 3dB over, so 9dB is given back.
        const float targetReductionDb = overDb > 0.0f ? -overDb * (1.0f - 1.0f / ratio_) : 0.0f;

        // More reduction is the attack direction; letting go is release.
        const float coeff = targetReductionDb < reductionDb_ ? attackCoeff_ : releaseCoeff_;
        reductionDb_ += coeff * (targetReductionDb - reductionDb_);

        return std::pow(10.0f, reductionDb_ / 20.0f);
    }

    /** How much gain reduction is currently applied, in dB (negative). Used by
        the tests, and by any meter that wants to show it. */
    float currentReductionDb() const noexcept { return reductionDb_; }

private:
    void updateCoefficients() noexcept
    {
        attackCoeff_  = timeToCoeff(attackMs_);
        releaseCoeff_ = timeToCoeff(releaseMs_);
    }

    /** One-pole coefficient reaching ~63% of a step in the stated time, which
        is the convention the numbers on a pedal refer to. */
    float timeToCoeff(float ms) const noexcept
    {
        const double samples = std::max(1.0, (double) ms * 0.001 * sampleRate_);
        return (float) (1.0 - std::exp(-1.0 / samples));
    }

    double sampleRate_   = 48000.0;
    float  thresholdDb_  = -18.0f;
    float  ratio_        = 4.0f;
    float  attackMs_     = 10.0f;
    float  releaseMs_    = 120.0f;
    float  attackCoeff_  = 0.01f;
    float  releaseCoeff_ = 0.001f;
    float  reductionDb_  = 0.0f;
};

/**
    A tremolo pedal: amplitude modulation, the oldest effect on this list.

    Depth is expressed as how far the *quiet* part drops rather than as a
    peak-to-peak swing, so depth 1 means "silent at the bottom" and depth 0
    means the pedal is doing nothing. That's the only reading under which
    turning the knob down leaves the signal untouched, which is what a player
    expects a depth control to do.

    The phase is kept in a double and wrapped rather than accumulated
    unbounded: at 48kHz an unwrapped float phase loses its resolution within
    minutes, and the audible result is a tremolo that gradually stops being
    periodic.
*/
class Tremolo
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    void reset() noexcept { phase_ = 0.0; }

    void setRateHz(float hz) noexcept  { rateHz_ = std::clamp(hz, 0.05f, 20.0f); }
    void setDepth(float depth) noexcept { depth_ = std::clamp(depth, 0.0f, 1.0f); }

    /** The gain for this sample, and the phase advances. Like the compressor,
        the gain is returned rather than applied so both channels move
        together — a tremolo that drifted between channels would turn into an
        auto-panner. */
    float nextGain() noexcept
    {
        // Starts at full and dips: a tremolo that began silent would swallow
        // the front of any note played on the beat.
        const float lfo  = 0.5f * (1.0f + (float) std::cos(kTwoPi * phase_));
        const float gain = 1.0f - depth_ * (1.0f - lfo);

        phase_ += (double) rateHz_ / sampleRate_;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        return gain;
    }

private:
    static constexpr double kTwoPi = 6.283185307179586;

    double sampleRate_ = 48000.0;
    double phase_      = 0.0;
    float  rateHz_     = 5.0f;
    float  depth_      = 0.5f;
};

} // namespace looper::engine
