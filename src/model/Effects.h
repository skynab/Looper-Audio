#pragma once

#include <string>
#include <vector>

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

/** Which effect the shared send bus applies (see SendBusSettings). */
enum class SendBusEffectType { Reverb, Delay };

/**
    The shared send/return bus: every track can send a pre-fader portion of its
    signal into it (Track::sendLevel), summed and passed through one effect —
    reverb or delay, chosen by effectType — then mixed back into the master
    before its own effects chain. Unlike the master versions of these effects,
    the send bus's effect is always fully wet — there's no "dry" concept for a
    return bus — so returnLevel is the only level control (a plain output gain
    on the wet return), shared by both effect types. roomSize/damping apply
    when effectType is Reverb; delayTimeMs/delayFeedback when it's Delay —
    both sets of params are always stored so switching types doesn't lose
    whichever one isn't currently active.
*/
struct SendBusSettings
{
    bool              enabled    = false;
    SendBusEffectType effectType = SendBusEffectType::Reverb;

    float roomSize = 0.5f; // 0..1, reverb only
    float damping  = 0.5f; // 0..1, reverb only

    float delayTimeMs   = 300.0f; // delay only
    float delayFeedback = 0.35f;  // 0..0.95, delay only

    float returnLevel = 0.5f; // 0..1, linear gain on the wet return

    bool operator==(const SendBusSettings&) const = default;
};



/** Which plugin format an entry came from. The numeric values go into the
    project file, so: append, never renumber. */
enum class PluginFormat
{
    Unknown   = 0,
    VST3      = 1,
    AudioUnit = 2
};

/**
    A reference to a hosted plugin, as the *document* sees it.

    Deliberately JUCE-free rather than a juce::PluginDescription. Three
    reasons, in order of weight: the test target links Catch2 only, so anything
    JUCE-typed can't be covered headlessly and this is exactly the
    serialization-shaped problem that needs tests; a project must survive a
    plugin being missing, which means storing enough to *name* the one it
    wanted rather than silently dropping it; and PluginDescription is an
    engine/UI concern, converted at the boundary the same way AutomationCurve
    and AutomationLane already are.

    `state` is the plugin's own opaque blob, base64'd. It is not this format's
    business to understand it.
*/
struct PluginRef
{
    PluginFormat format = PluginFormat::Unknown;
    std::string  identifier; // what JUCE needs to find it again
    std::string  name;       // for display, and to say which plugin is missing
    std::string  state;      // base64 of the plugin's own state

    bool operator==(const PluginRef&) const = default;
};

/** What one slot of a track's effect chain is. */
enum class EffectKind
{
    Filter = 0,
    Delay  = 1,
    Reverb = 2,
    Plugin = 3
};

/**
    One effect in a track's chain: a built-in or a hosted plugin.

    Every built-in's settings are stored regardless of which kind the slot
    currently is, so switching kind doesn't lose the others — the same
    trade (a slightly fat struct for no lost state) SendBusSettings already
    makes for its reverb-or-delay choice.

    `enabled` belongs to the slot rather than to the settings structs, so
    bypass means the same thing for a plugin as for a built-in. The
    per-settings `enabled` flags stay for the master bus and send bus, which
    are single fixed effects rather than chain slots.
*/
struct EffectSlot
{
    EffectKind kind    = EffectKind::Filter;
    bool       enabled = false;

    FilterSettings filter;
    DelaySettings  delay;
    ReverbSettings reverb;
    PluginRef      plugin; // meaningful when kind == Plugin

    bool operator==(const EffectSlot&) const = default;
};

} // namespace looper::model
