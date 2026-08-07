#pragma once

#include <algorithm>
#include <cmath>

namespace looper::engine
{
/**
    A single RBJ-cookbook biquad, in one of three shapes: low shelf, high
    shelf, or peaking (bell). This is the DSP a graphic/mastering EQ needs
    that StateVariableFilter can't do — a shelf or peak that boosts or cuts a
    band's *gain* rather than just picking a cutoff.

    JUCE-free like the rest of this engine's DSP, so a shelf's low-frequency
    gain or a peak's centre-frequency bump is a headless, testable claim
    (Direct Form 1, double-precision state — this runs once per master bus,
    not per voice, so the extra precision costs nothing that matters).
*/
class ShelfPeakFilter
{
public:
    enum class Shape { LowShelf, HighShelf, Peaking };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        reset();
        updateCoefficients();
    }

    void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0.0; }

    void setShape(Shape shape) noexcept  { shape_ = shape; updateCoefficients(); }
    void setFrequency(float hz)          { frequency_ = hz; updateCoefficients(); }
    void setGainDb(float db)             { gainDb_ = db; updateCoefficients(); }
    void setQ(float q)                   { q_ = q; updateCoefficients(); }

    float processSample(float in) noexcept
    {
        const double x0 = (double) in;
        const double y0 = b0_ * x0 + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x0;
        y2_ = y1_; y1_ = y0;
        return (float) y0;
    }

private:
    void updateCoefficients()
    {
        if (sampleRate_ <= 0.0)
            return;

        constexpr double pi = 3.14159265358979323846;
        const double freq  = std::clamp((double) frequency_, 20.0, sampleRate_ * 0.49);
        const double A     = std::pow(10.0, (double) gainDb_ / 40.0);
        const double w0    = 2.0 * pi * freq / sampleRate_;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);

        double b0, b1, b2, a0, a1, a2;

        if (shape_ == Shape::Peaking)
        {
            const double alpha = sinw0 / (2.0 * std::max(0.05, (double) q_));
            b0 =  1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 =  1.0 - alpha * A;
            a0 =  1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha / A;
        }
        else
        {
            // Shelf slope S = 1 (the RBJ cookbook's "gentlest reasonable" slope
            // — a mastering shelf should sound like a tilt, not a brick wall).
            // With S = 1, (1/S - 1) == 0, so alpha reduces to sin(w0)/2 * sqrt(2).
            const double alpha = sinw0 / 2.0 * std::sqrt(2.0);
            const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;

            if (shape_ == Shape::LowShelf)
            {
                b0 =    A * ((A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAalpha);
                b1 =  2*A * ((A - 1.0) - (A + 1.0) * cosw0);
                b2 =    A * ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAalpha);
                a0 =        (A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAalpha;
                a1 =   -2 * ((A - 1.0) + (A + 1.0) * cosw0);
                a2 =        (A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAalpha;
            }
            else // HighShelf
            {
                b0 =    A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAalpha);
                b1 = -2*A * ((A - 1.0) + (A + 1.0) * cosw0);
                b2 =    A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAalpha);
                a0 =        (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAalpha;
                a1 =    2 * ((A - 1.0) - (A + 1.0) * cosw0);
                a2 =        (A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAalpha;
            }
        }

        b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0;
        a1_ = a1 / a0; a2_ = a2 / a0;
    }

    double sampleRate_ = 0.0;
    float  frequency_  = 1000.0f;
    float  gainDb_     = 0.0f;
    float  q_          = 0.707f;
    Shape  shape_       = Shape::Peaking;

    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

} // namespace looper::engine
