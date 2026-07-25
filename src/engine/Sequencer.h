#pragma once

#include <array>
#include <atomic>

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

    A clip start offset (in beats) delays when the pattern begins: the track
    stays silent until the transport reaches that point, then plays and loops
    indefinitely from there — an arrangement-style "this part enters at bar N."
    The clip's *length* deliberately does not gate playback (only its start):
    every track created through the current UI has clip.lengthBeats equal to the
    pattern's own length, so honouring it would silence every track after one
    loop and regress the "plays until Stop" behaviour the whole app is built
    around. Gating on length too is future work once there's a UI for it.

    Pattern edits are handed over lock-free (inbox/reclaim FIFOs, same as clips),
    and the audio thread never allocates. On stop, or when playback is before
    the clip start, any notes it started are flushed so voices don't hang.
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

    void setClipStartBeats(double beats) { clipStartBeats_.store(beats, std::memory_order_relaxed); }

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
                wasActive_  = false;
            }
            return;
        }
        wasPlaying_ = true;

        if (current_ == nullptr || context.transport.bpm <= 0.0 || context.sampleRate <= 0.0)
            return;

        const double samplesPerBeat  = context.sampleRate * 60.0 / context.transport.bpm;
        const double clipStartSample = clipStartBeats_.load(std::memory_order_relaxed) * samplesPerBeat;
        const double localStart      = (double) context.transport.playheadSamples - clipStartSample;
        const int    numSamples      = context.numSamples;

        if (localStart + (double) numSamples <= 0.0)
        {
            // The whole block is before this track's clip starts.
            if (wasActive_)
            {
                flushActiveNotes(midi);
                wasActive_ = false;
            }
            return;
        }
        wasActive_ = true;

        const double length = current_->lengthBeats * samplesPerBeat;
        if (length <= 1.0)
            return;

        // blockStart is the pattern-local position, wrapped; for the block that
        // straddles the clip's start, this can place a note up to one block
        // early — an accepted, documented imprecision (blocks are a few ms).
        const double blockStart = wrapPositive(localStart, length);

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

    std::atomic<double>   clipStartBeats_ { 0.0 };
    bool                  wasPlaying_ = false;
    bool                  wasActive_  = false;
    std::array<bool, 128> activeNotes_ {};
};

} // namespace looper::engine
