#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Pattern.h"
#include "engine/ProcessContext.h"
#include "engine/SequencerMath.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Turns a Pattern into MIDI, emitted into the per-block buffer sample-accurately
    and slaved to the transport. The pattern loops on its own length, so it
    repeats cleanly whether or not the transport's loop is engaged (its length
    should divide the transport loop length).

    Pattern edits are handed over lock-free (inbox/reclaim FIFOs, same as clips),
    and the audio thread never allocates. On stop, any notes it started are
    flushed so voices don't hang.
*/
class Sequencer
{
public:
    ~Sequencer()
    {
        collectRetired();
        delete current_;

        Pattern* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    // ---- message thread ----
    void submitPattern(Pattern* pattern)
    {
        if (! inbox_.push(pattern))
            delete pattern;
    }

    void collectRetired()
    {
        Pattern* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    // ---- audio thread ----
    void renderBlock(juce::MidiBuffer& midi, const ProcessContext& context)
    {
        Pattern* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_);
            current_ = incoming;
        }

        if (! context.transport.playing)
        {
            if (wasPlaying_)
            {
                flushActiveNotes(midi);
                wasPlaying_ = false;
            }
            return;
        }
        wasPlaying_ = true;

        if (current_ == nullptr || context.transport.bpm <= 0.0 || context.sampleRate <= 0.0)
            return;

        const double samplesPerBeat = context.sampleRate * 60.0 / context.transport.bpm;
        const double length         = current_->lengthBeats * samplesPerBeat;
        if (length <= 1.0)
            return;

        const int    numSamples = context.numSamples;
        const double blockStart = wrapPositive((double) context.transport.playheadSamples, length);

        for (const auto& note : current_->notes)
        {
            double onSample  = note.startBeats * samplesPerBeat;
            double offSample = (note.startBeats + note.lengthBeats) * samplesPerBeat;

            if (onSample >= length)
                continue;
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
                activeNotes_[(size_t) noteNumber] = true;
            }

            if (edgeInBlock(offSample, blockStart, length, numSamples, offset))
            {
                midi.addEvent(juce::MidiMessage::noteOff(1, noteNumber), offset);
                activeNotes_[(size_t) noteNumber] = false;
            }
        }
    }

private:
    void flushActiveNotes(juce::MidiBuffer& midi)
    {
        for (int n = 0; n < 128; ++n)
        {
            if (activeNotes_[(size_t) n])
            {
                midi.addEvent(juce::MidiMessage::noteOff(1, n), 0);
                activeNotes_[(size_t) n] = false;
            }
        }
    }

    Pattern* current_ = nullptr;
    rt::SpscRingBuffer<Pattern*> inbox_   { 16 };
    rt::SpscRingBuffer<Pattern*> reclaim_ { 32 };

    bool                  wasPlaying_ = false;
    std::array<bool, 128> activeNotes_ {};
};

} // namespace looper::engine
