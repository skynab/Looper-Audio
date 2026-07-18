#include <catch2/catch_test_macros.hpp>

#include <model/History.h>
#include <model/Song.h>

using namespace looper::model;

TEST_CASE("addTrack assigns increasing ids and appends", "[model][song]")
{
    Song s;
    // Capture ids by value — addTrack returns a reference into the vector, which
    // a subsequent addTrack may invalidate by reallocating.
    const int aId = addTrack(s, TrackType::Instrument, "Synth").id;
    const int bId = addTrack(s, TrackType::Audio, "Vocals").id;

    REQUIRE(s.tracks.size() == 2);
    REQUIRE(aId == 1);
    REQUIRE(bId == 2);
    REQUIRE(s.tracks[0].name == "Synth");
    REQUIRE(s.tracks[1].type == TrackType::Audio);
}

TEST_CASE("findTrack and removeTrack behave", "[model][song]")
{
    Song s;
    const int id = addTrack(s, TrackType::Instrument, "A").id;

    REQUIRE(findTrack(s, id) != nullptr);
    REQUIRE(findTrack(s, 999) == nullptr);
    REQUIRE(removeTrack(s, id));
    REQUIRE(s.tracks.empty());
    REQUIRE_FALSE(removeTrack(s, id));
}

TEST_CASE("addClip appends to a track with a fresh id", "[model][song]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "A").id;

    Clip clip;
    clip.lengthBeats = 8.0;
    Clip* added = addClip(s, trackId, clip);

    REQUIRE(added != nullptr);
    REQUIRE(added->id != 0);
    REQUIRE(findTrack(s, trackId)->clips.size() == 1);
    REQUIRE(addClip(s, 999, clip) == nullptr); // missing track
}

TEST_CASE("Song edits are undoable through History", "[model][song][history]")
{
    Song initial;
    addTrack(initial, TrackType::Instrument, "Synth");

    History<Song> h(initial);
    h.edit("Add track", [](Song& s) { addTrack(s, TrackType::Audio, "Drums"); });
    REQUIRE(h.current().tracks.size() == 2);

    h.undo();
    REQUIRE(h.current().tracks.size() == 1);
    REQUIRE(h.current() == initial); // exact restore

    h.redo();
    REQUIRE(h.current().tracks.size() == 2);
}
