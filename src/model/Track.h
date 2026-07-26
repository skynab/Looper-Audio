#pragma once

#include <map>
#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Clip.h"
#include "model/DrumKit.h"
#include "model/Effects.h"
#include "model/SynthSettings.h"

namespace looper::model
{
enum class TrackType
{
    Instrument, // MIDI clips driving a synth
    Audio,      // audio-file clips
    Drum        // MIDI clips driving a per-pad drum kit (see DrumKit)
};

/** Which of a track's parameters an automation lane drives (see
    Track::automation). Stored as the map's key, so adding an automatable
    parameter is a new enumerator plus the code that applies it — not a new
    field on Track, a new serialization record and a new playback branch, as
    it was when gain was the only one.

    The numeric values are written to the project file, so they are part of
    the format: append, never renumber. */
enum class TrackParam
{
    Gain      = 0, // dB
    Pan       = 1, // -1..+1
    SendLevel = 2  // 0..1
};

struct Track
{
    int               id     = 0;
    std::string       name;
    TrackType         type   = TrackType::Instrument;
    float             gainDb     = 0.0f;
    float             pan        = 0.0f; // -1 = hard left, 0 = centre, +1 = hard right
    bool              muted      = false;
    bool              solo       = false;
    float             sendLevel  = 0.0f; // 0..1, pre-fader send to the shared send bus
    std::vector<Clip> clips;

    // Automation lanes, keyed by TrackParam. A parameter with no lane (or an
    // empty one) simply uses its static value, which is why an unautomated
    // track carries no lanes at all rather than a set of empty ones.
    std::map<int, AutomationLane> automation;
    DrumKit           drumKit; // only meaningful when type == Drum; empty pads otherwise
    SynthSettings     synthSettings; // only meaningful when type == Instrument

    // This track's own insert effects, applied to its output before its fader
    // (and so before its send too). The same three effects the master bus
    // has, in the same fixed order — filter, then delay, then reverb — each
    // switchable on its own and all disabled by default, so a track that has
    // never been touched sounds exactly as it did before inserts existed.
    //
    // Deliberately a fixed trio rather than a general chain: it needs no
    // real-time graph surgery, which is the rule the whole engine is built
    // on. Arbitrary ordering and duplicate effects want a proper slot
    // abstraction, and that's better designed alongside plugin hosting, which
    // forces the question anyway.
    FilterSettings    insertFilter;
    DelaySettings     insertDelay;
    ReverbSettings    insertReverb;

    bool operator==(const Track&) const = default;

    /** The lane driving @p param, or nullptr if that parameter isn't
        automated. Read side — never creates a lane, so merely asking doesn't
        change the document. */
    const AutomationLane* lane(TrackParam param) const
    {
        const auto it = automation.find((int) param);
        return (it != automation.end() && ! it->second.empty()) ? &it->second : nullptr;
    }

    /** The lane driving @p param, creating an empty one if needed. Write
        side, for recording automation. */
    AutomationLane& laneFor(TrackParam param) { return automation[(int) param]; }

    /** True if any parameter on this track is automated. */
    bool hasAutomation() const
    {
        for (const auto& [param, lane] : automation)
            if (! lane.empty())
                return true;
        return false;
    }
};

} // namespace looper::model
