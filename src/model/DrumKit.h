#pragma once

#include <string>
#include <vector>

namespace looper::model
{
/** One pad of a drum kit: a MIDI note number that triggers it, a display
    label, and the sample assigned to it (empty = silent, no sample loaded
    yet — the same safe default this project already uses for an audio clip
    with no file assigned). */
struct DrumPad
{
    int         noteNumber = 36;
    std::string label;
    std::string samplePath;

    bool operator==(const DrumPad&) const = default;
};

/** A track's drum kit: a small, fixed-ish list of independently replaceable
    one-shot pads (see engine::DrumKitNode), as opposed to the one melodic
    synth timbre an Instrument track's notes all share. */
struct DrumKit
{
    std::vector<DrumPad> pads;

    bool operator==(const DrumKit&) const = default;
};

/** The starting kit a new Drum track gets: Kick/Snare/Hat/Other on the usual
    GM-ish note numbers, no samples assigned yet. Not a full GM drum map —
    matches "one or more bass, snare, and other instrument types"; more pads
    can be added later without changing this shape. */
inline DrumKit makeDefaultDrumKit()
{
    DrumKit kit;
    kit.pads.push_back({ 36, "Kick", "" });
    kit.pads.push_back({ 38, "Snare", "" });
    kit.pads.push_back({ 42, "Hat", "" });
    kit.pads.push_back({ 45, "Other", "" });
    return kit;
}

} // namespace looper::model
