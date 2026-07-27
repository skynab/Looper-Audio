#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Pattern.h"
#include "engine/SequencerMath.h"

namespace looper::engine
{
/** The 128 note numbers a player currently has sounding, so they can be
    released when it stops or switches to different material. */
using ActiveNotes = std::array<bool, 128>;

/**
    Turning a looping Pattern into MIDI for one block.

    Shared by the arrangement's Sequencer and the session's SessionPlayer:
    both emit exactly the same notes from exactly the same Pattern, and only
    differ in *which* pattern is playing and *from when*. Keeping one copy
    means the next timing bug has one place to be fixed rather than two that
    quietly disagree.
*/
struct PatternPlayback
{
    /** Emits the note edges falling inside this block.

        @p localStartSamples is where the block begins relative to the
        pattern's own start — the sequencer measures it from a clip's position
        on the timeline, the session player from wherever the clip was
        launched. It's wrapped to the pattern length here, so either caller can
        hand over a raw distance. */
    static void emitBlock(juce::MidiBuffer& midi, const Pattern& pattern,
                          double localStartSamples, double samplesPerBeat,
                          int numSamples, ActiveNotes& activeNotes)
    {
        const double length = pattern.lengthBeats * samplesPerBeat;
        if (length <= 1.0)
            return;

        const double blockStart = wrapPositive(localStartSamples, length);

        for (const auto& note : pattern.notes)
        {
            double onSample  = note.startBeats * samplesPerBeat;
            double offSample = (note.startBeats + note.lengthBeats) * samplesPerBeat;

            if (onSample >= length)
                continue; // starts past the loop's end, so it never sounds
            if (offSample >= length)
                offSample = length - 1.0;
            if (offSample <= onSample)
                offSample = onSample + 1.0;

            const int noteNumber = juce::jlimit(0, 127, note.noteNumber);
            int offset = 0;

            if (edgeInBlock(onSample, blockStart, length, numSamples, offset))
            {
                const auto velocity = (juce::uint8) juce::jlimit(1, 127, (int) (note.velocity * 127.0f));
                midi.addEvent(juce::MidiMessage::noteOn(1, noteNumber, velocity), offset);
                activeNotes[(size_t) noteNumber] = true;
            }

            if (edgeInBlock(offSample, blockStart, length, numSamples, offset))
            {
                midi.addEvent(juce::MidiMessage::noteOff(1, noteNumber), offset);
                activeNotes[(size_t) noteNumber] = false;
            }
        }
    }

    /** Releases everything currently sounding. Called when playback stops, or
        when a player switches material, so notes can't hang. */
    static void flush(juce::MidiBuffer& midi, ActiveNotes& activeNotes)
    {
        for (int n = 0; n < 128; ++n)
        {
            if (activeNotes[(size_t) n])
            {
                midi.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                activeNotes[(size_t) n] = false;
            }
        }
    }
};

} // namespace looper::engine
