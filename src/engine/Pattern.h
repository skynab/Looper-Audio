#pragma once

#include <vector>

namespace looper::engine
{
/** One note in a pattern. Times are in quarter-note beats from the pattern start. */
/** How a note is played, as distinct from how hard.

    Velocity could have carried this - quiet notes treated as muted - and that
    would have cost nothing to build. It was rejected because it conflates two
    independent things: a riff wants loud palm mutes and quiet open notes, and
    a threshold makes both impossible.

    Guitar tracks are the only instrument that acts on this today; everything
    else ignores it, which is why the default has to be the behaviour that
    already existed. */
enum class Articulation : int
{
    Normal = 0,

    /** Palm muted: the picking hand rests on the strings at the bridge, so the
        note is short and dark rather than merely quiet. See engine::GuitarNode
        for what that means in the string model, and model::GuitarSettings for
        the two values that shape it. */
    PalmMute = 1
};

struct Note
{
    double       startBeats   = 0.0;
    double       lengthBeats  = 0.25;
    int          noteNumber   = 60;
    float        velocity     = 0.8f; // 0..1
    Articulation articulation = Articulation::Normal;

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
