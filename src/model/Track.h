#pragma once

#include <string>
#include <vector>

#include "model/AutomationLane.h"
#include "model/Clip.h"

namespace looper::model
{
enum class TrackType
{
    Instrument, // MIDI clips driving a synth
    Audio       // audio-file clips
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

    bool operator==(const Track&) const = default;
};

} // namespace looper::model
