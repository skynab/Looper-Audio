#pragma once

#include <algorithm>
#include <cmath>

namespace looper::engine
{
/**
    Anti-aliased waveshaping, the core of a drive pedal.

    Clipping generates harmonics without limit. Every one above Nyquist folds
    back to a frequency unrelated to anything being played, so it isn't heard
    as brightness — it's heard as metallic, detuned grit that gets worse the
    higher you play. That folding is the main thing separating a distortion
    that sounds like an amp from one that sounds like a broken converter.

    Oversampling is the usual fix and costs a resampler, its filters, and
    several times the work per sample. First-order antiderivative
    anti-aliasing (ADAA) buys a measured 4.5-7dB of alias reduction for a few
    flops and no buffers — which also means there is nothing to allocate, so
    it is RT-safe by construction rather than by discipline.

    That is a real improvement, not a solved problem: heavy drive on high
    notes will still fold. Second-order ADAA or 2x oversampling on top is the
    next step if it isn't enough, and both fit behind this interface.

    For a memoryless shaper f with antiderivative F:

        y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])

    which is f averaged over the segment the signal actually crossed this
    sample, instead of a point sample of it. That average is what suppresses
    the aliases.

    JUCE-free so the aliasing claim can be measured headlessly rather than
    asserted in a comment — see the tests, which drive a sine whose fifth
    harmonic folds to a bin nothing else occupies.
*/
class Waveshaper
{
public:
    enum class Kind
    {
        Soft = 0, // tanh: an overdrive, compressing gradually into clip
        Hard      // a fuzz: flat above the threshold
    };

    void reset() noexcept { lastInput_ = 0.0; }

    void setKind(Kind kind) noexcept { kind_ = kind; }

    /** @p drive multiplies the input before shaping, which is what a drive
        knob does: the shaper's own curve never changes, you just push more
        signal into the same curve. */
    void setDrive(float drive) noexcept { drive_ = std::max(0.01f, drive); }

    float processSample(float input) noexcept
    {
        const double x0 = lastInput_;
        const double x1 = (double) input * (double) drive_;
        lastInput_      = x1;

        const double delta = x1 - x0;

        // As x1 approaches x0 the quotient becomes 0/0 and, well before that,
        // loses precision: F is order 1-10 at usable drive settings while the
        // difference goes to zero, so the subtraction cancels away most of the
        // significant digits. A sine passes through this at both peaks, every
        // cycle.
        //
        // Done in double so that region stays well conditioned. Measured, this
        // makes no difference to the aliasing figures against a float version
        // — the ill-conditioned window is narrow and rarely landed in — but
        // it costs almost nothing and the hazard is real rather than
        // theoretical.
        //
        // A sustained note is exactly where consecutive samples are nearly
        // equal, so getting the fallback wrong turns held notes into noise,
        // which is precisely backwards.
        if (std::abs(delta) < kMinDelta)
            return (float) shape(0.5 * (x0 + x1));

        return (float) ((antiderivative(x1) - antiderivative(x0)) / delta);
    }

    /** The shaper itself, with no anti-aliasing. Only useful for comparison —
        the tests measure how much aliasing the ADAA path avoids relative to
        this. */
    float processSampleNaive(float input) noexcept
    {
        return (float) shape((double) input * (double) drive_);
    }

private:
    // Only has to cover a genuine 0/0. In double the cancellation near it
    // still leaves ample precision, and the midpoint fallback is a good
    // approximation over so short a segment anyway.
    static constexpr double kMinDelta = 1.0e-8;

    double shape(double x) const noexcept
    {
        if (kind_ == Kind::Hard)
            return std::clamp(x, -1.0, 1.0);
        return std::tanh(x);
    }

    double antiderivative(double x) const noexcept
    {
        if (kind_ == Kind::Hard)
        {
            // The integral of clamp: quadratic inside the linear region,
            // linear outside it, and continuous at the corners.
            const double a = std::abs(x);
            return a <= 1.0 ? 0.5 * x * x : a - 0.5;
        }

        // The integral of tanh is log(cosh(x)) — but cosh overflows to
        // infinity around |x| = 710, and a drive pedal is exactly where large
        // inputs turn up. This identity is equal to it and overflows nowhere:
        //     log(cosh x) = |x| + log1p(exp(-2|x|)) - log 2
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - kLogTwo;
    }

    static constexpr double kLogTwo = 0.69314718055994531;

    Kind   kind_      = Kind::Soft;
    float  drive_     = 1.0f;
    double lastInput_ = 0.0;
};

/**
    A guitar speaker, roughly.

    A real cab rolls off hard above ~5kHz and below ~90Hz. Distortion is full
    of energy above that top corner, and without the cab it is heard as fizz:
    the same signal that sounds like an amp through a speaker sounds like a
    broken tweeter without one. This ships alongside the drive rather than
    after it, because drive without it would be judged as sounding wrong and
    the wrongness would be blamed on the drive.

    Two one-pole sections rather than an impulse response: a convolution would
    be more faithful and would need an IR to ship, a partitioned convolver,
    and a latency story. This is the part of the effect you'd notice missing,
    not the part you'd notice approximated.
*/
class CabinetSim
{
public:
    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        setCorners(kDefaultLowHz, kDefaultHighHz);
        reset();
    }

    void reset() noexcept
    {
        lowState_ = highState_ = 0.0f;
        lowState2_ = 0.0f;
    }

    void setCorners(float highPassHz, float lowPassHz) noexcept
    {
        highPassCoeff_ = onePoleCoeff(highPassHz);
        lowPassCoeff_  = onePoleCoeff(lowPassHz);
    }

    float processSample(float input) noexcept
    {
        // Two cascaded one-poles for the top end: a single pole's 6dB/octave
        // isn't steep enough to take the fizz out, and it's the difference
        // between "dull" and "like a speaker".
        lowState_  += lowPassCoeff_ * (input - lowState_);
        lowState2_ += lowPassCoeff_ * (lowState_ - lowState2_);

        // Highpass by subtracting a lowpassed copy — the cheapest way to get
        // the cab's missing bottom, which is what keeps drive from turning
        // into mud.
        highState_ += highPassCoeff_ * (lowState2_ - highState_);
        return lowState2_ - highState_;
    }

private:
    static constexpr float kDefaultLowHz  = 90.0f;
    static constexpr float kDefaultHighHz = 4500.0f;

    float onePoleCoeff(float hz) const noexcept
    {
        const float x = (float) (6.2831853 * (double) hz / sampleRate_);
        return std::clamp(x / (1.0f + x), 0.0f, 1.0f);
    }

    double sampleRate_    = 48000.0;
    float  lowPassCoeff_  = 0.5f;
    float  highPassCoeff_ = 0.01f;
    float  lowState_      = 0.0f;
    float  lowState2_     = 0.0f;
    float  highState_     = 0.0f;
};

} // namespace looper::engine
