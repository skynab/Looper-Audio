#include <catch2/catch_test_macros.hpp>

#include <engine/GuitarChords.h>

#include <stdexcept>
#include <string>

using namespace looper::engine;

namespace
{
    constexpr int kStandard[6] = { 40, 45, 50, 55, 59, 64 }; // E2 A2 D3 G3 B3 E4

    const ChordShape& shapeNamed(const char* name)
    {
        for (const auto& shape : kChordShapes)
            if (std::string(shape.name) == name)
                return shape;
        throw std::runtime_error("no such shape");
    }
}

TEST_CASE("An open E is the notes a guitarist would play", "[engine][chords]")
{
    // 0-2-2-1-0-0 in standard tuning: E B E G# B E. Getting this wrong would
    // be a chord that's *nearly* right, which is harder to notice than one
    // that's obviously broken.
    const auto notes = GuitarChords::notesForShape(shapeNamed("E"), kStandard);
    REQUIRE(notes == std::vector<int> { 40, 47, 52, 56, 59, 64 });
}

TEST_CASE("Unplayed strings are skipped, not silently sounded", "[engine][chords]")
{
    // A major is x-0-2-2-2-0: the low E isn't struck at all. A chord voicing
    // that quietly added it would be a different chord.
    const auto notes = GuitarChords::notesForShape(shapeNamed("A"), kStandard);
    REQUIRE(notes.size() == 5);
    REQUIRE(notes.front() == 45); // starts on the open A, not the low E

    const auto d = GuitarChords::notesForShape(shapeNamed("D"), kStandard);
    REQUIRE(d == std::vector<int> { 50, 57, 62, 66 });
}

TEST_CASE("Every shape produces at least a triad", "[engine][chords]")
{
    for (const auto& shape : kChordShapes)
    {
        const auto notes = GuitarChords::notesForShape(shape, kStandard);
        INFO("shape " << shape.name);
        REQUIRE(notes.size() >= 3);
        for (int note : notes)
            REQUIRE((note >= 0 && note <= 127));
    }
}

TEST_CASE("A fret offset moves the shape up the neck", "[engine][chords]")
{
    // The E shape barred at fret 3 is a G — every fretted note up three
    // semitones. Open strings move too, since a barre replaces the nut.
    const auto open   = GuitarChords::notesForShape(shapeNamed("E"), kStandard, 0);
    const auto barred = GuitarChords::notesForShape(shapeNamed("E"), kStandard, 3);

    REQUIRE(open.size() == barred.size());
    for (size_t i = 0; i < open.size(); ++i)
        REQUIRE(barred[i] == open[i] + 3);
}

TEST_CASE("A downstroke strikes low to high, an upstroke high to low", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 20.0;

    strum.downstroke = true;
    const auto down = GuitarChords::strumChord(shapeNamed("E"), kStandard, 0, 0.0, 1.0, 120.0, strum);
    REQUIRE(down.front().noteNumber == 40); // low E first
    REQUIRE(down.back().noteNumber == 64);

    strum.downstroke = false;
    const auto up = GuitarChords::strumChord(shapeNamed("E"), kStandard, 0, 0.0, 1.0, 120.0, strum);
    REQUIRE(up.front().noteNumber == 64); // high E first
    REQUIRE(up.back().noteNumber == 40);
}

TEST_CASE("Strummed notes are staggered, not simultaneous", "[engine][chords]")
{
    // The whole point: six notes at the same instant read as an organ.
    StrumSettings strum;
    strum.spreadMs = 24.0;

    const auto notes = GuitarChords::strumChord(shapeNamed("E"), kStandard, 0, 0.0, 1.0, 120.0, strum);
    REQUIRE(notes.size() == 6);

    for (size_t i = 1; i < notes.size(); ++i)
        REQUIRE(notes[i].startBeats > notes[i - 1].startBeats);

    // 24ms at 120bpm is 0.048 beats from first string to last.
    const double span = notes.back().startBeats - notes.front().startBeats;
    REQUIRE(std::abs(span - 0.048) < 0.002);
}

TEST_CASE("A zero spread is a simultaneous chord", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 0.0;

    const auto notes = GuitarChords::strumChord(shapeNamed("G"), kStandard, 0, 2.0, 1.0, 120.0, strum);
    for (const auto& note : notes)
        REQUIRE(std::abs(note.startBeats - 2.0) < 1.0e-9);
}

TEST_CASE("Humanising never reorders the strokes", "[engine][chords]")
{
    // Jitter is capped below half the gap between strings precisely so this
    // holds: a hand moving across the strings cannot hit the fourth before
    // the third, however loose its timing.
    StrumSettings strum;
    strum.spreadMs = 15.0;
    strum.humanise = 1.0;

    for (uint32_t seed = 1; seed <= 50; ++seed)
    {
        const auto notes = GuitarChords::strumChord(shapeNamed("C"), kStandard, 0, 1.0, 1.0, 140.0, strum, seed);
        INFO("seed " << seed);
        for (size_t i = 1; i < notes.size(); ++i)
            REQUIRE(notes[i].startBeats > notes[i - 1].startBeats);
    }
}

TEST_CASE("Humanising varies the timing it is given", "[engine][chords]")
{
    // ...but it must actually do something, or the control is decorative.
    StrumSettings straight;
    straight.spreadMs = 15.0;
    straight.humanise = 0.0;

    StrumSettings loose = straight;
    loose.humanise = 1.0;

    const auto exact    = GuitarChords::strumChord(shapeNamed("C"), kStandard, 0, 0.0, 1.0, 120.0, straight, 7);
    const auto humanised = GuitarChords::strumChord(shapeNamed("C"), kStandard, 0, 0.0, 1.0, 120.0, loose, 7);

    bool anyDifferent = false;
    for (size_t i = 0; i < exact.size(); ++i)
        if (std::abs(exact[i].startBeats - humanised[i].startBeats) > 1.0e-9)
            anyDifferent = true;

    REQUIRE(anyDifferent);
}

TEST_CASE("A strum never places a note before the bar it was asked for", "[engine][chords]")
{
    StrumSettings strum;
    strum.spreadMs = 30.0;
    strum.humanise = 1.0;

    for (uint32_t seed = 1; seed <= 20; ++seed)
    {
        const auto notes = GuitarChords::strumChord(shapeNamed("Am"), kStandard, 0, 0.0, 1.0, 120.0, strum, seed);
        for (const auto& note : notes)
            REQUIRE(note.startBeats >= 0.0);
    }
}
