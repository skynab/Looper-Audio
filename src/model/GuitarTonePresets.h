#pragma once

#include <vector>

#include "engine/GuitarTone.h"
#include "model/Effects.h"
#include "model/GuitarSettings.h"

namespace looper::model
{
/** A GuitarSettings/effect-chain bundle for one engine::GuitarTone. */
struct GuitarTonePreset
{
    GuitarSettings           guitar;
    std::vector<EffectSlot>  effectChain;
};

/**
    A guitar sound purpose-tuned for @p tone, for FretboardPane's one-click
    tone buttons — built the same in-memory, literal-struct-per-field way
    presetForGenre() builds a genre's SynthPreset (see GenrePresets.h): no
    file I/O, no save/load, just known-good values applied in one undo step.
*/
inline GuitarTonePreset presetForGuitarTone(engine::GuitarTone tone)
{
    GuitarTonePreset p;

    switch (tone)
    {
        case engine::GuitarTone::ModernMetal:
        {
            // Tight and percussive, not thin: the compressor feeds the drive
            // stage a consistent level (and adds sustain a bare limiter-style
            // drive wouldn't), the drive sits well up its range for real
            // high-gain saturation, and the gate cleans up the noise floor
            // that much gain raises. Drive's own make-up gain falls as
            // 1/sqrt(drive) (see DriveEffect::process), so `level` is pushed
            // to its ceiling to keep the driven tone from coming out quiet
            // and thin despite all that gain.
            p.guitar.decaySeconds  = 2.2f;
            p.guitar.brightness    = 0.75f;
            p.guitar.pickPosition  = 0.14f; // near the bridge - the classic metal pick attack
            p.guitar.pickHardness  = 0.85f;
            p.guitar.muteOnNoteOff = 0.55f; // palm-mute-style choke, not fully dead

            EffectSlot compressor;
            compressor.kind                  = EffectKind::Compressor;
            compressor.enabled               = true;
            compressor.compressor.enabled    = true;
            compressor.compressor.thresholdDb = -20.0f;
            compressor.compressor.ratio       = 4.0f;
            compressor.compressor.attackMs    = 8.0f;
            compressor.compressor.releaseMs   = 100.0f;
            compressor.compressor.makeUpDb    = 6.0f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 65.0f;
            drive.drive.tone     = 0.58f;
            drive.drive.level    = 2.0f; // ceiling - see comment above
            drive.drive.hardClip = true;
            drive.drive.cabinet  = true;

            EffectSlot gate;
            gate.kind             = EffectKind::Gate;
            gate.enabled          = true;
            gate.gate.enabled     = true;
            gate.gate.thresholdDb = -32.0f;
            gate.gate.rangeDb     = 60.0f;
            gate.gate.attackMs    = 0.5f;
            gate.gate.holdMs      = 15.0f;
            gate.gate.releaseMs   = 55.0f;

            p.effectChain = { compressor, drive, gate };
            break;
        }
        case engine::GuitarTone::CleanJazz:
        {
            // Warm, long-ringing, no drive - articulate rather than loud.
            p.guitar.decaySeconds  = 3.5f;
            p.guitar.brightness    = 0.35f;
            p.guitar.pickPosition  = 0.35f; // toward the neck - round, warm attack
            p.guitar.pickHardness  = 0.25f; // fingertip-soft
            p.guitar.muteOnNoteOff = 0.0f;  // let it ring

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -24.0f;
            compressor.compressor.ratio       = 2.5f;
            compressor.compressor.attackMs    = 15.0f;
            compressor.compressor.releaseMs   = 150.0f;
            compressor.compressor.makeUpDb    = 2.0f;

            EffectSlot reverb;
            reverb.kind            = EffectKind::Reverb;
            reverb.enabled         = true;
            reverb.reverb.enabled  = true;
            reverb.reverb.roomSize = 0.3f;
            reverb.reverb.damping  = 0.6f;
            reverb.reverb.mix      = 0.18f;

            p.effectChain = { compressor, reverb };
            break;
        }
        case engine::GuitarTone::ClassicRockCrunch:
        {
            // Warm tube-like overdrive, not a wall of gain - dynamic enough
            // that pick attack still comes through.
            p.guitar.decaySeconds  = 2.6f;
            p.guitar.brightness    = 0.55f;
            p.guitar.pickPosition  = 0.22f;
            p.guitar.pickHardness  = 0.65f;
            p.guitar.muteOnNoteOff = 0.15f;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -22.0f;
            compressor.compressor.ratio       = 3.0f;
            compressor.compressor.attackMs    = 10.0f;
            compressor.compressor.releaseMs   = 120.0f;
            compressor.compressor.makeUpDb    = 3.0f;

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 22.0f;
            drive.drive.tone     = 0.5f;
            drive.drive.level    = 1.0f;
            drive.drive.hardClip = false; // soft clip - the tube-y crunch
            drive.drive.cabinet  = true;

            p.effectChain = { compressor, drive };
            break;
        }
        case engine::GuitarTone::AmbientShoegaze:
        {
            // Long, swelling sustain under a heavy chorus/reverb wash.
            p.guitar.decaySeconds  = 6.0f;
            p.guitar.brightness    = 0.6f;
            p.guitar.pickPosition  = 0.3f;
            p.guitar.pickHardness  = 0.4f;
            p.guitar.muteOnNoteOff = 0.0f;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.3f;
            chorus.chorus.depth   = 0.8f;
            chorus.chorus.mix     = 0.6f;

            EffectSlot reverb;
            reverb.kind            = EffectKind::Reverb;
            reverb.enabled         = true;
            reverb.reverb.enabled  = true;
            reverb.reverb.roomSize = 0.9f;
            reverb.reverb.damping  = 0.3f;
            reverb.reverb.mix      = 0.5f;

            p.effectChain = { chorus, reverb };
            break;
        }
        case engine::GuitarTone::FunkPercussive:
        {
            // Tight, choked, minimal sustain - the chord is a percussion hit.
            p.guitar.decaySeconds  = 0.5f;
            p.guitar.brightness    = 0.65f;
            p.guitar.pickPosition  = 0.18f;
            p.guitar.pickHardness  = 0.7f;
            p.guitar.muteOnNoteOff = 0.85f;

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -18.0f;
            compressor.compressor.ratio       = 6.0f;
            compressor.compressor.attackMs    = 2.0f;
            compressor.compressor.releaseMs   = 60.0f;
            compressor.compressor.makeUpDb    = 4.0f;

            p.effectChain = { compressor };
            break;
        }
    }

    return p;
}

} // namespace looper::model
