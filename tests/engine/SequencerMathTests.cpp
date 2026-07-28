#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/SequencerMath.h>

using Catch::Approx;
using looper::engine::edgeInBlock;
using looper::engine::wrapPositive;

TEST_CASE("wrapPositive keeps results within the pattern length", "[engine][seq]")
{
    REQUIRE(wrapPositive(5.0, 4.0)  == Approx(1.0));
    REQUIRE(wrapPositive(-1.0, 4.0) == Approx(3.0));
    REQUIRE(wrapPositive(0.0, 4.0)  == Approx(0.0));
    REQUIRE(wrapPositive(8.0, 4.0)  == Approx(0.0));
    REQUIRE(wrapPositive(3.5, 4.0)  == Approx(3.5));
}

TEST_CASE("edgeInBlock detects an edge inside the block", "[engine][seq]")
{
    int offset = -1;
    REQUIRE(edgeInBlock(50.0, 0.0, 1000.0, 100, offset));
    REQUIRE(offset == 50);

    REQUIRE(edgeInBlock(0.0, 0.0, 1000.0, 100, offset));
    REQUIRE(offset == 0);
}

TEST_CASE("edgeInBlock rejects an edge outside the block", "[engine][seq]")
{
    int offset = -1;
    REQUIRE_FALSE(edgeInBlock(150.0, 0.0, 1000.0, 100, offset));
    REQUIRE_FALSE(edgeInBlock(999.0, 0.0, 1000.0, 100, offset));
}

TEST_CASE("edgeInBlock handles pattern wrap-around", "[engine][seq]")
{
    int offset = -1;

    // Block that runs off the end of the pattern.
    REQUIRE(edgeInBlock(990.0, 950.0, 1000.0, 100, offset));
    REQUIRE(offset == 40);

    // Edge at time 30 with the block starting at 950 wraps: (30-950) mod 1000 = 80.
    REQUIRE(edgeInBlock(30.0, 950.0, 1000.0, 100, offset));
    REQUIRE(offset == 80);
}

TEST_CASE("A loop runs over what has been arranged", "[engine][loop]")
{
    // The loop used to be a hardcoded four bars whatever the song held, so
    // arranging anything longer silently looped only its opening.
    REQUIRE(looper::engine::loopEndForContent(16.0, 4.0) == 16.0); // exactly four bars
    REQUIRE(looper::engine::loopEndForContent(13.0, 4.0) == 16.0); // rounded up to the bar
    REQUIRE(looper::engine::loopEndForContent(0.5, 4.0)  == 4.0);
}

TEST_CASE("An empty song still gets a loop of one bar", "[engine][loop]")
{
    // A zero-length loop region would stall the transport or divide by zero
    // downstream, and an empty song is exactly when that would happen.
    REQUIRE(looper::engine::loopEndForContent(0.0, 4.0) == 4.0);
    REQUIRE(looper::engine::loopEndForContent(-5.0, 4.0) == 4.0);
    REQUIRE(looper::engine::loopEndForContent(0.0, 3.0) == 3.0); // and in 3/4
}

TEST_CASE("The loop end follows the time signature", "[engine][loop]")
{
    REQUIRE(looper::engine::loopEndForContent(10.0, 3.0) == 12.0); // 3/4: four bars
    REQUIRE(looper::engine::loopEndForContent(10.0, 5.0) == 10.0); // 5/4: two bars exactly
}

TEST_CASE("A nonsense bar length falls back rather than dividing by zero", "[engine][loop]")
{
    REQUIRE(looper::engine::loopEndForContent(10.0, 0.0) == 12.0); // treated as 4/4
    REQUIRE(std::isfinite(looper::engine::loopEndForContent(10.0, -1.0)));
}
