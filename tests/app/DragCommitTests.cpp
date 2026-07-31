#include <catch2/catch_test_macros.hpp>
#include <app/DragCommit.h>

using namespace looper;

namespace
{
    struct Fake { float gain = 0.0f; int untouched = 7; };
    auto writeGain = [](Fake& f, float v) { f.gain = v; };
}

TEST_CASE("A drag becomes one undo step back to where it started", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    for (float v : { -5.0f, -3.0f, 0.0f, 2.0f })
        history.mutableCurrent().gain = v;
    REQUIRE_FALSE(history.canUndo());
    REQUIRE(commitDrag(history, "Set gain", -6.0f, history.current().gain, writeGain));
    REQUIRE(history.current().gain == 2.0f);
    REQUIRE(history.canUndo());
    history.undo();
    REQUIRE(history.current().gain == -6.0f);
    history.redo();
    REQUIRE(history.current().gain == 2.0f);
}

TEST_CASE("Without the rewind, undo lands mid-drag", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    history.mutableCurrent().gain = 2.0f;
    history.edit("Set gain", [](Fake& f) { f.gain = 2.0f; }); // the naive/broken version
    history.undo();
    REQUIRE(history.current().gain == 2.0f); // ...not -6, which is the bug
}

TEST_CASE("A drag that goes nowhere leaves no step", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    REQUIRE_FALSE(commitDrag(history, "Set gain", -6.0f, -6.0f, writeGain));
    REQUIRE_FALSE(history.canUndo());
    REQUIRE_FALSE(commitDrag(history, "Set gain", -6.0f, -6.0000001f, writeGain));
    REQUIRE_FALSE(history.canUndo());
}

TEST_CASE("Committing a drag disturbs nothing else", "[app][drag]")
{
    model::History<Fake> history(Fake { -6.0f, 7 });
    history.mutableCurrent().gain = 3.0f;
    history.mutableCurrent().untouched = 42;
    REQUIRE(commitDrag(history, "Set gain", -6.0f, 3.0f, writeGain));
    REQUIRE(history.current().gain == 3.0f);
    REQUIRE(history.current().untouched == 42);
    history.undo();
    REQUIRE(history.current().gain == -6.0f);
    REQUIRE(history.current().untouched == 42);
}
