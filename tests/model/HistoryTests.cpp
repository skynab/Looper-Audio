#include <catch2/catch_test_macros.hpp>

#include <model/History.h>

#include <vector>

using looper::model::History;

TEST_CASE("History holds the initial state and starts clean", "[model][history]")
{
    History<int> h(5);
    REQUIRE(h.current() == 5);
    REQUIRE_FALSE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
}

TEST_CASE("History apply records undo and walks back", "[model][history]")
{
    History<int> h(0);
    h.apply(1, "one");
    h.apply(2, "two");

    REQUIRE(h.current() == 2);
    REQUIRE(h.undoLabel() == "two");

    h.undo();
    REQUIRE(h.current() == 1);
    REQUIRE(h.canRedo());

    h.undo();
    REQUIRE(h.current() == 0);
    REQUIRE_FALSE(h.canUndo());
}

TEST_CASE("History redo re-applies undone edits", "[model][history]")
{
    History<int> h(0);
    h.apply(1);
    h.apply(2);
    h.undo();
    h.undo();
    REQUIRE(h.current() == 0);

    h.redo();
    REQUIRE(h.current() == 1);
    h.redo();
    REQUIRE(h.current() == 2);
    REQUIRE_FALSE(h.canRedo());
}

TEST_CASE("History clears redo when a new edit is applied", "[model][history]")
{
    History<int> h(0);
    h.apply(1);
    h.apply(2);
    h.undo(); // current == 1, redo holds 2
    REQUIRE(h.canRedo());

    h.apply(99);
    REQUIRE_FALSE(h.canRedo());
    REQUIRE(h.current() == 99);

    h.undo();
    REQUIRE(h.current() == 1);
}

TEST_CASE("History edit mutates a copy and commits", "[model][history]")
{
    History<std::vector<int>> h(std::vector<int>{});
    h.edit("push", [](std::vector<int>& v) { v.push_back(7); });

    REQUIRE(h.current().size() == 1);
    REQUIRE(h.current()[0] == 7);

    h.undo();
    REQUIRE(h.current().empty());
}
