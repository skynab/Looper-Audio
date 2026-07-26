#pragma once

#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Clip.h"
#include "model/DrumKit.h"
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
    bool              muted      = false;
    bool              solo       = false;
    float             sendLevel  = 0.0f; // 0..1, pre-fader send to the shared send bus
    std::vector<Clip> clips;
    AutomationLane    gainAutomation; // this track's gain (dB) over beats; empty = static gainDb only
    DrumKit           drumKit; // only meaningful when type == Drum; empty pads otherwise
    SynthSettings     synthSettings; // only meaningful when type == Instrument

    bool operator==(const Track&) const = default;
};

} // namespace looper::model
