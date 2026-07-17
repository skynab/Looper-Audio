#pragma once

#include <cstdint>

namespace looper::engine
{
/** An immutable snapshot of the transport at the start of an audio block. */
struct TransportSnapshot
{
    bool    playing            = false;
    int64_t playheadSamples    = 0;
    double  bpm                = 120.0;
    int     timeSigNumerator   = 4;
    int     timeSigDenominator = 4;
    double  ppqPosition        = 0.0; // quarter-note position at block start
};

/** Everything a Node needs to render one block. Passed by const ref; POD, no JUCE. */
struct ProcessContext
{
    double            sampleRate = 0.0;
    int               numSamples = 0;
    TransportSnapshot transport;
};

} // namespace looper::engine
