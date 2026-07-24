#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <engine/DelayLine.h>

#include <vector>

using Catch::Approx;
using looper::engine::DelayLine;

TEST_CASE("DelayLine delays an impulse by the delay time", "[engine][delay]")
{
    DelayLine dl;
    dl.prepare(8);

    std::vector<float> in { 1, 0, 0, 0, 0, 0, 0, 0 };
    std::vector<float> out;
    for (float x : in)
        out.push_back(dl.processSample(x, 3, 0.0f));

    REQUIRE(out[0] == Approx(0.0f));
    REQUIRE(out[2] == Approx(0.0f));
    REQUIRE(out[3] == Approx(1.0f)); // impulse reappears 3 samples later
    REQUIRE(out[4] == Approx(0.0f));
}

TEST_CASE("DelayLine feedback produces decaying echoes", "[engine][delay]")
{
    DelayLine dl;
    dl.prepare(8);

    std::vector<float> in(8, 0.0f);
    in[0] = 1.0f;
    std::vector<float> out;
    for (float x : in)
        out.push_back(dl.processSample(x, 2, 0.5f));

    REQUIRE(out[2] == Approx(1.0f));  // first tap
    REQUIRE(out[4] == Approx(0.5f));  // one feedback round
    REQUIRE(out[6] == Approx(0.25f)); // two feedback rounds
}

TEST_CASE("DelayLine clamps an over-long delay to its buffer", "[engine][delay]")
{
    DelayLine dl;
    dl.prepare(4);
    REQUIRE(dl.processSample(1.0f, 100, 0.0f) == Approx(0.0f)); // no out-of-bounds read
}
