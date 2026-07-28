#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/PianoRoll.h>

using namespace looper;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    constexpr float kWidth = 800.0f;
}

TEST_CASE("The playhead starts at the left of the grid, not the gutter", "[gui][pianoroll]")
{
    // The gutter holds the pitch names; beat zero is where the grid begins.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);

    roll.setPlayheadBeats(0.0, true);
    REQUIRE(roll.playheadXForTesting(kWidth) > 0.0f);
    REQUIRE(roll.playheadXForTesting(kWidth) < kWidth * 0.2f); // just past the gutter
}

TEST_CASE("The playhead advances across the grid with the beat", "[gui][pianoroll]")
{
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);

    roll.setPlayheadBeats(0.0, true);
    const float atStart = roll.playheadXForTesting(kWidth);

    roll.setPlayheadBeats(1.0, true);
    const float atOne = roll.playheadXForTesting(kWidth);

    roll.setPlayheadBeats(3.0, true);
    const float atThree = roll.playheadXForTesting(kWidth);

    REQUIRE(atOne > atStart);
    REQUIRE(atThree > atOne);
    REQUIRE(atThree <= kWidth);
}

TEST_CASE("The playhead never leaves the grid", "[gui][pianoroll]")
{
    // A clip loops, so the owner wraps the position — but a rounding error or
    // a pattern-length change between frames must not put the line outside
    // the grid, where it would read as a rendering fault.
    JuceFixture fixture;
    PianoRoll roll;
    roll.setSize((int) kWidth, 400);

    for (double beats : { -5.0, -0.001, 0.0, 4.0, 100.0 })
    {
        roll.setPlayheadBeats(beats, true);
        const float x = roll.playheadXForTesting(kWidth);
        INFO("beats " << beats << " -> x " << x);
        REQUIRE(x >= 0.0f);
        REQUIRE(x <= kWidth);
    }
}

TEST_CASE("The grid's bar length follows the time signature", "[gui][pianoroll]")
{
    // In 4/4 a bar is four beats and the grid marks every fourth; in 3/4 it
    // has to mark every third, or the heavy lines say the wrong thing.
    JuceFixture fixture;
    PianoRoll roll;

    REQUIRE(roll.beatsPerBarForTesting() == 4.0); // the default

    roll.setBeatsPerBar(3.0);
    REQUIRE(roll.beatsPerBarForTesting() == 3.0);

    // Nonsense is refused rather than dividing the grid by zero.
    roll.setBeatsPerBar(0.0);
    REQUIRE(roll.beatsPerBarForTesting() == 4.0);
}
