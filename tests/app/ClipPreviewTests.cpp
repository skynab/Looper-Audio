#include <catch2/catch_test_macros.hpp>

#include <app/ClipPreview.h>

using namespace looper;

namespace
{
    engine::Note note(double start, double length, int pitch)
    {
        engine::Note n;
        n.startBeats  = start;
        n.lengthBeats = length;
        n.noteNumber  = pitch;
        n.velocity    = 0.8f;
        return n;
    }
}

TEST_CASE("An empty clip previews as nothing", "[app][preview]")
{
    REQUIRE(clipPreviewBlocks({}, 4.0, 4.0).empty());
}

TEST_CASE("Notes land where they are in the clip", "[app][preview]")
{
    const std::vector<engine::Note> notes { note(0.0, 1.0, 60), note(2.0, 1.0, 60) };
    const auto blocks = clipPreviewBlocks(notes, 4.0, 4.0);

    REQUIRE(blocks.size() == 2);
    REQUIRE(blocks[0].x == 0.0);
    REQUIRE(std::abs(blocks[0].width - 0.25) < 1.0e-9);
    REQUIRE(std::abs(blocks[1].x - 0.5) < 1.0e-9);
}

TEST_CASE("Higher notes are drawn higher up", "[app][preview]")
{
    // y runs down the rectangle, so the higher pitch must have the smaller y.
    const std::vector<engine::Note> notes { note(0.0, 1.0, 48), note(1.0, 1.0, 72) };
    const auto blocks = clipPreviewBlocks(notes, 4.0, 4.0);

    REQUIRE(blocks.size() == 2);
    REQUIRE(blocks[1].y < blocks[0].y);
}

TEST_CASE("Every block stays inside the rectangle", "[app][preview]")
{
    const std::vector<engine::Note> notes {
        note(0.0, 1.0, 36), note(1.5, 0.5, 84), note(3.9, 2.0, 60) // the last runs past the end
    };
    const auto blocks = clipPreviewBlocks(notes, 4.0, 4.0);

    for (const auto& b : blocks)
    {
        INFO("block " << b.x << "," << b.y << " " << b.width << "x" << b.height);
        REQUIRE(b.x >= 0.0);
        REQUIRE(b.y >= 0.0);
        REQUIRE(b.x + b.width <= 1.0 + 1.0e-9);
        REQUIRE(b.y + b.height <= 1.0 + 1.0e-9);
    }
}

TEST_CASE("A clip longer than its pattern shows the repeats", "[app][preview]")
{
    // The engine wraps playback within the pattern, so an 8-beat clip holding
    // a 4-beat pattern plays it twice. Drawing it once would show something
    // the clip doesn't do.
    const std::vector<engine::Note> notes { note(0.0, 1.0, 60), note(2.0, 1.0, 64) };

    const auto once  = clipPreviewBlocks(notes, 4.0, 4.0);
    const auto twice = clipPreviewBlocks(notes, 4.0, 8.0);

    REQUIRE(once.size() == 2);
    REQUIRE(twice.size() == 4);

    // The second pass sits in the second half.
    REQUIRE(twice[2].x >= 0.5);
    REQUIRE(twice[3].x >= 0.5);
}

TEST_CASE("A part-repeat is cut where the clip ends", "[app][preview]")
{
    // A 6-beat clip over a 4-beat pattern plays one and a half passes: the
    // note at beat 2 of the second pass falls past the end and isn't heard.
    const std::vector<engine::Note> notes { note(0.0, 1.0, 60), note(2.0, 1.0, 64) };
    const auto blocks = clipPreviewBlocks(notes, 4.0, 6.0);

    REQUIRE(blocks.size() == 3); // both of pass one, only the first of pass two
    for (const auto& b : blocks)
        REQUIRE(b.x + b.width <= 1.0 + 1.0e-9);
}

TEST_CASE("A single-pitch clip still draws", "[app][preview]")
{
    // Scaling pitch to the notes present divides by their span, which is zero
    // here — a drum part on one pad is the obvious real case.
    const std::vector<engine::Note> notes { note(0.0, 0.5, 36), note(1.0, 0.5, 36) };
    const auto blocks = clipPreviewBlocks(notes, 4.0, 4.0);

    REQUIRE(blocks.size() == 2);
    for (const auto& b : blocks)
    {
        REQUIRE(std::isfinite(b.y));
        REQUIRE(b.height > 0.0);
        REQUIRE(b.y >= 0.0);
        REQUIRE(b.y + b.height <= 1.0 + 1.0e-9);
    }
}

TEST_CASE("Pitch is scaled to the notes present, not the MIDI range", "[app][preview]")
{
    // Against 0..127 a bassline occupies a twelfth of the height and reads as
    // a flat line. Two notes an octave apart should be clearly separated.
    const std::vector<engine::Note> notes { note(0.0, 1.0, 40), note(1.0, 1.0, 52) };
    const auto blocks = clipPreviewBlocks(notes, 4.0, 4.0);

    REQUIRE(blocks.size() == 2);
    REQUIRE(std::abs(blocks[0].y - blocks[1].y) > 0.5);
}

TEST_CASE("A dense clip is capped rather than drawn in full", "[app][preview]")
{
    std::vector<engine::Note> notes;
    for (int i = 0; i < 500; ++i)
        notes.push_back(note((double) i * 0.01, 0.01, 60 + (i % 12)));

    const auto blocks = clipPreviewBlocks(notes, 8.0, 64.0, 128);
    REQUIRE(blocks.size() <= 128);
    REQUIRE_FALSE(blocks.empty());
}

TEST_CASE("A pattern length of zero doesn't divide by zero", "[app][preview]")
{
    const std::vector<engine::Note> notes { note(0.0, 1.0, 60) };
    const auto blocks = clipPreviewBlocks(notes, 0.0, 4.0);

    REQUIRE(blocks.size() == 1);
    REQUIRE(std::isfinite(blocks[0].x));
    REQUIRE(std::isfinite(blocks[0].width));
}
