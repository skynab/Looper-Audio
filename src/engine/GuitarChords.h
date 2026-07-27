#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "engine/Pattern.h"

namespace looper::engine
{
inline constexpr int kChordStrings = 6;

/**
    A chord as a guitarist holds it: one fret per string, or -1 for a string
    that isn't played.

    Shapes rather than note sets, because that's the difference between a
    chord a guitar can play and six notes that happen to spell one. The same
    triad has many voicings on a neck and only some are reachable by a hand;
    storing the shape keeps the reachable one.
*/
struct ChordShape
{
    const char* name;
    int         frets[kChordStrings]; // -1 = string not played
};

/** Open-position shapes, the ones most guitar parts are actually built from.
    Movable barre chords come from the same table with a fret offset, which is
    exactly how a player thinks of them. */
inline constexpr ChordShape kChordShapes[] = {
    { "E",  {  0,  2,  2,  1,  0,  0 } },
    { "Em", {  0,  2,  2,  0,  0,  0 } },
    { "A",  { -1,  0,  2,  2,  2,  0 } },
    { "Am", { -1,  0,  2,  2,  1,  0 } },
    { "D",  { -1, -1,  0,  2,  3,  2 } },
    { "Dm", { -1, -1,  0,  2,  3,  1 } },
    { "G",  {  3,  2,  0,  0,  0,  3 } },
    { "C",  { -1,  3,  2,  0,  1,  0 } },
};

inline constexpr int kNumChordShapes = (int) (sizeof(kChordShapes) / sizeof(kChordShapes[0]));

/** How a chord is struck. */
struct StrumSettings
{
    bool   downstroke = true; // low string first; an upstroke starts high
    double spreadMs   = 18.0; // time between the first string and the last
    double humanise   = 0.0;  // 0..1, jitter on timing and velocity
};

/**
    Turning chord shapes into notes.

    JUCE-free so the timing can be measured headlessly, and deliberately
    producing *real notes at real times* rather than a playback-time "strum
    feel" — the same choice §18's swing made. A strum that exists in the
    pattern stays visible and editable, and the engine needs to know nothing
    about it.
*/
struct GuitarChords
{
    /** The MIDI notes a shape produces on a given tuning, low string to high,
        skipping strings that aren't played. @p fretOffset slides the shape up
        the neck, which is what makes a barre chord out of an open one. */
    static std::vector<int> notesForShape(const ChordShape& shape, const int* tuning, int fretOffset = 0)
    {
        std::vector<int> notes;
        notes.reserve(kChordStrings);

        for (int s = 0; s < kChordStrings; ++s)
        {
            if (shape.frets[s] < 0)
                continue; // muted or simply not struck

            // An open string stays open when the shape moves: you can't slide
            // a nut up the neck. Fretted notes move with the offset.
            const int fret = shape.frets[s] == 0 && fretOffset == 0 ? 0 : shape.frets[s] + fretOffset;
            const int note = tuning[s] + fret;
            if (note >= 0 && note <= 127)
                notes.push_back(note);
        }
        return notes;
    }

    /**
        Stamps a strummed chord into notes with staggered start times.

        Strings are struck in sequence, not together — roughly 10–30ms apart —
        and that stagger is most of what makes a strum sound like a hand rather
        than an organ. A downstroke starts on the lowest string, an upstroke on
        the highest.

        Jitter is bounded to less than half the gap between strings, so a
        humanised strum can never reorder itself: the strings must be struck in
        the order the hand moves, however loose the timing.
    */
    static std::vector<Note> strumChord(const ChordShape& shape, const int* tuning, int fretOffset,
                                        double startBeats, double lengthBeats, double bpm,
                                        const StrumSettings& strum, uint32_t seed = 1)
    {
        auto notes = notesForShape(shape, tuning, fretOffset);
        if (notes.empty())
            return {};

        if (! strum.downstroke)
            std::reverse(notes.begin(), notes.end()); // an upstroke hits the high strings first

        const double beatsPerMs = bpm > 0.0 ? bpm / 60000.0 : 0.0;
        const double spread     = std::max(0.0, strum.spreadMs) * beatsPerMs;
        const double step       = notes.size() > 1 ? spread / (double) (notes.size() - 1) : 0.0;
        const double humanise   = std::clamp(strum.humanise, 0.0, 1.0);

        uint32_t random = seed == 0 ? 1 : seed;
        auto nextUnit = [&random]
        {
            random = random * 1664525u + 1013904223u;
            return (double) ((int32_t) random) / 2147483648.0; // -1..1
        };

        std::vector<Note> out;
        out.reserve(notes.size());

        for (size_t i = 0; i < notes.size(); ++i)
        {
            // Capped below half a step, which is what guarantees the strokes
            // stay in order no matter how much humanising is asked for.
            const double jitter = nextUnit() * humanise * step * 0.4;
            const double start  = startBeats + (double) i * step + jitter;

            Note note;
            note.startBeats  = std::max(0.0, start);
            note.lengthBeats = std::max(0.05, lengthBeats);
            note.noteNumber  = notes[i];
            note.velocity    = (float) std::clamp(0.85 + nextUnit() * humanise * 0.15, 0.15, 1.0);
            out.push_back(note);
        }
        return out;
    }
};

} // namespace looper::engine
