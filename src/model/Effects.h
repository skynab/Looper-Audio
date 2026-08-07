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

/** Master 3-band EQ: fixed-frequency treble/mid/bass shelving+peak, the
    "regular mastering controls" a whole-song bus gets rather than the
    sweepable single-band FilterSettings above. Crossovers are fixed
    (bassHz/trebleHz below) rather than user-adjustable — three knobs, not a
    parametric EQ. */
struct EqSettings
{
    bool  enabled = false;
    float bassDb   = 0.0f; // -18..+18, low shelf below bassHz
    float midDb    = 0.0f; // -18..+18, peaking band between bassHz and trebleHz
    float trebleDb = 0.0f; // -18..+18, high shelf above trebleHz

    static constexpr float bassHz   = 250.0f;
    static constexpr float trebleHz = 4000.0f;

    bool operator==(const EqSettings&) const = default;
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
    Plugin = 3,
    Drive      = 4, // the guitar pedals — see engine::DriveEffect and PedalEffects.h
    Compressor = 5,
    Tremolo    = 6,
    Chorus     = 7,
    Wobble     = 8
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
/** An overdrive/distortion pedal. `hardClip` picks a fuzz's flat ceiling over
    an overdrive's gradual compression; `cabinet` is on by default because
    drive without a speaker sim is heard as fizz rather than distortion. */
struct DriveSettings
{
    bool  enabled  = false;
    float drive    = 4.0f;  // how hard the signal is pushed into the shaper
    float tone     = 0.5f;  // 0..1, dark to bright, after the clipping
    float level    = 0.7f;  // make-up gain
    bool  hardClip = false;
    bool  cabinet  = true;

    bool operator==(const DriveSettings&) const = default;
};

/** A compressor pedal. Threshold and ratio are the shape; attack and release
    are the feel, and they mean what they say — see engine::Compressor. */
struct CompressorSettings
{
    bool  enabled     = false;
    float thresholdDb = -18.0f;
    float ratio       = 4.0f;
    float attackMs    = 10.0f;
    float releaseMs   = 120.0f;
    float makeUpDb    = 0.0f;

    bool operator==(const CompressorSettings&) const = default;
};

/** A tremolo pedal. `depth` is how far the quiet part drops, so zero is off. */
struct TremoloSettings
{
    bool  enabled = false;
    float rateHz  = 5.0f;
    float depth   = 0.5f;

    bool operator==(const TremoloSettings&) const = default;
};

/** A chorus pedal. `depth` is how far the delay sweeps, `mix` how much of the
    swept copies is heard — at zero depth it's a fixed comb rather than a
    chorus, which is a usable tone and not a broken one. */
struct ChorusSettings
{
    bool  enabled = false;
    float rateHz  = 0.6f;
    float depth   = 0.5f;
    float mix     = 0.5f;

    bool operator==(const ChorusSettings&) const = default;
};

/** A wobble pedal: a resonant low-pass swept by an LFO locked to the song's
    tempo, in beats rather than Hz — dubstep's "wub wub." `rateBeats` is how
    many beats one full sweep takes (0.25/0.5/1.0/2.0 for a sixteenth, an
    eighth, a quarter, a half note); `depth` is how far the sweep opens above
    `baseCutoffHz`, so depth zero leaves a static low-pass rather than muting
    anything. See engine::Wobble and PedalDsp.h. */
struct WobbleSettings
{
    bool  enabled      = false;
    float rateBeats    = 0.25f;
    float depth        = 0.7f;
    float baseCutoffHz = 200.0f;
    float resonance    = 0.9f;
    float mix          = 1.0f;

    bool operator==(const WobbleSettings&) const = default;
};

struct EffectSlot
{
    EffectKind kind    = EffectKind::Filter;
    bool       enabled = false;

    FilterSettings filter;
    DelaySettings  delay;
    ReverbSettings reverb;
    DriveSettings      drive;
    CompressorSettings compressor;
    TremoloSettings    tremolo;
    ChorusSettings     chorus;
    WobbleSettings     wobble;
    PluginRef          plugin; // meaningful when kind == Plugin

    bool operator==(const EffectSlot&) const = default;
};

} // namespace looper::model
