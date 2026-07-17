#pragma once

#include <cmath>

namespace looper::engine
{
/**
    Equal-tempered MIDI note number → frequency in Hz (note 69 = A4 = 440 Hz).
    JUCE-free so the tuning math is unit-tested headless.
*/
inline double midiNoteToHertz(int midiNote, double a4Hz = 440.0) noexcept
{
    return a4Hz * std::pow(2.0, (midiNote - 69) / 12.0);
}

} // namespace looper::engine
