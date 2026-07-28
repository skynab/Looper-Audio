#pragma once

#include <cmath>

namespace looper::engine
{
/** Where a loop should end, given how far the arranged content reaches.

    Rounded up to a whole bar so the loop lands on a bar line rather than
    mid-phrase, and never shorter than one bar — a zero-length loop region
    would either stall the transport or divide by zero downstream, and an
    empty song is exactly when that would happen. */
inline double loopEndForContent(double contentEndBeats, double beatsPerBar) noexcept
{
    const double bar = beatsPerBar > 0.0 ? beatsPerBar : 4.0;
    if (! (contentEndBeats > 0.0))
        return bar;

    const double bars = std::ceil(contentEndBeats / bar);
    return (bars < 1.0 ? 1.0 : bars) * bar;
}

/** Positive floating-point modulo: result is always in [0, length). */
inline double wrapPositive(double x, double length) noexcept
{
    if (length <= 0.0)
        return 0.0;

    const double m = std::fmod(x, length);
    return m < 0.0 ? m + length : m;
}

/**
    Decides whether a note edge (a start or end time, in pattern samples) falls
    within the block that begins at @p blockStartInPattern and spans @p numSamples,
    accounting for the pattern looping with period @p length. If so, writes the
    in-block sample @p offset and returns true.

    Blocks are always shorter than the pattern, so each edge is hit at most once
    per block — which makes the modular comparison unambiguous.
*/
inline bool edgeInBlock(double edgeSample, double blockStartInPattern, double length,
                        int numSamples, int& offset) noexcept
{
    const double delta = wrapPositive(edgeSample - blockStartInPattern, length);
    if (delta < (double) numSamples)
    {
        offset = (int) delta;
        return true;
    }
    return false;
}

} // namespace looper::engine
