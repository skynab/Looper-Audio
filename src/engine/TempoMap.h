#pragma once

#include <cmath>
#include <cstdint>

namespace looper::engine
{
/**
    Converts between sample position and musical position (PPQ = quarter-note
    beats, and bars/beats).

    Phase 1 models a single constant tempo and time signature. The interface is
    deliberately shaped so it can grow into a piecewise map (a sorted list of
    tempo/time-signature changes) without callers changing — they already ask
    "what is the musical position at sample X?" rather than assuming constant tempo.

    No JUCE dependency, so it unit-tests fast and headless.
*/
class TempoMap
{
public:
    void setSampleRate(double sr) noexcept        { if (sr > 0.0) sampleRate_ = sr; }
    void setTempo(double bpm) noexcept            { if (bpm > 0.0) bpm_ = bpm; }
    void setTimeSignature(int num, int den) noexcept
    {
        if (num > 0) numerator_ = num;
        if (den > 0) denominator_ = den;
    }

    double sampleRate() const noexcept          { return sampleRate_; }
    double tempo() const noexcept               { return bpm_; }
    int    timeSigNumerator() const noexcept    { return numerator_; }
    int    timeSigDenominator() const noexcept  { return denominator_; }

    /** Samples per quarter note at the current tempo/sample-rate. */
    double samplesPerBeat() const noexcept { return sampleRate_ * 60.0 / bpm_; }

    /** Quarter-note bar length (e.g. 4 for 4/4, 3 for 6/8). */
    double quartersPerBar() const noexcept { return numerator_ * 4.0 / denominator_; }

    double  ppqFromSamples(int64_t samples) const noexcept { return (double) samples / samplesPerBeat(); }
    int64_t samplesFromPpq(double ppq) const noexcept      { return (int64_t) std::llround(ppq * samplesPerBeat()); }

    /** 1-based bar and beat (beat counted in the time signature's denominator unit) plus fractional tick. */
    struct BarsBeats
    {
        int    bar  = 1;
        int    beat = 1;
        double tick = 0.0; // 0..1 within the beat
    };

    BarsBeats barsBeatsFromPpq(double ppq) const noexcept
    {
        // Position measured in denominator-beats (e.g. eighth notes for x/8).
        const double posInBeats = ppq * denominator_ / 4.0;
        const int    bar        = (int) std::floor(posInBeats / numerator_);
        const double beatInBar  = posInBeats - (double) bar * numerator_;
        const int    beat       = (int) std::floor(beatInBar);
        return { bar + 1, beat + 1, beatInBar - beat };
    }

    BarsBeats barsBeatsFromSamples(int64_t samples) const noexcept
    {
        return barsBeatsFromPpq(ppqFromSamples(samples));
    }

private:
    double sampleRate_  = 48000.0;
    double bpm_         = 120.0;
    int    numerator_   = 4;
    int    denominator_ = 4;
};

} // namespace looper::engine
