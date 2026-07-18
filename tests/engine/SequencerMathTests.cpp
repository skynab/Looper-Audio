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
