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
            // Drop C (C2 G2 C3 F3 A3 D4). The growl in this genre is mostly
            // the tuning: dropped low strings put the fundamentals under the
            // cabinet's 90Hz corner, so what's actually heard is the *low-mid
            // harmonics* the distortion generates from them, which is the
            // sound. Standard E through the same chain reads as "distorted
            // rock guitar", not as this.
            p.guitar.tuning = { 36, 43, 48, 53, 57, 62 };

            p.guitar.decaySeconds  = 2.0f;
            // Deliberately a dark string into a saturated amp, which is the
            // real topology: a humbucker is dark, and essentially all of the
            // perceived brightness in this genre comes from the amp and cab
            // downstream. Measured - see looper_bounce's `growl` figure,
            // which is what these two values were actually tuned against.
            p.guitar.brightness    = 0.38f;
            p.guitar.pickPosition  = 0.24f;
            p.guitar.pickHardness  = 0.85f;
            p.guitar.muteOnNoteOff = 0.55f; // palm-mute-style choke, not fully dead

            EffectSlot compressor;
            compressor.kind                   = EffectKind::Compressor;
            compressor.enabled                = true;
            compressor.compressor.enabled     = true;
            compressor.compressor.thresholdDb = -20.0f;
            compressor.compressor.ratio       = 4.0f;
            compressor.compressor.attackMs    = 8.0f;
            compressor.compressor.releaseMs   = 100.0f;
            compressor.compressor.makeUpDb    = 6.0f;

            // Two cascaded gain stages rather than one hard clipper, which is
            // both the real rig (a mid-focused boost pedal into a high-gain
            // amp) and the reason this growls instead of buzzing: a single
            // hard clip at high drive is a square wave, heard as fizz, while
            // stacked soft (tanh) stages saturate progressively and stay
            // harmonically dense in the low-mids.
            //
            // Cabinet is off on the boost and on for the amp, matching the
            // same rig - a boost pedal has no speaker, and cascading two
            // CabinetSims would put four poles of 4.5kHz lowpass in the path
            // and just sound muffled.
            EffectSlot boost;
            boost.kind           = EffectKind::Drive;
            boost.enabled        = true;
            boost.drive.enabled  = true;
            boost.drive.drive    = 10.0f;
            boost.drive.tone     = 0.5f;
            boost.drive.level    = 1.6f;
            boost.drive.hardClip = false;
            boost.drive.cabinet  = false;

            EffectSlot amp;
            amp.kind           = EffectKind::Drive;
            amp.enabled        = true;
            amp.drive.enabled  = true;
            amp.drive.drive    = 75.0f;
            // Darker than halfway: DriveEffect's post-tilt scales everything
            // above ~900Hz by (0.25 + 1.75*tone), so pulling tone down is
            // what leaves the low-mid growl in front of the fizz.
            amp.drive.tone     = 0.26f;
            amp.drive.level    = 2.0f; // ceiling - make-up gain falls as 1/sqrt(drive)
            amp.drive.hardClip = false;
            amp.drive.cabinet  = true;

            EffectSlot gate;
            gate.kind             = EffectKind::Gate;
            gate.enabled          = true;
            gate.gate.enabled     = true;
            gate.gate.thresholdDb = -32.0f;
            gate.gate.rangeDb     = 60.0f;
            gate.gate.attackMs    = 0.5f;
            gate.gate.holdMs      = 15.0f;
            gate.gate.releaseMs   = 55.0f;

            p.effectChain = { compressor, boost, amp, gate };
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
            p.guitar.brightness    = 0.38f;
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
        case engine::GuitarTone::IndustrialCyber:
        {
            // Cold and mechanical rather than heavy: drop C like the metal
            // tone, but where that one is tuned for low-mid growl this is
            // deliberately thinner and more brittle - a hard clip (the fizz
            // Modern Metal avoids is the point here), a bright tone, and a
            // gate set to slam shut so notes end abruptly rather than
            // decaying. Chorus and a short delay do the "cyber" shimmer.
            p.guitar.tuning = { 36, 43, 48, 53, 57, 62 };

            p.guitar.decaySeconds  = 1.2f;  // short - this tone doesn't sustain
            p.guitar.brightness    = 0.8f;  // brittle and present, not warm
            p.guitar.pickPosition  = 0.1f;  // hard at the bridge - thin and glassy
            p.guitar.pickHardness  = 0.95f;
            p.guitar.muteOnNoteOff = 0.9f;  // notes stop dead, machine-like

            EffectSlot drive;
            drive.kind           = EffectKind::Drive;
            drive.enabled        = true;
            drive.drive.enabled  = true;
            drive.drive.drive    = 30.0f;
            drive.drive.tone     = 0.8f;  // bright, unlike Modern Metal's 0.26
            drive.drive.level    = 1.2f;
            drive.drive.hardClip = true;  // square-ish and buzzy on purpose
            drive.drive.cabinet  = true;

            EffectSlot chorus;
            chorus.kind           = EffectKind::Chorus;
            chorus.enabled        = true;
            chorus.chorus.enabled = true;
            chorus.chorus.rateHz  = 0.8f;
            chorus.chorus.depth   = 0.5f;
            chorus.chorus.mix     = 0.3f;

            EffectSlot delay;
            delay.kind           = EffectKind::Delay;
            delay.enabled        = true;
            delay.delay.enabled  = true;
            delay.delay.timeMs   = 250.0f;
            delay.delay.feedback = 0.3f;
            delay.delay.mix      = 0.22f;

            // Tighter and deeper than the metal gate: this tone is about
            // notes stopping, so the gate is part of the sound rather than
            // just noise control.
            EffectSlot gate;
            gate.kind             = EffectKind::Gate;
            gate.enabled          = true;
            gate.gate.enabled     = true;
            gate.gate.thresholdDb = -26.0f;
            gate.gate.rangeDb     = 70.0f;
            gate.gate.attackMs    = 0.3f;
            gate.gate.holdMs      = 8.0f;
            gate.gate.releaseMs   = 25.0f;

            p.effectChain = { drive, gate, chorus, delay };
            break;
        }
    }

    return p;
}

} // namespace looper::model
