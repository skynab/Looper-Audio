#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "model/Song.h"

namespace looper::model
{
/**
    A small, line-based text format for the project document. Deliberately
    JUCE-free so the round-trip can be unit-tested headless.

    Layout is flat and count-prefixed so it parses deterministically. Numbers use
    %.17g (exact IEEE double round-trip); string fields (track name, audio file)
    are the rest of their line, so they may contain spaces.

    **Versioning.** `serialize` always writes kFormatVersion; there is no
    "save as an older version", so there's one write path to reason about and
    test. `deserialize` is the tolerant side: records introduced after a given
    version are read only *if present*, so an older file simply leaves those
    fields at their struct defaults (which are chosen to be behaviour-
    preserving). Where a record's own shape changed rather than a new record
    being added — only DPAD so far — the version decides how to read it.

    A file written by a *newer* build is refused outright rather than
    part-parsed: silently dropping records the user can't see would be worse
    than declining to open it.
*/

/** Bumped whenever the format changes. History worth knowing:
      11  the format before per-track synths
      12  + SYNTH (per-track model::SynthSettings)
      13  DPAD carries per-pad gain/pan/pitch/mute/solo before its sample path
      14  + TFX (per-track insert filter/delay/reverb)
      15  TRACK carries pan before its (rest-of-line) name
      16  TAUTO (one gain lane) -> TAUTOS/TLANE (a lane per parameter)
      17  + SCENES/SCENE and per-track SESSION/SSLOT (the session grid)
      18  TFX (a fixed filter/delay/reverb trio) -> FXCHAIN/FXSLOT (an
          ordered chain whose slots may be built-ins or hosted plugins)
      19  + GUITAR (per-track model::GuitarSettings)
      20  FXSLOT gains five drive fields (the guitar pedal). A file written
          before this simply stops short of them, and the reader keeps the
          defaults it started with. */
inline constexpr int kFormatVersion = 20;
namespace detail
{
    inline std::string num(double v)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return buffer;
    }

    /** One clip record: its header plus its note list. Shared by the
        arrangement's clips and the session grid's, so the two can't drift. */
    inline void writeClip(std::ostringstream& out, const Clip& clip)
    {
        out << "CLIP " << clip.id << " " << (int) clip.type << " "
            << num(clip.startBeats) << " " << num(clip.lengthBeats) << " "
            << num(clip.pattern.lengthBeats) << " " << clip.audioFile << "\n";
        out << "NOTES " << clip.pattern.notes.size() << "\n";

        for (const auto& note : clip.pattern.notes)
            out << "NOTE " << num(note.startBeats) << " " << num(note.lengthBeats)
                << " " << note.noteNumber << " " << num((double) note.velocity) << "\n";
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
    out << "LOOPER " << kFormatVersion << "\n";
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
        << (int) song.sendBus.effectType << " "
        << detail::num((double) song.sendBus.roomSize) << " "
        << detail::num((double) song.sendBus.damping) << " "
        << detail::num((double) song.sendBus.delayTimeMs) << " "
        << detail::num((double) song.sendBus.delayFeedback) << " "
        << detail::num((double) song.sendBus.returnLevel) << "\n";
    out << "PROJECTROOT " << song.projectRootFolder << "\n";
    out << "AUTO " << song.masterGainDb.points().size() << "\n";
    for (const auto& p : song.masterGainDb.points())
        out << "APT " << detail::num(p.beat) << " " << detail::num((double) p.value) << "\n";
    out << "SCENES " << song.scenes.size() << "\n";
    for (const auto& scene : song.scenes)
        out << "SCENE " << scene.name << "\n"; // name is rest-of-line, so it may contain spaces

    out << "TRACKS " << song.tracks.size() << "\n";

    for (const auto& track : song.tracks)
    {
        out << "TRACK " << track.id << " " << (int) track.type << " "
            << detail::num((double) track.gainDb) << " " << (track.muted ? 1 : 0)
            << " " << (track.solo ? 1 : 0) << " " << detail::num((double) track.sendLevel)
            << " " << detail::num((double) track.pan)
            << " " << track.name << "\n";
        // Only non-empty lanes are written, so an unautomated track costs one
        // "TAUTOS 0" line rather than one empty record per automatable
        // parameter (a list that will only grow).
        size_t laneCount = 0;
        for (const auto& [param, lane] : track.automation)
            if (! lane.empty())
                ++laneCount;

        out << "TAUTOS " << laneCount << "\n";
        for (const auto& [param, lane] : track.automation)
        {
            if (lane.empty())
                continue;
            out << "TLANE " << param << " " << lane.points().size() << "\n";
            for (const auto& pt : lane.points())
                out << "TAPT " << detail::num(pt.beat) << " " << detail::num((double) pt.value) << "\n";
        }
        out << "DRUMKIT " << track.drumKit.pads.size() << "\n";
        for (const auto& pad : track.drumKit.pads)
            // label is a space-free token (no pad-rename UI exists yet, so
            // this always holds); samplePath is the rest of the line, like
            // clip.audioFile/track.name, since a real file path can have
            // spaces — so every fixed-width field has to precede it.
            out << "DPAD " << pad.noteNumber << " " << pad.label << " "
                << detail::num((double) pad.gainDb) << " "
                << detail::num((double) pad.pan) << " "
                << detail::num((double) pad.pitchSemitones) << " "
                << (pad.muted ? 1 : 0) << " " << (pad.solo ? 1 : 0) << " "
                << pad.samplePath << "\n";

        const auto& synth = track.synthSettings;
        out << "SYNTH " << synth.waveform << " "
            << detail::num((double) synth.attackMs) << " " << detail::num((double) synth.decayMs) << " "
            << detail::num((double) synth.sustain) << " " << detail::num((double) synth.releaseMs) << " "
            << (synth.filterEnabled ? 1 : 0) << " " << synth.filterMode << " "
            << detail::num((double) synth.filterCutoff) << " " << detail::num((double) synth.filterResonance) << " "
            << detail::num((double) synth.gainDb) << "\n";

        const auto& guitar = track.guitarSettings;
        out << "GUITAR";
        for (int note : guitar.tuning)
            out << " " << note;
        out << " " << detail::num((double) guitar.decaySeconds)
            << " " << detail::num((double) guitar.brightness)
            << " " << detail::num((double) guitar.pickPosition)
            << " " << detail::num((double) guitar.pickHardness)
            << " " << detail::num((double) guitar.muteOnNoteOff) << "\n";

        // The effect chain, in order. A slot carries every built-in's settings
        // regardless of its kind, so switching kind doesn't lose the others.
        out << "FXCHAIN " << track.effectChain.size() << "\n";
        for (const auto& slot : track.effectChain)
        {
            out << "FXSLOT " << (int) slot.kind << " " << (slot.enabled ? 1 : 0) << " "
                << slot.filter.mode << " "
                << detail::num((double) slot.filter.cutoff) << " "
                << detail::num((double) slot.filter.resonance) << " "
                << detail::num((double) slot.delay.timeMs) << " "
                << detail::num((double) slot.delay.feedback) << " "
                << detail::num((double) slot.delay.mix) << " "
                << detail::num((double) slot.reverb.roomSize) << " "
                << detail::num((double) slot.reverb.damping) << " "
                << detail::num((double) slot.reverb.mix) << " "
                << detail::num((double) slot.drive.drive) << " "
                << detail::num((double) slot.drive.tone) << " "
                << detail::num((double) slot.drive.level) << " "
                << (slot.drive.hardClip ? 1 : 0) << " "
                << (slot.drive.cabinet ? 1 : 0) << "\n";

            if (slot.kind == EffectKind::Plugin)
            {
                // Split across lines because identifier, name and state are all
                // free-form: each takes the rest of its own line rather than
                // needing escaping.
                out << "FXPLUGFMT " << (int) slot.plugin.format << "\n";
                out << "FXPLUGID " << slot.plugin.identifier << "\n";
                out << "FXPLUGNAME " << slot.plugin.name << "\n";
                out << "FXPLUGSTATE " << slot.plugin.state << "\n";
            }
        }

        // The session grid's column for this track. Slots are written by index
        // including the empty ones, since the index is the scene.
        out << "SESSION " << track.sessionSlots.size() << "\n";
        for (const auto& slot : track.sessionSlots)
        {
            out << "SSLOT " << (slot.hasClip ? 1 : 0) << "\n";
            if (slot.hasClip)
                detail::writeClip(out, slot.clip);
        }

        out << "CLIPS " << track.clips.size() << "\n";

        for (const auto& clip : track.clips)
            detail::writeClip(out, clip);
    }

    return out.str();
}

/** Reads a project. @p errorOut, if given, receives a short human-readable
    reason on failure (the caller shows it — see MainComponent::openProject);
    @p out is left untouched unless the whole parse succeeds. */
inline bool deserialize(const std::string& text, Song& out, std::string* errorOut = nullptr)
{
    auto fail = [&](const char* why)
    {
        if (errorOut != nullptr)
            *errorOut = why;
        return false;
    };

    // Buffered into lines with a cursor, rather than streamed, so a record can
    // be *offered* and declined without being consumed — which is what lets an
    // older file skip records added in later versions (see readTagged below).
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string        line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    size_t cursor = 0;

    /** Consumes the next line and returns its remainder *only* if it carries
        @p expectedTag; otherwise leaves the cursor alone and returns false.
        Required records treat false as an error; records added in a later
        format version simply let their defaults stand. */
    auto readTagged = [&](const char* expectedTag, std::string& rest) -> bool
    {
        if (cursor >= lines.size())
            return false;

        std::istringstream ls(lines[cursor]);
        std::string        tag;
        ls >> tag;
        if (tag != expectedTag)
            return false;

        std::getline(ls, rest);
        rest = detail::trimLeadingSpace(std::move(rest));
        ++cursor;
        return true;
    };

    std::string rest;

    /** One clip record, the mirror of detail::writeClip — used for both the
        arrangement's clips and the session grid's, so the two can't drift
        apart. Returns false if the record is missing or truncated. */
    auto readClip = [&](Clip& clip) -> bool
    {
        if (! readTagged("CLIP", rest))
            return false;
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
        return true;
    };

    if (! readTagged("LOOPER", rest))
        return fail("not a Looper project file");

    const int version = std::atoi(rest.c_str());
    if (version <= 0)
        return fail("unrecognised project format version");
    if (version > kFormatVersion)
        return fail("saved by a newer version of Looper-Audio");

    Song song;

    if (! readTagged("BPM", rest))    return fail("missing tempo"); song.bpm = std::strtod(rest.c_str(), nullptr);
    if (! readTagged("TSNUM", rest))  return fail("missing time signature"); song.timeSigNumerator = std::atoi(rest.c_str());
    if (! readTagged("TSDEN", rest))  return fail("missing time signature"); song.timeSigDenominator = std::atoi(rest.c_str());
    if (! readTagged("NEXTID", rest)) return fail("missing id counter"); song.nextId = std::atoi(rest.c_str());

    // Everything from here to TRACKS is read only if present: each of these
    // records joined the format at some point, so an older file just leaves
    // the corresponding defaults in place.
    if (readTagged("FILTER", rest))
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

    if (readTagged("DELAY", rest))
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

    if (readTagged("REVERB", rest))
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

    if (readTagged("SENDBUS", rest))
    {
        std::istringstream sb(rest);
        int    enabled = 0, effectType = 0;
        double roomSize = 0.0, damping = 0.0, delayTimeMs = 0.0, delayFeedback = 0.0, returnLevel = 0.0;
        sb >> enabled >> effectType >> roomSize >> damping >> delayTimeMs >> delayFeedback >> returnLevel;
        song.sendBus.enabled       = enabled != 0;
        song.sendBus.effectType    = (SendBusEffectType) effectType;
        song.sendBus.roomSize      = (float) roomSize;
        song.sendBus.damping       = (float) damping;
        song.sendBus.delayTimeMs   = (float) delayTimeMs;
        song.sendBus.delayFeedback = (float) delayFeedback;
        song.sendBus.returnLevel   = (float) returnLevel;
    }

    if (readTagged("PROJECTROOT", rest))
        song.projectRootFolder = rest;

    if (readTagged("AUTO", rest))
    {
        const int pointCount = std::atoi(rest.c_str());
        song.masterGainDb.clear();
        for (int i = 0; i < pointCount; ++i)
        {
            // Once a count-prefixed record is present its points are not
            // optional — a short list means the file is damaged, not old.
            if (! readTagged("APT", rest)) return fail("truncated master automation");
            std::istringstream ps(rest);
            double beat = 0.0, value = 0.0;
            ps >> beat >> value;
            song.masterGainDb.addPoint(beat, (float) value);
        }
    }

    if (readTagged("SCENES", rest)) // added in v17; older files have no session grid
    {
        const int sceneCount = std::atoi(rest.c_str());
        for (int s = 0; s < sceneCount; ++s)
        {
            if (! readTagged("SCENE", rest)) return fail("truncated scene list");
            song.scenes.push_back(Scene { rest });
        }
    }

    if (! readTagged("TRACKS", rest)) return fail("missing track list");
    const int trackCount = std::atoi(rest.c_str());

    for (int i = 0; i < trackCount; ++i)
    {
        if (! readTagged("TRACK", rest))
            return fail("truncated track list");

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

            // Pan joined this record in v15, ahead of the rest-of-line name.
            // Like DPAD, the field count can't be used to detect it, so the
            // version decides.
            if (version >= 15)
            {
                double pan = 0.0;
                ts >> pan;
                track.pan = (float) pan;
            }

            std::string name;
            std::getline(ts, name);
            track.name = detail::trimLeadingSpace(std::move(name));
        }

        // Before v16 a track had exactly one lane, always gain, written as a
        // bare TAUTO point list. Read it straight into the Gain lane so an
        // older project keeps its automation rather than silently losing it.
        if (readTagged("TAUTO", rest))
        {
            const int pointCount = std::atoi(rest.c_str());
            auto&     gainLane   = track.laneFor(TrackParam::Gain);
            for (int p = 0; p < pointCount; ++p)
            {
                if (! readTagged("TAPT", rest)) return fail("truncated track automation");
                std::istringstream ps(rest);
                double beat = 0.0, value = 0.0;
                ps >> beat >> value;
                gainLane.addPoint(beat, (float) value);
            }
        }
        else if (readTagged("TAUTOS", rest))
        {
            const int laneCount = std::atoi(rest.c_str());
            for (int l = 0; l < laneCount; ++l)
            {
                if (! readTagged("TLANE", rest)) return fail("truncated automation lane list");
                std::istringstream ls(rest);
                int paramId = 0, pointCount = 0;
                ls >> paramId >> pointCount;

                auto& lane = track.automation[paramId];
                for (int p = 0; p < pointCount; ++p)
                {
                    if (! readTagged("TAPT", rest)) return fail("truncated track automation");
                    std::istringstream ps(rest);
                    double beat = 0.0, value = 0.0;
                    ps >> beat >> value;
                    lane.addPoint(beat, (float) value);
                }
            }
        }

        if (readTagged("DRUMKIT", rest))
        {
            const int padCount = std::atoi(rest.c_str());
            for (int p = 0; p < padCount; ++p)
            {
                if (! readTagged("DPAD", rest)) return fail("truncated drum kit");
                std::istringstream ps(rest);
                DrumPad pad;
                ps >> pad.noteNumber >> pad.label;

                // The one record whose *shape* changed rather than being
                // added wholesale: before v13 a pad was just note/label/path,
                // and the mix fields didn't exist. They can't be detected by
                // token count because the path is rest-of-line and may
                // contain spaces, so the version decides.
                if (version >= 13)
                {
                    double gainDb = 0.0, pan = 0.0, pitchSemitones = 0.0;
                    int    muted = 0, solo = 0;
                    ps >> gainDb >> pan >> pitchSemitones >> muted >> solo;
                    pad.gainDb         = (float) gainDb;
                    pad.pan            = (float) pan;
                    pad.pitchSemitones = (float) pitchSemitones;
                    pad.muted          = muted != 0;
                    pad.solo           = solo != 0;
                }

                std::string samplePath;
                std::getline(ps, samplePath);
                pad.samplePath = detail::trimLeadingSpace(std::move(samplePath));
                track.drumKit.pads.push_back(pad);
            }
        }

        if (readTagged("SYNTH", rest)) // added in v12; older files keep the defaults
        {
            std::istringstream ss(rest);
            int    waveform = 0, filterEnabled = 0, filterMode = 0;
            double attackMs = 0.0, decayMs = 0.0, sustain = 0.0, releaseMs = 0.0;
            double filterCutoff = 0.0, filterResonance = 0.0, gainDb = 0.0;
            ss >> waveform >> attackMs >> decayMs >> sustain >> releaseMs
               >> filterEnabled >> filterMode >> filterCutoff >> filterResonance >> gainDb;
            track.synthSettings.waveform        = waveform;
            track.synthSettings.attackMs        = (float) attackMs;
            track.synthSettings.decayMs         = (float) decayMs;
            track.synthSettings.sustain         = (float) sustain;
            track.synthSettings.releaseMs       = (float) releaseMs;
            track.synthSettings.filterEnabled   = filterEnabled != 0;
            track.synthSettings.filterMode      = filterMode;
            track.synthSettings.filterCutoff    = (float) filterCutoff;
            track.synthSettings.filterResonance = (float) filterResonance;
            track.synthSettings.gainDb          = (float) gainDb;
        }

        if (readTagged("GUITAR", rest)) // added in v19; older files keep the defaults
        {
            std::istringstream gs(rest);
            for (int s = 0; s < kNumGuitarStrings; ++s)
                gs >> track.guitarSettings.tuning[(size_t) s];

            double decay = 0.0, brightness = 0.0, position = 0.0, hardness = 0.0, mute = 0.0;
            gs >> decay >> brightness >> position >> hardness >> mute;
            track.guitarSettings.decaySeconds  = (float) decay;
            track.guitarSettings.brightness    = (float) brightness;
            track.guitarSettings.pickPosition  = (float) position;
            track.guitarSettings.pickHardness  = (float) hardness;
            track.guitarSettings.muteOnNoteOff = (float) mute;
        }

        // v14..v17 stored a fixed filter/delay/reverb trio. Migrate it into
        // three chain slots in that same order, so an old project comes back
        // with its effects in the order it had them and sounding the same.
        if (readTagged("TFX", rest))
        {
            std::istringstream fs(rest);
            int    filterOn = 0, filterMode = 0, delayOn = 0, reverbOn = 0;
            double cutoff = 0.0, resonance = 0.0;
            double delayTime = 0.0, delayFeedback = 0.0, delayMix = 0.0;
            double room = 0.0, damping = 0.0, reverbMix = 0.0;
            fs >> filterOn >> filterMode >> cutoff >> resonance
               >> delayOn >> delayTime >> delayFeedback >> delayMix
               >> reverbOn >> room >> damping >> reverbMix;

            EffectSlot filterSlot;
            filterSlot.kind             = EffectKind::Filter;
            filterSlot.enabled          = filterOn != 0;
            filterSlot.filter.enabled   = filterOn != 0;
            filterSlot.filter.mode      = filterMode;
            filterSlot.filter.cutoff    = (float) cutoff;
            filterSlot.filter.resonance = (float) resonance;

            EffectSlot delaySlot;
            delaySlot.kind           = EffectKind::Delay;
            delaySlot.enabled        = delayOn != 0;
            delaySlot.delay.enabled  = delayOn != 0;
            delaySlot.delay.timeMs   = (float) delayTime;
            delaySlot.delay.feedback = (float) delayFeedback;
            delaySlot.delay.mix      = (float) delayMix;

            EffectSlot reverbSlot;
            reverbSlot.kind            = EffectKind::Reverb;
            reverbSlot.enabled         = reverbOn != 0;
            reverbSlot.reverb.enabled  = reverbOn != 0;
            reverbSlot.reverb.roomSize = (float) room;
            reverbSlot.reverb.damping  = (float) damping;
            reverbSlot.reverb.mix      = (float) reverbMix;

            track.effectChain.push_back(filterSlot);
            track.effectChain.push_back(delaySlot);
            track.effectChain.push_back(reverbSlot);
        }
        else if (readTagged("FXCHAIN", rest)) // v18 onward
        {
            const int slotCount = std::atoi(rest.c_str());
            for (int s = 0; s < slotCount; ++s)
            {
                if (! readTagged("FXSLOT", rest)) return fail("truncated effect chain");

                std::istringstream ss(rest);
                int    kind = 0, enabled = 0, filterMode = 0;
                double cutoff = 0.0, resonance = 0.0;
                double delayTime = 0.0, delayFeedback = 0.0, delayMix = 0.0;
                double room = 0.0, damping = 0.0, reverbMix = 0.0;

                // Defaults matter: a file written before version 20 has no
                // drive fields, the extractions below simply fail, and these
                // values are what the slot keeps.
                double driveAmount = 4.0, driveTone = 0.5, driveLevel = 0.7;
                int    driveHard = 0, driveCab = 1;

                ss >> kind >> enabled >> filterMode >> cutoff >> resonance
                   >> delayTime >> delayFeedback >> delayMix >> room >> damping >> reverbMix
                   >> driveAmount >> driveTone >> driveLevel >> driveHard >> driveCab;

                EffectSlot slot;
                slot.kind              = (EffectKind) kind;
                slot.enabled           = enabled != 0;
                slot.filter.enabled    = slot.enabled && slot.kind == EffectKind::Filter;
                slot.filter.mode       = filterMode;
                slot.filter.cutoff     = (float) cutoff;
                slot.filter.resonance  = (float) resonance;
                slot.delay.enabled     = slot.enabled && slot.kind == EffectKind::Delay;
                slot.delay.timeMs      = (float) delayTime;
                slot.delay.feedback    = (float) delayFeedback;
                slot.delay.mix         = (float) delayMix;
                slot.reverb.enabled    = slot.enabled && slot.kind == EffectKind::Reverb;
                slot.reverb.roomSize   = (float) room;
                slot.reverb.damping    = (float) damping;
                slot.reverb.mix        = (float) reverbMix;
                slot.drive.enabled     = slot.enabled && slot.kind == EffectKind::Drive;
                slot.drive.drive       = (float) driveAmount;
                slot.drive.tone        = (float) driveTone;
                slot.drive.level       = (float) driveLevel;
                slot.drive.hardClip    = driveHard != 0;
                slot.drive.cabinet     = driveCab != 0;

                if (slot.kind == EffectKind::Plugin)
                {
                    if (! readTagged("FXPLUGFMT", rest))   return fail("truncated plugin slot");
                    slot.plugin.format = (PluginFormat) std::atoi(rest.c_str());
                    if (! readTagged("FXPLUGID", rest))    return fail("truncated plugin slot");
                    slot.plugin.identifier = rest;
                    if (! readTagged("FXPLUGNAME", rest))  return fail("truncated plugin slot");
                    slot.plugin.name = rest;
                    if (! readTagged("FXPLUGSTATE", rest)) return fail("truncated plugin slot");
                    slot.plugin.state = rest;
                }

                track.effectChain.push_back(std::move(slot));
            }
        }

        if (readTagged("SESSION", rest)) // added in v17
        {
            const int slotCount = std::atoi(rest.c_str());
            for (int s = 0; s < slotCount; ++s)
            {
                if (! readTagged("SSLOT", rest)) return fail("truncated session grid");

                SessionSlot slot;
                slot.hasClip = std::atoi(rest.c_str()) != 0;
                if (slot.hasClip && ! readClip(slot.clip))
                    return fail("truncated session clip");
                track.sessionSlots.push_back(std::move(slot));
            }
        }

        if (! readTagged("CLIPS", rest))
            return fail("missing clip list");
        const int clipCount = std::atoi(rest.c_str());

        for (int j = 0; j < clipCount; ++j)
        {
            Clip clip;
            if (! readClip(clip))
                return fail("truncated clip list");
            track.clips.push_back(std::move(clip));
        }

        song.tracks.push_back(std::move(track));
    }

    out = std::move(song);
    return true;
}

} // namespace looper::model
