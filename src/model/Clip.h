#pragma once

#include <string>

#include "engine/Pattern.h"

namespace looper::model
{
enum class ClipType
{
    Instrument, // holds a MIDI Pattern
    Audio       // references an audio file
};

/** A clip placed on a track's timeline. Times are in quarter-note beats. */
struct Clip
{
    int         id          = 0;
    ClipType    type        = ClipType::Instrument;
    double      startBeats  = 0.0;
    double      lengthBeats = 4.0;

    engine::Pattern pattern;   // used when type == Instrument
    std::string     audioFile; // used when type == Audio

    bool operator==(const Clip&) const = default;
};

} // namespace looper::model
