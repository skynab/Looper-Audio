#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/TempoMap.h>

using Catch::Approx;
using looper::engine::TempoMap;

TEST_CASE("TempoMap converts samples <-> beats at 120 bpm / 48k", "[engine][tempo]")
{
    TempoMap t;
    t.setSampleRate(48000.0);
    t.setTempo(120.0);

    REQUIRE(t.samplesPerBeat() == Approx(24000.0));
    REQUIRE(t.ppqFromSamples(24000) == Approx(1.0));
    REQUIRE(t.ppqFromSamples(96000) == Approx(4.0));
    REQUIRE(t.samplesFromPpq(1.0) == 24000);
    REQUIRE(t.samplesFromPpq(4.0) == 96000);
}

TEST_CASE("TempoMap reports bars/beats in 4/4", "[engine][tempo]")
{
    TempoMap t;
    t.setSampleRate(48000.0);
    t.setTempo(120.0);
    t.setTimeSignature(4, 4);

    REQUIRE(t.quartersPerBar() == Approx(4.0));

    auto start = t.barsBeatsFromSamples(0);
    REQUIRE(start.bar == 1);
    REQUIRE(start.beat == 1);

    auto beat2 = t.barsBeatsFromSamples(24000); // one quarter note
    REQUIRE(beat2.bar == 1);
    REQUIRE(beat2.beat == 2);

    auto beat3 = t.barsBeatsFromSamples(48000);
    REQUIRE(beat3.bar == 1);
    REQUIRE(beat3.beat == 3);

    auto bar2 = t.barsBeatsFromSamples(96000); // four quarters
    REQUIRE(bar2.bar == 2);
    REQUIRE(bar2.beat == 1);
}

TEST_CASE("TempoMap respects the time signature denominator", "[engine][tempo]")
{
    TempoMap t;
    t.setSampleRate(48000.0);
    t.setTempo(120.0);
    t.setTimeSignature(6, 8);

    REQUIRE(t.quartersPerBar() == Approx(3.0)); // 6 eighths = 3 quarters

    auto eighth = t.barsBeatsFromSamples(12000); // half a quarter = one eighth
    REQUIRE(eighth.bar == 1);
    REQUIRE(eighth.beat == 2);

    auto nextBar = t.barsBeatsFromSamples(72000); // 3 quarters = one 6/8 bar
    REQUIRE(nextBar.bar == 2);
    REQUIRE(nextBar.beat == 1);
}
