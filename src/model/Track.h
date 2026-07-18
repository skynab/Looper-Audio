#pragma once

#include <string>
#include <vector>

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
    float             gainDb = 0.0f;
    bool              muted  = false;
    std::vector<Clip> clips;

    bool operator==(const Track&) const = default;
};

} // namespace looper::model
