#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/MidiNote.h>

using Catch::Approx;
using looper::engine::midiNoteToHertz;

TEST_CASE("midiNoteToHertz maps reference pitches", "[engine][midi]")
{
    REQUIRE(midiNoteToHertz(69) == Approx(440.0));            // A4
    REQUIRE(midiNoteToHertz(57) == Approx(220.0));            // A3, one octave down
    REQUIRE(midiNoteToHertz(81) == Approx(880.0));            // A5, one octave up
    REQUIRE(midiNoteToHertz(60) == Approx(261.6255653).epsilon(0.0001)); // middle C
    REQUIRE(midiNoteToHertz(72) == Approx(523.2511306).epsilon(0.0001)); // C5
}

TEST_CASE("midiNoteToHertz honours a custom concert pitch", "[engine][midi]")
{
    REQUIRE(midiNoteToHertz(69, 432.0) == Approx(432.0));
    REQUIRE(midiNoteToHertz(81, 432.0) == Approx(864.0));
}
