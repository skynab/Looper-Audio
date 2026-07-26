#pragma once

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
    AutomationLane    gainAutomation; // this track's gain (dB) over beats; empty = static gainDb only
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
};

} // namespace looper::model
