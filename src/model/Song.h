#pragma once

#include <string>
#include <vector>

#include "model/Track.h"

namespace looper::model
{
/**
    The whole project document: tempo/metre plus a list of tracks. Pure value
    type — copyable and comparable — which is what makes snapshot undo/redo and
    (later) serialization straightforward. All mutation goes through the helper
    functions below so ids are allocated consistently.
*/
struct Song
{
    double bpm                = 120.0;
    int    timeSigNumerator   = 4;
    int    timeSigDenominator = 4;

    std::vector<Track> tracks;
    int                nextId = 1; // monotonic id source for tracks and clips

    bool operator==(const Song&) const = default;
};

inline int allocateId(Song& song) { return song.nextId++; }

inline Track& addTrack(Song& song, TrackType type, std::string name)
{
    Track track;
    track.id   = allocateId(song);
    track.type = type;
    track.name = std::move(name);
    song.tracks.push_back(std::move(track));
    return song.tracks.back();
}

inline Track* findTrack(Song& song, int id)
{
    for (auto& t : song.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

inline const Track* findTrack(const Song& song, int id)
{
    for (const auto& t : song.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}

inline bool removeTrack(Song& song, int id)
{
    for (auto it = song.tracks.begin(); it != song.tracks.end(); ++it)
    {
        if (it->id == id)
        {
            song.tracks.erase(it);
            return true;
        }
    }
    return false;
}

/** Adds @p clip to the given track, assigning it a fresh id. Returns nullptr if the track is missing. */
inline Clip* addClip(Song& song, int trackId, Clip clip)
{
    Track* track = findTrack(song, trackId);
    if (track == nullptr)
        return nullptr;

    clip.id = allocateId(song);
    track->clips.push_back(std::move(clip));
    return &track->clips.back();
}

} // namespace looper::model
