#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Track.h"

namespace looper
{
/** One entry in the track-colour menu. */
struct TrackColourOption
{
    const char*  name;
    juce::uint32 argb; // 0 = "leave it alone", i.e. the default lane colour
};

/** The palette offered per track.

    Deliberately a fixed set rather than a colour picker: the point of track
    colours is telling parts apart at a glance, and that works better with a
    handful of distinguishable hues than with the whole spectrum, most of
    which is indistinguishable at the size a clip is drawn.

    Stored on the track as the ARGB value rather than as an index into this
    list, so reordering or extending the palette can't silently recolour
    everyone's existing projects.
*/
inline constexpr TrackColourOption kTrackColours[] = {
    { "Default", 0x00000000 },
    { "Red",     0xffb0413e },
    { "Orange",  0xffb0703a },
    { "Yellow",  0xff9c8f34 },
    { "Green",   0xff3a7d44 },
    { "Teal",    0xff2f7d78 },
    { "Blue",    0xff36618e },
    { "Purple",  0xff6b4a8f },
};

inline constexpr int kNumTrackColours = (int) (sizeof(kTrackColours) / sizeof(kTrackColours[0]));

/** The lane colour a track with no colour of its own gets — the green clips
    were drawn in before any of this existed, so untouched projects look
    exactly as they did. */
inline constexpr juce::uint32 kDefaultTrackColour = 0xff3a7d44;

inline juce::Colour trackColour(juce::uint32 stored)
{
    return juce::Colour(stored == 0 ? kDefaultTrackColour : stored);
}

/** A short tag naming what kind of track this is.

    Track names double as the type indicator until someone renames one — call
    a guitar track "Verse" and nothing on screen says it is a guitar any more.
    This is what keeps that readable, so renaming costs nothing.
*/
inline const char* trackTypeTag(model::TrackType type)
{
    switch (type)
    {
        case model::TrackType::Instrument: return "SYN";
        case model::TrackType::Audio:      return "AUD";
        case model::TrackType::Drum:       return "DRM";
        case model::TrackType::Guitar:     return "GTR";
    }
    return "SYN";
}

} // namespace looper
