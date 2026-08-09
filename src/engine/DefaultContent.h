#pragma once

#include "engine/Pattern.h"

namespace looper::engine
{
/** The GM-ish note numbers model::makeDefaultDrumKit() assigns its four
    starting pads — kept here, not just there, because the loop below has to
    agree with them exactly or it plays the wrong pad. */
inline constexpr int kDefaultKickNote  = 36;
inline constexpr int kDefaultSnareNote = 38;
inline constexpr int kDefaultHatNote   = 42;

/**
    A one-bar starter beat: kick on 1 and 3, snare on 2 and 4, closed hats on
    every eighth note — about as generic a 4/4 pattern as exists, which is
    the point. Not meant to be interesting; meant to be recognizable enough
    on first launch that a new drum track visibly (and audibly) does
    something, rather than opening on a silent, empty grid.

    JUCE-free, like Pattern itself, so the shape of the beat — which beats
    carry which drum — is a headless, testable claim rather than something
    only checkable by ear.
*/
inline Pattern makeDefaultDrumLoopPattern()
{
    Pattern pattern;
    pattern.lengthBeats = 4.0;

    auto add = [&](double beat, int note, float velocity)
    {
        pattern.notes.push_back({ beat, 0.25, note, velocity });
    };

    add(0.0, kDefaultKickNote, 0.95f);
    add(2.0, kDefaultKickNote, 0.9f);

    add(1.0, kDefaultSnareNote, 0.85f);
    add(3.0, kDefaultSnareNote, 0.85f);

    for (double beat = 0.0; beat < 4.0; beat += 0.5)
        add(beat, kDefaultHatNote, 0.6f);

    return pattern;
}

/**
    A one-bar starter riff: straight eighth-note chugs on the lowest string,
    with the root leaving for a minor third and a fourth at the end of the
    bar so it's a phrase rather than a metronome. The guitar equivalent of
    makeDefaultDrumLoopPattern, and there for the same reason — a fresh
    launch should make the guitar track *audibly* do something.

    @p lowStringNote is the open pitch of the lowest string, and is required
    rather than defaulted on purpose: a guitar can only sound a note some
    string can actually reach, so a riff written for standard tuning is
    silent on a dropped one (it lands below every open string) and vice
    versa. Taking the real tuning is the same call DrumLoopParams makes in
    asking for the target kit's actual pad notes. Every pitch below is an
    offset from it, so the riff transposes with the tuning instead of
    breaking against it.
*/
inline Pattern makeDefaultGuitarRiffPattern(int lowStringNote)
{
    Pattern pattern;
    pattern.lengthBeats = 4.0;

    // Short relative to the eighth-note spacing, which is what makes this
    // read as palm-muted chugging rather than as held notes.
    auto add = [&](double beat, int semitonesAboveOpen, float velocity)
    {
        pattern.notes.push_back({ beat, 0.3, lowStringNote + semitonesAboveOpen, velocity });
    };

    add(0.0, 0, 1.0f);  // the downbeat, hit hardest
    add(0.5, 0, 0.8f);
    add(1.0, 0, 0.85f);
    add(1.5, 0, 0.8f);
    add(2.0, 0, 0.95f);
    add(2.5, 0, 0.8f);
    add(3.0, 3, 0.9f);  // minor third
    add(3.5, 5, 0.9f);  // fourth, leading back to the root

    return pattern;
}

} // namespace looper::engine
