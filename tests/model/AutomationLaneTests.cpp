#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <model/AutomationLane.h>

using Catch::Approx;
using looper::model::AutomationLane;

TEST_CASE("AutomationLane returns the fallback when empty", "[model][automation]")
{
    AutomationLane lane;
    REQUIRE(lane.empty());
    REQUIRE(lane.valueAt(5.0, -1.0f) == Approx(-1.0f));
}

TEST_CASE("AutomationLane interpolates linearly and holds the ends", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 8.0f);

    REQUIRE(lane.valueAt(0.0)  == Approx(0.0f));
    REQUIRE(lane.valueAt(1.0)  == Approx(2.0f));
    REQUIRE(lane.valueAt(2.0)  == Approx(4.0f));
    REQUIRE(lane.valueAt(4.0)  == Approx(8.0f));
    REQUIRE(lane.valueAt(-1.0) == Approx(0.0f)); // before first -> first
    REQUIRE(lane.valueAt(9.0)  == Approx(8.0f)); // after last -> last
}

TEST_CASE("AutomationLane keeps points sorted and replaces coincident beats", "[model][automation]")
{
    AutomationLane lane;
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 0.5f);

    REQUIRE(lane.points().size() == 3);
    REQUIRE(lane.points()[0].beat == Approx(0.0));
    REQUIRE(lane.points()[1].beat == Approx(2.0));
    REQUIRE(lane.points()[2].beat == Approx(4.0));

    lane.addPoint(2.0, 0.9f); // replace, not insert
    REQUIRE(lane.points().size() == 3);
    REQUIRE(lane.valueAt(2.0) == Approx(0.9f));
}
