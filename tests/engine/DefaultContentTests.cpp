#include <catch2/catch_test_macros.hpp>

#include <engine/DefaultContent.h>

using namespace looper::engine;

namespace
{
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
