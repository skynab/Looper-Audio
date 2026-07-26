#pragma once

namespace looper::model
{
/**
    Per-track synth timbre for an Instrument track (model::Track::synthSettings)
    — the model-side counterpart to engine::SynthInstrumentNode's per-voice
    oscillator/filter/envelope, the same way FilterSettings/DelaySettings are
    the model-side counterpart to the master FilterEffect/DelayEffect.

    waveform: 0 = sine, 1 = saw, 2 = square, 3 = triangle (a plain int, not a
    shared enum with the engine, matching how FilterSettings::mode already
    documents its meaning by comment rather than a shared type).

    The filter here shapes this track's own voices before they're mixed in —
    distinct from FilterSettings, which shapes the whole master bus.
*/
struct SynthSettings
{
    int   waveform = 0;

    float attackMs  = 5.0f;
    float decayMs   = 120.0f;
    float sustain   = 0.7f;  // 0..1
    float releaseMs = 250.0f;

    bool  filterEnabled = false;
    int   filterMode    = 0; // 0 = low-pass, 1 = high-pass, 2 = band-pass
    float filterCutoff    = 1000.0f; // Hz
    float filterResonance = 0.707f;

    float gainDb = 0.0f; // additional per-track output trim, on top of the track fader

    bool operator==(const SynthSettings&) const = default;
};

} // namespace looper::model
