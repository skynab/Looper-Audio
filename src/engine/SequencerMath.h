#pragma once

#include <cmath>

namespace looper::engine
{
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
