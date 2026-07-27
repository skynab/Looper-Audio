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

TEST_CASE("Every edit gets its own state id", "[model][history]")
{
    History<int> history(1);
    const auto start = history.stateId();

    history.apply(2, "two");
    REQUIRE(history.stateId() != start);

    const auto second = history.stateId();
    history.apply(3, "three");
    REQUIRE(history.stateId() != second);
    REQUIRE(history.stateId() != start);
}

TEST_CASE("Undoing back to a state restores that state's id", "[model][history]")
{
    // This is the whole point: a user who edits, saves, edits again and then
    // undoes is back at exactly what's on disk, and the document must read as
    // saved again rather than nagging them about changes they took back.
    History<int> history(1);
    history.apply(2, "two");
    const auto saved = history.stateId();

    history.apply(3, "three");
    REQUIRE(history.stateId() != saved);

    history.undo();
    REQUIRE(history.current() == 2);
    REQUIRE(history.stateId() == saved);

    history.redo();
    REQUIRE(history.current() == 3);
    REQUIRE(history.stateId() != saved);
}

TEST_CASE("A different edit from the same point is not the same state", "[model][history]")
{
    // Undo depth alone can't tell these apart — both leave one entry on the
    // undo stack — so a dirty check built on depth would call this saved.
    History<int> history(1);
    history.apply(2, "two");
    const auto saved = history.stateId();

    history.undo();
    history.apply(99, "something else");

    REQUIRE(history.stateId() != saved);
}

TEST_CASE("A live tweak counts as a change", "[model][history]")
{
    // Fader moves go through mutableCurrent() so they don't each land on the
    // undo stack, but they still change the document and must not leave it
    // looking saved.
    History<int> history(1);
    const auto saved = history.stateId();

    history.mutableCurrent() = 5;
    REQUIRE(history.stateId() != saved);
}

TEST_CASE("Loading a document gives it a fresh identity", "[model][history]")
{
    History<int> history(1);
    const auto before = history.stateId();
    history.reset(42);
    REQUIRE(history.stateId() != before);
}
