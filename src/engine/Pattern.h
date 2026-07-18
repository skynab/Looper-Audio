#pragma once

#include <vector>

namespace looper::engine
{
/** One note in a pattern. Times are in quarter-note beats from the pattern start. */
struct Note
{
    double startBeats  = 0.0;
    double lengthBeats = 0.25;
    int    noteNumber  = 60;
    float  velocity    = 0.8f; // 0..1

    bool operator==(const Note&) const = default;
};

/** A looping bar of notes. JUCE-free so it can be copied and snapshotted freely. */
struct Pattern
{
    std::vector<Note> notes;
    double            lengthBeats = 4.0; // one 4/4 bar

    bool operator==(const Pattern&) const = default;
};

} // namespace looper::engine
