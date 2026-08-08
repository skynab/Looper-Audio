#include <catch2/catch_test_macros.hpp>

#include <engine/GenerativeLoop.h>

using namespace looper::engine;

// --- euclideanRhythm ---------------------------------------------------

TEST_CASE("euclideanRhythm produces exactly the requested number of onsets", "[engine][generativeloop]")
{
    for (int steps : { 1, 4, 8, 13, 16 })
    {
        for (int pulses = 0; pulses <= steps; ++pulses)
        {
            const auto pattern = euclideanRhythm(steps, pulses);
            int        count   = 0;
            for (bool onset : pattern)
                count += onset ? 1 : 0;
            INFO("steps=" << steps << " pulses=" << pulses);
            REQUIRE(count == pulses);
        }
    }
}

TEST_CASE("euclideanRhythm is deterministic", "[engine][generativeloop]")
{
    REQUIRE(euclideanRhythm(16, 5, 3) == euclideanRhythm(16, 5, 3));
}

TEST_CASE("euclideanRhythm's rotation is a cyclic shift of the unrotated pattern", "[engine][generativeloop]")
{
    const auto base = euclideanRhythm(16, 5, 0);
    for (int rotation = 0; rotation < 16; ++rotation)
    {
        const auto rotated = euclideanRhythm(16, 5, rotation);
        for (int i = 0; i < 16; ++i)
            REQUIRE(rotated[(size_t) i] == base[(size_t) ((i + rotation) % 16)]);
    }
}

TEST_CASE("euclideanRhythm(3, 8) is the tresillo", "[engine][generativeloop]")
{
    // A concrete, hand-checked case (not recalled from memory): with the
    // floor-division onset rule, onsets land at i=0, i=3 and i=6 - the
    // canonical "X..X..X." tresillo.
    const auto pattern = euclideanRhythm(8, 3);
    const std::vector<bool> expected { true, false, false, true, false, false, true, false };
    REQUIRE(pattern == expected);
}

TEST_CASE("euclideanRhythm handles the edges: zero pulses and every step filled", "[engine][generativeloop]")
{
    for (bool b : euclideanRhythm(8, 0))
        REQUIRE_FALSE(b);
    for (bool b : euclideanRhythm(8, 8))
        REQUIRE(b);
}

// --- generateDrumLoop ----------------------------------------------------

namespace
{
    bool onlyUsesNotes(const Pattern& p, int a, int b, int c)
    {
        for (const auto& note : p.notes)
            if (note.noteNumber != a && note.noteNumber != b && note.noteNumber != c)
                return false;
        return true;
    }

    /** True if any two notes sharing a pitch overlap in time. Two notes of
        *different* pitches overlapping is normal polyphony; two notes of the
        *same* pitch overlapping is what PatternPlayback can't represent
        cleanly (see clampNoteLengths's doc comment) and is exactly the "note
        hangs, then buzzes" bug this guards against. */
    bool hasSamePitchOverlap(const Pattern& p)
    {
        for (size_t i = 0; i < p.notes.size(); ++i)
        {
            const double aEnd = p.notes[i].startBeats + p.notes[i].lengthBeats;
            for (size_t j = i + 1; j < p.notes.size(); ++j)
            {
                if (p.notes[j].noteNumber != p.notes[i].noteNumber)
                    continue;
                const double bEnd = p.notes[j].startBeats + p.notes[j].lengthBeats;
                const bool overlaps = p.notes[i].startBeats < bEnd - 1.0e-9
                                   && p.notes[j].startBeats < aEnd - 1.0e-9;
                if (overlaps)
                    return true;
            }
        }
        return false;
    }
}

TEST_CASE("generateDrumLoop only uses the caller's three note numbers", "[engine][generativeloop]")
{
    DrumLoopParams params;
    params.kickNote  = 36;
    params.snareNote = 38;
    params.hatNote   = 42;
    const auto pattern = generateDrumLoop(params);
    REQUIRE(onlyUsesNotes(pattern, 36, 38, 42));
}

TEST_CASE("generateDrumLoop's pattern length matches bars", "[engine][generativeloop]")
{
    DrumLoopParams params;
    params.bars = 2;
    const auto pattern = generateDrumLoop(params);
    REQUIRE(pattern.lengthBeats == 8.0);
}

TEST_CASE("generateDrumLoop keeps every note inside the pattern", "[engine][generativeloop]")
{
    DrumLoopParams params;
    params.bars = 2;
    const auto pattern = generateDrumLoop(params);
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
    }
}

TEST_CASE("generateDrumLoop's snare lands on the backbeat", "[engine][generativeloop]")
{
    DrumLoopParams params;
    int snareHits = 0;
    for (const auto& note : generateDrumLoop(params).notes)
        if (note.noteNumber == params.snareNote)
        {
            REQUIRE((note.startBeats == 1.0 || note.startBeats == 3.0));
            ++snareHits;
        }
    REQUIRE(snareHits == 2);
}

TEST_CASE("generateDrumLoop is deterministic for a fixed seed", "[engine][generativeloop]")
{
    DrumLoopParams params;
    params.seed = 42;
    REQUIRE(generateDrumLoop(params).notes == generateDrumLoop(params).notes);
}

TEST_CASE("generateDrumLoop's seed changes the output", "[engine][generativeloop]")
{
    DrumLoopParams a;
    a.seed = 1;
    DrumLoopParams b = a;
    b.seed = 2;
    REQUIRE_FALSE(generateDrumLoop(a).notes == generateDrumLoop(b).notes);
}

TEST_CASE("generateDrumLoop's note count grows with density", "[engine][generativeloop]")
{
    DrumLoopParams low;
    low.seed    = 7;
    low.density = 0.1;
    DrumLoopParams high = low;
    high.density         = 0.9;

    REQUIRE(generateDrumLoop(high).notes.size() > generateDrumLoop(low).notes.size());
}

TEST_CASE("generateDrumLoop's swing changes the pattern but leaves the kick's downbeat alone",
          "[engine][generativeloop]")
{
    // Step 0 is even, so NoteOps::quantizeNotes never moves it - the kick's
    // rotation-0 Euclidean rhythm always has an onset there (see
    // generateDrumLoop's own doc comment), so it must survive any swing
    // amount exactly.
    DrumLoopParams straight;
    straight.seed    = 3;
    straight.density = 0.9; // busy enough that some hat hit lands on an odd step
    DrumLoopParams swung = straight;
    swung.swing           = 0.5;

    const auto straightPattern = generateDrumLoop(straight);
    const auto swungPattern    = generateDrumLoop(swung);
    REQUIRE_FALSE(straightPattern.notes == swungPattern.notes);

    auto firstKickBeat = [&](const Pattern& p)
    {
        double best = -1.0;
        for (const auto& note : p.notes)
            if (note.noteNumber == straight.kickNote && (best < 0.0 || note.startBeats < best))
                best = note.startBeats;
        return best;
    };
    REQUIRE(firstKickBeat(straightPattern) == 0.0);
    REQUIRE(firstKickBeat(swungPattern) == 0.0);
}

TEST_CASE("generateDrumLoop's swing keeps every note inside the pattern", "[engine][generativeloop]")
{
    DrumLoopParams params;
    params.bars  = 2;
    params.swing = 0.6;
    const auto pattern = generateDrumLoop(params);
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
    }
}

TEST_CASE("generateDrumLoop never overlaps two hits on the same pad, at any swing or density",
          "[engine][generativeloop]")
{
    // A swung hit's length was sized before the swing shift; a busy voice
    // (small gaps between its own hits) plus heavy swing is exactly the
    // combination that used to push one hit into the next one on the same
    // pad - this is the regression test for that bug.
    for (unsigned seed = 1; seed <= 20; ++seed)
    {
        DrumLoopParams params;
        params.seed    = seed;
        params.density = 0.95;
        params.swing   = 0.85;
        const auto pattern = generateDrumLoop(params);
        INFO("seed " << seed);
        REQUIRE_FALSE(hasSamePitchOverlap(pattern));
    }
}

// --- generateMelodicLoop --------------------------------------------------

TEST_CASE("generateMelodicLoop's notes are all in the requested scale", "[engine][generativeloop]")
{
    MelodicLoopParams params;
    params.scale = Scale { ScaleType::Dorian, 62 };
    params.bars  = 2;
    const auto pattern = generateMelodicLoop(params);
    REQUIRE_FALSE(pattern.notes.empty());
    for (const auto& note : pattern.notes)
        REQUIRE(isInScale(note.noteNumber, params.scale));
}

TEST_CASE("generateMelodicLoop keeps every note inside the pattern", "[engine][generativeloop]")
{
    MelodicLoopParams params;
    params.bars = 2;
    const auto pattern = generateMelodicLoop(params);
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
    }
}

TEST_CASE("generateMelodicLoop is deterministic for a fixed seed", "[engine][generativeloop]")
{
    MelodicLoopParams params;
    params.seed = 99;
    REQUIRE(generateMelodicLoop(params).notes == generateMelodicLoop(params).notes);
}

TEST_CASE("generateMelodicLoop's seed changes the output", "[engine][generativeloop]")
{
    MelodicLoopParams a;
    a.seed = 1;
    MelodicLoopParams b = a;
    b.seed = 2;
    REQUIRE_FALSE(generateMelodicLoop(a).notes == generateMelodicLoop(b).notes);
}

TEST_CASE("generateMelodicLoop's note count grows with density", "[engine][generativeloop]")
{
    MelodicLoopParams low;
    low.seed    = 7;
    low.density = 0.1;
    MelodicLoopParams high = low;
    high.density            = 0.9;

    REQUIRE(generateMelodicLoop(high).notes.size() > generateMelodicLoop(low).notes.size());
}

TEST_CASE("generateMelodicLoop's swing changes the pattern but leaves the first note's onset alone",
          "[engine][generativeloop]")
{
    MelodicLoopParams straight;
    straight.seed    = 3;
    straight.density = 0.9;
    MelodicLoopParams swung = straight;
    swung.swing              = 0.5;

    const auto straightPattern = generateMelodicLoop(straight);
    const auto swungPattern    = generateMelodicLoop(swung);
    REQUIRE_FALSE(straightPattern.notes.empty());
    REQUIRE_FALSE(swungPattern.notes.empty());
    REQUIRE_FALSE(straightPattern.notes == swungPattern.notes);

    // The first onset is always step 0 (even, so swing never moves it) -
    // the same guarantee generateDrumLoop's kick relies on.
    REQUIRE(straightPattern.notes.front().startBeats == 0.0);
    REQUIRE(swungPattern.notes.front().startBeats == 0.0);
}

TEST_CASE("generateMelodicLoop's swing keeps every note inside the pattern", "[engine][generativeloop]")
{
    MelodicLoopParams params;
    params.bars  = 2;
    params.swing = 0.6;
    const auto pattern = generateMelodicLoop(params);
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
    }
}

TEST_CASE("generateMelodicLoop never overlaps two notes on the same scale degree, at any swing or density",
          "[engine][generativeloop]")
{
    // The random walk revisits the same degree (and therefore the same
    // pitch) often - combined with heavy swing shifting a note later
    // without shortening it, this used to overlap into the next note on
    // that same pitch. Regression test for that bug.
    for (unsigned seed = 1; seed <= 20; ++seed)
    {
        MelodicLoopParams params;
        params.seed    = seed;
        params.density = 0.95;
        params.swing   = 0.85;
        const auto pattern = generateMelodicLoop(params);
        INFO("seed " << seed);
        REQUIRE_FALSE(hasSamePitchOverlap(pattern));
    }
}
