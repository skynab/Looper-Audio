#include <catch2/catch_test_macros.hpp>

#include <app/DockLayoutTree.h>

using namespace looper;

namespace
{
    std::unique_ptr<DockLayoutNode> makeLeaf(std::vector<std::string> panels, std::string active = {})
    {
        auto leaf = std::make_unique<DockLayoutNode>();
        leaf->panels      = std::move(panels);
        leaf->activePanel = std::move(active);
        return leaf;
    }

    std::unique_ptr<DockLayoutNode> makeSplit(bool horizontal, double ratio,
                                              std::unique_ptr<DockLayoutNode> first,
                                              std::unique_ptr<DockLayoutNode> second)
    {
        auto node = std::make_unique<DockLayoutNode>();
        node->horizontal = horizontal;
        node->ratio      = ratio;
        node->first      = std::move(first);
        node->second     = std::move(second);
        return node;
    }

    /** Structural comparison — the round-trip only has to preserve meaning,
        and ratios go through a %.4f text form. */
    bool same(const DockLayoutNode& a, const DockLayoutNode& b)
    {
        if (a.isLeaf() != b.isLeaf())
            return false;

        if (a.isLeaf())
            return a.panels == b.panels && a.activePanel == b.activePanel;

        return a.horizontal == b.horizontal
            && std::abs(a.ratio - b.ratio) < 1.0e-4
            && same(*a.first, *b.first)
            && same(*a.second, *b.second);
    }
}

TEST_CASE("A single tab group round-trips", "[app][dock]")
{
    const auto original = makeLeaf({ "Tracks", "Keys", "Mixer" }, "Keys");

    const auto restored = parseDockLayout(writeDockLayout(*original));
    REQUIRE(restored != nullptr);
    REQUIRE(same(*restored, *original));
}

TEST_CASE("A nested split layout round-trips", "[app][dock]")
{
    // Files | ( (Tracks over Keys/Synth) | Mixer ) — the shape of the
    // app's default workspace.
    auto original = makeSplit(true, 0.18,
                              makeLeaf({ "Files" }, "Files"),
                              makeSplit(true, 0.72,
                                        makeSplit(false, 0.45,
                                                  makeLeaf({ "Tracks" }, "Tracks"),
                                                  makeLeaf({ "Keys", "Synth" }, "Synth")),
                                        makeLeaf({ "Mixer" }, "Mixer")));

    const auto restored = parseDockLayout(writeDockLayout(*original));
    REQUIRE(restored != nullptr);
    REQUIRE(same(*restored, *original));
}

TEST_CASE("An empty tab group round-trips", "[app][dock]")
{
    // A workspace emptied down to one bare region — still a valid layout,
    // and the one a user sees after dragging every panel out of it.
    const auto original = makeLeaf({});

    const auto text = writeDockLayout(*original);
    REQUIRE(text == "[|]");

    const auto restored = parseDockLayout(text);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->isLeaf());
    REQUIRE(restored->panels.empty());
}

TEST_CASE("Split orientation and ratio survive the round trip", "[app][dock]")
{
    const auto stacked = makeSplit(false, 0.25, makeLeaf({ "A" }), makeLeaf({ "B" }));

    const auto text = writeDockLayout(*stacked);
    REQUIRE(text.front() == 'V'); // stacked, not side by side

    const auto restored = parseDockLayout(text);
    REQUIRE(restored != nullptr);
    REQUIRE_FALSE(restored->horizontal);
    REQUIRE(std::abs(restored->ratio - 0.25) < 1.0e-4);
}

TEST_CASE("Out-of-range ratios are clamped on parse", "[app][dock]")
{
    // Hand-written/corrupted input: a ratio of 0 would give one half no space
    // at all and no way to drag it back.
    const auto restored = parseDockLayout("H0.0000([|A],[|B])");
    REQUIRE(restored != nullptr);
    REQUIRE(restored->ratio >= 0.05);
}

TEST_CASE("parseDockLayout rejects malformed input", "[app][dock]")
{
    REQUIRE(parseDockLayout("") == nullptr);
    REQUIRE(parseDockLayout("nonsense") == nullptr);
    REQUIRE(parseDockLayout("H0.5([|A]") == nullptr);          // unbalanced
    REQUIRE(parseDockLayout("H0.5([|A][|B])") == nullptr);     // missing comma
    REQUIRE(parseDockLayout("X0.5([|A],[|B])") == nullptr);    // unknown tag
    REQUIRE(parseDockLayout("[|A]trailing") == nullptr);       // trailing junk
    REQUIRE(parseDockLayout("[Aterminated") == nullptr);       // unterminated leaf
}
