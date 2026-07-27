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

TEST_CASE("A clip can be removed from a track", "[model][song]")
{
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth").id;
    addClip(song, id, Clip {});
    addClip(song, id, Clip {});
    const int secondClipId = findTrack(song, id)->clips[1].id;

    REQUIRE(removeClip(song, id, 0));
    REQUIRE(findTrack(song, id)->clips.size() == 1);
    REQUIRE(findTrack(song, id)->clips[0].id == secondClipId); // the right one survived
}

TEST_CASE("Removing a clip refuses coordinates it doesn't have", "[model][song]")
{
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth").id;
    addClip(song, id, Clip {});

    REQUIRE_FALSE(removeClip(song, id, 1));    // past the end
    REQUIRE_FALSE(removeClip(song, id, -1));
    REQUIRE_FALSE(removeClip(song, 9999, 0));  // no such track
    REQUIRE(findTrack(song, id)->clips.size() == 1);
}

TEST_CASE("A removed clip's id is never handed out again", "[model][song]")
{
    // Ids come from a counter that only counts up. If removal freed ids for
    // reuse, a clip could inherit a stale reference to a deleted one.
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth").id;
    const int firstId = addClip(song, id, Clip {})->id;

    REQUIRE(removeClip(song, id, 0));
    REQUIRE(addClip(song, id, Clip {})->id != firstId);
}

TEST_CASE("Removing a track takes its session column with it", "[model][song]")
{
    Song song;
    const int keep = addTrack(song, TrackType::Instrument, "Keep").id;
    const int drop = addTrack(song, TrackType::Instrument, "Drop").id;
    addScene(song, "A");
    addScene(song, "B");
    setSessionClip(song, 1, 0, Clip {});

    REQUIRE(removeTrack(song, drop));
    REQUIRE(song.tracks.size() == 1);
    REQUIRE(song.tracks[0].id == keep);
    REQUIRE(song.tracks[0].sessionSlots.size() == 2); // the grid is still two rows deep
}

TEST_CASE("Removing a scene keeps the session grid rectangular", "[model][song]")
{
    // Every session lookup indexes a track's slots by scene index, so a grid
    // that loses a row from the scene list but not from the tracks would read
    // the wrong cell from then on.
    Song song;
    addTrack(song, TrackType::Instrument, "One");
    addTrack(song, TrackType::Instrument, "Two");
    addScene(song, "A");
    addScene(song, "B");
    addScene(song, "C");

    Clip marker;
    marker.lengthBeats = 7.0;
    setSessionClip(song, 0, 2, marker); // a clip in scene C

    REQUIRE(removeScene(song, 0)); // drop scene A

    REQUIRE(song.scenes.size() == 2);
    for (const auto& track : song.tracks)
        REQUIRE(track.sessionSlots.size() == 2);

    // C was the third row and is now the second; the clip must have moved
    // with it rather than staying at an index that no longer means C.
    const Clip* moved = sessionClip(song, 0, 1);
    REQUIRE(moved != nullptr);
    REQUIRE(moved->lengthBeats == 7.0);
}

TEST_CASE("Removing a scene refuses an index it doesn't have", "[model][song]")
{
    Song song;
    addTrack(song, TrackType::Instrument, "One");
    addScene(song, "A");

    REQUIRE_FALSE(removeScene(song, 1));
    REQUIRE_FALSE(removeScene(song, -1));
    REQUIRE(song.scenes.size() == 1);
}

TEST_CASE("A track can be renamed", "[model][song]")
{
    Song song;
    const int id = addTrack(song, TrackType::Instrument, "Synth 1").id;

    REQUIRE(renameTrack(song, id, "Lead"));
    REQUIRE(findTrack(song, id)->name == "Lead");
    REQUIRE_FALSE(renameTrack(song, 9999, "Nope"));
}
