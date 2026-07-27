#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "engine/GuitarString.h"
#include "engine/MidiNote.h"
#include "engine/Node.h"

namespace looper::engine
{
/** How many strings the instrument has. Six is the guitar; the code is written
    against this constant rather than a literal so a seven-string is a one-line
    change later. */
inline constexpr int kNumGuitarStrings = 6;

/** Which note-driven instrument a track's notes go to. Replaces the old
    isDrumTrack boolean, which stopped being able to express the choice as soon
    as there were three of them. */
enum class TrackInstrument
{
    Synth  = 0,
    Drum   = 1,
    Guitar = 2
};

/** Standard tuning, low to high (E2 A2 D3 G3 B3 E4), as MIDI note numbers.
    Stored as notes rather than frequencies so alternate tunings are expressed
    the way a player would say them ("drop D"). */
inline constexpr int kStandardTuning[kNumGuitarStrings] = { 40, 45, 50, 55, 59, 64 };

/** The highest fret any string can reach. Beyond this a note simply can't be
    played on that string, which is what stops the allocator putting a high
    lead line on the low E. */
inline constexpr int kMaxFret = 24;

/**
    Six strings played like a guitar rather than six voices played like a synth.

    The difference is entirely in the allocation: a guitar can sound at most one
    note per string, and a new note on a string *cuts the one already ringing
    there*. That single rule is the most audible thing separating this from a
    polyphonic synth with a plucked patch — more than any amount of DSP
    refinement — which is why it's the part the bounce checks pin down.

    Note-offs are deliberately ignored by default. A guitar string rings until
    it's replucked or damped; stopping every note when the MIDI says so would
    make held chords behave like an organ. `setMuteOnNoteOff` exists for
    callers that want the other behaviour, and is what a palm-mute articulation
    will drive later.
*/
class GuitarNode final : public Node
{
public:
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        for (auto& string : strings_)
            string.prepare(sampleRate);
        applySettings();
    }

    // ---- message thread (atomics, read once per block) ----
    void setDecaySeconds(float seconds) { decaySeconds_.store(seconds, std::memory_order_relaxed); }
    void setBrightness(float value)     { brightness_.store(value, std::memory_order_relaxed); }
    void setPickPosition(float value)   { pickPosition_.store(value, std::memory_order_relaxed); }
    void setPickHardness(float value)   { pickHardness_.store(value, std::memory_order_relaxed); }
    void setMuteOnNoteOff(float value)  { muteOnNoteOff_.store(value, std::memory_order_relaxed); }

    /** Open-string pitch of one string, as a MIDI note. Drop-D is
        setOpenNote(0, 38). */
    void setOpenNote(int stringIndex, int midiNote)
    {
        if (stringIndex >= 0 && stringIndex < kNumGuitarStrings)
            openNotes_[(size_t) stringIndex].store(midiNote, std::memory_order_relaxed);
    }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const ProcessContext& /*context*/) override
    {
        applySettings();

        const int numSamples = buffer.getNumSamples();
        int       position   = 0;

        // Rendered in spans between MIDI events, so a pluck lands on the
        // sample it was scheduled for rather than at the top of the block.
        for (const auto metadata : midi)
        {
            const int eventTime = juce::jlimit(0, numSamples, metadata.samplePosition);
            renderSpan(buffer, position, eventTime - position);
            position = eventTime;

            const auto message = metadata.getMessage();
            if (message.isNoteOn())
                pluckNote(message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isNoteOff())
                releaseNote(message.getNoteNumber());
        }

        renderSpan(buffer, position, numSamples - position);
    }

    /** Which note each string is currently sounding, or -1. Atomic because the
        fretboard pane reads it from the UI thread every frame to show what's
        ringing — which is what makes the cut-on-retrigger behaviour visible
        rather than mysterious. */
    int noteOnString(int stringIndex) const
    {
        return (stringIndex >= 0 && stringIndex < kNumGuitarStrings)
                   ? sounding_[(size_t) stringIndex].load(std::memory_order_relaxed) : -1;
    }

private:
    void renderSpan(juce::AudioBuffer<float>& buffer, int start, int count)
    {
        if (count <= 0)
            return;

        const int channels = buffer.getNumChannels();
        for (int i = 0; i < count; ++i)
        {
            float sum = 0.0f;
            for (int s = 0; s < kNumGuitarStrings; ++s)
                if (strings_[(size_t) s].isRinging())
                    sum += strings_[(size_t) s].process();

            // Six strings can sum well past unity, so scale to keep a full
            // strum inside range without needing a limiter downstream.
            sum *= 0.4f;

            for (int ch = 0; ch < channels; ++ch)
                buffer.addSample(ch, start + i, sum);
        }
    }

    /**
        Chooses a string and sounds the note on it.

        Prefers a string that isn't already holding a note, and among those the
        one needing the lowest fret — roughly what a player reaching for the
        note would do.

        When every reachable string is *already held*, the note becomes a
        **hammer-on or pull-off**: the string is re-fretted without being struck
        again, so it keeps the energy it has and simply rings at the new pitch.
        That is what a guitarist does when their hand is already on the string,
        and it's why those notes are softer than picked ones — the softness
        falls out of the model rather than being simulated.

        Only when no held string can reach the note is one taken from the
        oldest. That isn't voice stealing to save CPU; it's the hand having to
        leave one note to play another.
    */
    void pluckNote(int midiNote, float velocity)
    {
        int best       = -1;
        int bestFret   = kMaxFret + 1;
        int hammerOn   = -1;
        int hammerMove = kMaxFret + 1;
        int oldest     = -1;
        int oldestAge  = -1;

        for (int s = 0; s < kNumGuitarStrings; ++s)
        {
            const int open = openNotes_[(size_t) s].load(std::memory_order_relaxed);
            const int fret = midiNote - open;
            if (fret < 0 || fret > kMaxFret)
                continue; // this string can't reach the note at all

            if (sounding_[(size_t) s].load(std::memory_order_relaxed) < 0)
            {
                if (fret < bestFret)
                {
                    bestFret = fret;
                    best     = s;
                }
            }
            else
            {
                // Held. The nearest hand movement wins the hammer-on: a player
                // re-frets whichever string is already closest to the new note.
                const int move = std::abs(midiNote - sounding_[(size_t) s].load(std::memory_order_relaxed));
                if (move < hammerMove)
                {
                    hammerMove = move;
                    hammerOn   = s;
                }

                const int age = pluckCounter_ - pluckedAt_[(size_t) s];
                if (age > oldestAge)
                {
                    oldestAge = age;
                    oldest    = s;
                }
            }
        }

        if (best >= 0)
        {
            auto& string = strings_[(size_t) best];
            string.mute(0.0f); // a fresh pluck lifts any damping the last note left
            string.setFrequency(midiNoteToHertz(midiNote));
            string.pluck(velocity);

            sounding_[(size_t) best].store(midiNote, std::memory_order_relaxed);
            pluckedAt_[(size_t) best] = ++pluckCounter_;
            return;
        }

        if (hammerOn >= 0)
        {
            // Hand already on the string: re-fret without striking it again.
            auto& string = strings_[(size_t) hammerOn];
            string.setFrequency(midiNoteToHertz(midiNote));

            sounding_[(size_t) hammerOn].store(midiNote, std::memory_order_relaxed);
            pluckedAt_[(size_t) hammerOn] = ++pluckCounter_;
            return;
        }

        if (oldest < 0)
            return; // no string can reach this note at all; better silent than wrong

        auto& string = strings_[(size_t) oldest];
        string.mute(0.0f);
        string.setFrequency(midiNoteToHertz(midiNote));
        string.pluck(velocity);

        sounding_[(size_t) oldest].store(midiNote, std::memory_order_relaxed);
        pluckedAt_[(size_t) oldest] = ++pluckCounter_;
    }

    /** A note-off frees the string for reuse, and damps it only if the caller
        asked for that — see the class comment on why ringing on is the
        default. */
    void releaseNote(int midiNote)
    {
        const float damping = muteOnNoteOff_.load(std::memory_order_relaxed);

        for (int s = 0; s < kNumGuitarStrings; ++s)
        {
            if (sounding_[(size_t) s].load(std::memory_order_relaxed) != midiNote)
                continue;

            if (damping > 0.0f)
                strings_[(size_t) s].mute(damping);

            // Freed either way: the string is no longer holding that note, so
            // the allocator may reach for it before it has finished ringing.
            sounding_[(size_t) s].store(-1, std::memory_order_relaxed);
            return;
        }
    }

    void applySettings()
    {
        const float decay     = decaySeconds_.load(std::memory_order_relaxed);
        const float bright    = brightness_.load(std::memory_order_relaxed);
        const float position  = pickPosition_.load(std::memory_order_relaxed);
        const float hardness  = pickHardness_.load(std::memory_order_relaxed);

        for (auto& string : strings_)
        {
            string.setDecaySeconds(decay);
            string.setBrightness(bright);
            string.setPickPosition(position);
            string.setPickHardness(hardness);
        }
    }

    std::array<GuitarString, kNumGuitarStrings> strings_;

    // Audio-thread state: which note each string holds, and when it was struck.
    std::array<std::atomic<int>, kNumGuitarStrings> sounding_ { -1, -1, -1, -1, -1, -1 };
    std::array<int, kNumGuitarStrings> pluckedAt_ {};
    int                                pluckCounter_ = 0;

    std::array<std::atomic<int>, kNumGuitarStrings> openNotes_ {
        kStandardTuning[0], kStandardTuning[1], kStandardTuning[2],
        kStandardTuning[3], kStandardTuning[4], kStandardTuning[5]
    };

    std::atomic<float> decaySeconds_  { 3.0f };
    std::atomic<float> brightness_    { 0.7f };
    std::atomic<float> pickPosition_  { 0.22f };
    std::atomic<float> pickHardness_  { 0.6f };
    std::atomic<float> muteOnNoteOff_ { 0.0f };
};

} // namespace looper::engine
