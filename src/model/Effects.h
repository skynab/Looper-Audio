#pragma once

namespace looper::model
{
/** Master delay settings, stored in the document (saved + undoable state). */
struct DelaySettings
{
    bool  enabled  = false;
    float timeMs   = 300.0f;
    float feedback = 0.35f; // 0..0.95
    float mix      = 0.30f; // 0..1

    bool operator==(const DelaySettings&) const = default;
};

/** Master filter settings. mode: 0 = low-pass, 1 = high-pass, 2 = band-pass. */
struct FilterSettings
{
    bool  enabled   = false;
    int   mode      = 0;
    float cutoff    = 1000.0f; // Hz
    float resonance = 0.707f;

    bool operator==(const FilterSettings&) const = default;
};

/** Master reverb settings. */
struct ReverbSettings
{
    bool  enabled  = false;
    float roomSize = 0.5f; // 0..1
    float damping  = 0.5f; // 0..1
    float mix      = 0.3f; // 0..1

    bool operator==(const ReverbSettings&) const = default;
};

/**
    The shared send/return bus: every track can send a pre-fader portion of its
    signal into it (Track::sendLevel), summed and passed through this reverb,
    then mixed back into the master before its own effects chain. Unlike the
    master reverb, the send bus's own reverb is always fully wet — there's no
    "dry" concept for a return bus — so returnLevel is the only level control
    (a plain output gain on the wet return).
*/
struct SendBusSettings
{
    bool  enabled     = false;
    float roomSize    = 0.5f; // 0..1
    float damping     = 0.5f; // 0..1
    float returnLevel = 0.5f; // 0..1, linear gain on the wet return

    bool operator==(const SendBusSettings&) const = default;
};

} // namespace looper::model
