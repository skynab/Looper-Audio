#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include "model/Song.h"

namespace looper::model
{
/**
    A small, line-based text format for the project document. Deliberately
    JUCE-free so the round-trip can be unit-tested headless.

    Layout is flat and count-prefixed so it parses deterministically. Numbers use
    %.17g (exact IEEE double round-trip); string fields (track name, audio file)
    are the rest of their line, so they may contain spaces.
*/
namespace detail
{
    inline std::string num(double v)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return buffer;
    }

    inline std::string trimLeadingSpace(std::string s)
    {
        if (! s.empty() && s.front() == ' ')
            s.erase(0, 1);
        return s;
    }
}

inline std::string serialize(const Song& song)
{
    std::ostringstream out;
    out << "LOOPER 1\n";
    out << "BPM " << detail::num(song.bpm) << "\n";
    out << "TSNUM " << song.timeSigNumerator << "\n";
    out << "TSDEN " << song.timeSigDenominator << "\n";
    out << "NEXTID " << song.nextId << "\n";
    out << "TRACKS " << song.tracks.size() << "\n";

    for (const auto& track : song.tracks)
    {
        out << "TRACK " << track.id << " " << (int) track.type << " "
            << detail::num((double) track.gainDb) << " " << (track.muted ? 1 : 0)
            << " " << track.name << "\n";
        out << "CLIPS " << track.clips.size() << "\n";

        for (const auto& clip : track.clips)
        {
            out << "CLIP " << clip.id << " " << (int) clip.type << " "
                << detail::num(clip.startBeats) << " " << detail::num(clip.lengthBeats) << " "
                << detail::num(clip.pattern.lengthBeats) << " " << clip.audioFile << "\n";
            out << "NOTES " << clip.pattern.notes.size() << "\n";

            for (const auto& note : clip.pattern.notes)
                out << "NOTE " << detail::num(note.startBeats) << " " << detail::num(note.lengthBeats)
                    << " " << note.noteNumber << " " << detail::num((double) note.velocity) << "\n";
        }
    }

    return out.str();
}

inline bool deserialize(const std::string& text, Song& out)
{
    std::istringstream in(text);
    std::string        line;

    // Reads the next line, checks its leading tag, and returns the remainder.
    auto readTagged = [&](const char* expectedTag, std::string& rest) -> bool
    {
        if (! std::getline(in, line))
            return false;
        std::istringstream ls(line);
        std::string        tag;
        ls >> tag;
        if (tag != expectedTag)
            return false;
        std::getline(ls, rest);
        rest = detail::trimLeadingSpace(std::move(rest));
        return true;
    };

    std::string rest;
    if (! readTagged("LOOPER", rest))
        return false;

    Song song;

    if (! readTagged("BPM", rest))    return false; song.bpm = std::strtod(rest.c_str(), nullptr);
    if (! readTagged("TSNUM", rest))  return false; song.timeSigNumerator = std::atoi(rest.c_str());
    if (! readTagged("TSDEN", rest))  return false; song.timeSigDenominator = std::atoi(rest.c_str());
    if (! readTagged("NEXTID", rest)) return false; song.nextId = std::atoi(rest.c_str());
    if (! readTagged("TRACKS", rest)) return false;
    const int trackCount = std::atoi(rest.c_str());

    for (int i = 0; i < trackCount; ++i)
    {
        if (! readTagged("TRACK", rest))
            return false;

        Track track;
        {
            std::istringstream ts(rest);
            int typeInt = 0, muteInt = 0;
            double gain = 0.0;
            ts >> track.id >> typeInt >> gain >> muteInt;
            track.type   = (TrackType) typeInt;
            track.gainDb = (float) gain;
            track.muted  = muteInt != 0;
            std::string name;
            std::getline(ts, name);
            track.name = detail::trimLeadingSpace(std::move(name));
        }

        if (! readTagged("CLIPS", rest))
            return false;
        const int clipCount = std::atoi(rest.c_str());

        for (int j = 0; j < clipCount; ++j)
        {
            if (! readTagged("CLIP", rest))
                return false;

            Clip clip;
            {
                std::istringstream cs(rest);
                int typeInt = 0;
                cs >> clip.id >> typeInt >> clip.startBeats >> clip.lengthBeats >> clip.pattern.lengthBeats;
                clip.type = (ClipType) typeInt;
                std::string audio;
                std::getline(cs, audio);
                clip.audioFile = detail::trimLeadingSpace(std::move(audio));
            }

            if (! readTagged("NOTES", rest))
                return false;
            const int noteCount = std::atoi(rest.c_str());

            for (int k = 0; k < noteCount; ++k)
            {
                if (! readTagged("NOTE", rest))
                    return false;
                std::istringstream ns(rest);
                engine::Note note;
                double velocity = 0.0;
                ns >> note.startBeats >> note.lengthBeats >> note.noteNumber >> velocity;
                note.velocity = (float) velocity;
                clip.pattern.notes.push_back(note);
            }

            track.clips.push_back(std::move(clip));
        }

        song.tracks.push_back(std::move(track));
    }

    out = std::move(song);
    return true;
}

} // namespace looper::model
