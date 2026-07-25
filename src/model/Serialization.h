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
    out << "LOOPER 7\n";
    out << "BPM " << detail::num(song.bpm) << "\n";
    out << "TSNUM " << song.timeSigNumerator << "\n";
    out << "TSDEN " << song.timeSigDenominator << "\n";
    out << "NEXTID " << song.nextId << "\n";
    out << "FILTER " << (song.filter.enabled ? 1 : 0) << " " << song.filter.mode << " "
        << detail::num((double) song.filter.cutoff) << " "
        << detail::num((double) song.filter.resonance) << "\n";
    out << "DELAY " << (song.delay.enabled ? 1 : 0) << " "
        << detail::num((double) song.delay.timeMs) << " "
        << detail::num((double) song.delay.feedback) << " "
        << detail::num((double) song.delay.mix) << "\n";
    out << "REVERB " << (song.reverb.enabled ? 1 : 0) << " "
        << detail::num((double) song.reverb.roomSize) << " "
        << detail::num((double) song.reverb.damping) << " "
        << detail::num((double) song.reverb.mix) << "\n";
    out << "SENDBUS " << (song.sendBus.enabled ? 1 : 0) << " "
        << detail::num((double) song.sendBus.roomSize) << " "
        << detail::num((double) song.sendBus.damping) << " "
        << detail::num((double) song.sendBus.returnLevel) << "\n";
    out << "AUTO " << song.masterGainDb.points().size() << "\n";
    for (const auto& p : song.masterGainDb.points())
        out << "APT " << detail::num(p.beat) << " " << detail::num((double) p.value) << "\n";
    out << "TRACKS " << song.tracks.size() << "\n";

    for (const auto& track : song.tracks)
    {
        out << "TRACK " << track.id << " " << (int) track.type << " "
            << detail::num((double) track.gainDb) << " " << (track.muted ? 1 : 0)
            << " " << (track.solo ? 1 : 0) << " " << detail::num((double) track.sendLevel)
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

    if (! readTagged("FILTER", rest)) return false;
    {
        std::istringstream fs(rest);
        int    enabled = 0, mode = 0;
        double cutoff = 0.0, resonance = 0.0;
        fs >> enabled >> mode >> cutoff >> resonance;
        song.filter.enabled   = enabled != 0;
        song.filter.mode      = mode;
        song.filter.cutoff    = (float) cutoff;
        song.filter.resonance = (float) resonance;
    }

    if (! readTagged("DELAY", rest)) return false;
    {
        std::istringstream ds(rest);
        int    enabled = 0;
        double timeMs = 0.0, feedback = 0.0, mix = 0.0;
        ds >> enabled >> timeMs >> feedback >> mix;
        song.delay.enabled  = enabled != 0;
        song.delay.timeMs   = (float) timeMs;
        song.delay.feedback = (float) feedback;
        song.delay.mix      = (float) mix;
    }

    if (! readTagged("REVERB", rest)) return false;
    {
        std::istringstream rs(rest);
        int    enabled = 0;
        double roomSize = 0.0, damping = 0.0, mix = 0.0;
        rs >> enabled >> roomSize >> damping >> mix;
        song.reverb.enabled  = enabled != 0;
        song.reverb.roomSize = (float) roomSize;
        song.reverb.damping  = (float) damping;
        song.reverb.mix      = (float) mix;
    }

    if (! readTagged("SENDBUS", rest)) return false;
    {
        std::istringstream sb(rest);
        int    enabled = 0;
        double roomSize = 0.0, damping = 0.0, returnLevel = 0.0;
        sb >> enabled >> roomSize >> damping >> returnLevel;
        song.sendBus.enabled     = enabled != 0;
        song.sendBus.roomSize    = (float) roomSize;
        song.sendBus.damping     = (float) damping;
        song.sendBus.returnLevel = (float) returnLevel;
    }

    if (! readTagged("AUTO", rest)) return false;
    {
        const int pointCount = std::atoi(rest.c_str());
        song.masterGainDb.clear();
        for (int i = 0; i < pointCount; ++i)
        {
            if (! readTagged("APT", rest)) return false;
            std::istringstream ps(rest);
            double beat = 0.0, value = 0.0;
            ps >> beat >> value;
            song.masterGainDb.addPoint(beat, (float) value);
        }
    }

    if (! readTagged("TRACKS", rest)) return false;
    const int trackCount = std::atoi(rest.c_str());

    for (int i = 0; i < trackCount; ++i)
    {
        if (! readTagged("TRACK", rest))
            return false;

        Track track;
        {
            std::istringstream ts(rest);
            int typeInt = 0, muteInt = 0, soloInt = 0;
            double gain = 0.0, sendLevel = 0.0;
            ts >> track.id >> typeInt >> gain >> muteInt >> soloInt >> sendLevel;
            track.type      = (TrackType) typeInt;
            track.gainDb    = (float) gain;
            track.muted     = muteInt != 0;
            track.solo      = soloInt != 0;
            track.sendLevel = (float) sendLevel;
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
