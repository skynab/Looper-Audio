#include <catch2/catch_test_macros.hpp>

#include <engine/DefaultContent.h>

using namespace looper::engine;

namespace
{
    // Restated rather than included from GuitarNode.h: that header pulls in
    // JUCE, and looper_tests deliberately doesn't link it (see
    // tests/CMakeLists.txt). DefaultContent.h is JUCE-free for the same
    // reason, which is exactly why makeDefaultGuitarRiffPattern takes the
    // open note as a parameter instead of reaching for kStandardTuning.
    constexpr int kStandardLowE = 40; // engine::kStandardTuning[0]
    constexpr int kHighestFret  = 24; // engine::kMaxFret

    int countAt(const Pattern& p, double beat, int note)
    {
        int n = 0;
        for (const auto& note_ : p.notes)
            if (note_.startBeats == beat && note_.noteNumber == note)
                ++n;
        return n;
    }
}

TEST_CASE("The default drum loop is one bar", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    REQUIRE(pattern.lengthBeats == 4.0);
}

TEST_CASE("The default drum loop puts the kick on 1 and 3", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    REQUIRE(countAt(pattern, 0.0, kDefaultKickNote) == 1);
    REQUIRE(countAt(pattern, 2.0, kDefaultKickNote) == 1);
    // Not also sounding where the snare is - a kick on every beat isn't the
    // pattern this claims to be.
    REQUIRE(countAt(pattern, 1.0, kDefaultKickNote) == 0);
    REQUIRE(countAt(pattern, 3.0, kDefaultKickNote) == 0);
}

TEST_CASE("The default drum loop puts the snare on 2 and 4", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    REQUIRE(countAt(pattern, 1.0, kDefaultSnareNote) == 1);
    REQUIRE(countAt(pattern, 3.0, kDefaultSnareNote) == 1);
}

TEST_CASE("The default drum loop plays a closed hat on every eighth note", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    int hats = 0;
    for (const auto& note : pattern.notes)
        if (note.noteNumber == kDefaultHatNote)
            ++hats;
    REQUIRE(hats == 8); // eight eighth-notes across one 4/4 bar

    for (double beat = 0.0; beat < 4.0; beat += 0.5)
        REQUIRE(countAt(pattern, beat, kDefaultHatNote) == 1);
}

TEST_CASE("Nothing in the default loop falls outside the bar", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultDrumLoopPattern();
    for (const auto& note : pattern.notes)
    {
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);
    }
}

TEST_CASE("The default guitar riff is one bar of eighth notes", "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultGuitarRiffPattern(kStandardLowE);
    REQUIRE(pattern.lengthBeats == 4.0);
    REQUIRE(pattern.notes.size() == 8);

    for (double beat = 0.0; beat < 4.0; beat += 0.5)
    {
        int atBeat = 0;
        for (const auto& note : pattern.notes)
            if (note.startBeats == beat)
                ++atBeat;
        INFO("beat " << beat);
        REQUIRE(atBeat == 1);
    }
}

TEST_CASE("The default guitar riff transposes with the tuning it's given", "[engine][defaultcontent]")
{
    // The property that makes the riff usable on a dropped tuning at all:
    // pitches are offsets from the low string, so handing it a different
    // open note moves the whole riff rather than leaving it unplayable.
    const auto standard = makeDefaultGuitarRiffPattern(kStandardLowE);
    const auto dropped  = makeDefaultGuitarRiffPattern(kStandardLowE - 4);

    REQUIRE(standard.notes.size() == dropped.notes.size());
    for (size_t i = 0; i < standard.notes.size(); ++i)
    {
        REQUIRE(dropped.notes[i].startBeats == standard.notes[i].startBeats);
        REQUIRE(dropped.notes[i].noteNumber == standard.notes[i].noteNumber - 4);
    }
}

TEST_CASE("Every note in the default guitar riff is reachable on the low string",
          "[engine][defaultcontent]")
{
    // A guitar sounds nothing it can't fret: below the open string there is
    // no fret at all, and past kHighestFret there's no neck. A riff that broke
    // this would simply be silent, which is the failure this catches.
    for (int lowString : { kStandardLowE, 36, 34 }) // standard E, drop C, drop B
    {
        const auto pattern = makeDefaultGuitarRiffPattern(lowString);
        INFO("low string " << lowString);
        for (const auto& note : pattern.notes)
        {
            REQUIRE(note.noteNumber >= lowString);
            REQUIRE(note.noteNumber <= lowString + kHighestFret);
        }
    }
}

TEST_CASE("Nothing in the default guitar riff falls outside the bar, or overlaps itself",
          "[engine][defaultcontent]")
{
    const auto pattern = makeDefaultGuitarRiffPattern(kStandardLowE);
    for (size_t i = 0; i < pattern.notes.size(); ++i)
    {
        const auto& note = pattern.notes[i];
        REQUIRE(note.startBeats >= 0.0);
        REQUIRE(note.startBeats + note.lengthBeats <= pattern.lengthBeats + 1.0e-9);

        // Repeated chugs are the same pitch back to back, so this is the one
        // pattern in the app most able to trip the same-pitch overlap that
        // clampNoteLengths exists for - see GenerativeLoop.h.
        for (size_t j = i + 1; j < pattern.notes.size(); ++j)
            if (pattern.notes[j].noteNumber == note.noteNumber)
                REQUIRE(pattern.notes[j].startBeats >= note.startBeats + note.lengthBeats - 1.0e-9);
    }
}
