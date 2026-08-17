#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/Node.h"
#include "engine/PianoNote.h"

namespace looper::engine
{
/**
    A piano: a pool of struck, damped, coupled unisons (see PianoNote).

    Built like GuitarNode — settings arrive as atomics and are read once per
    block, MIDI is rendered in spans so a note lands on the sample it was
    scheduled for — with three differences that come straight from the
    instrument.

    **Note-off damps.** A guitar string rings until it is replucked; a piano
    note stops when the key comes up, and that is the *normal* end of it.
    GuitarNode ignores note-offs on purpose; this one must not.

    **A voice is a key, not a string.** Each holds up to three strings of its
    own, so the pool is sized in notes and the real string count is up to three
    times larger. That is the cost model that matters here.

    **The pool is small, and stealing is expected.** With the sustain pedal
    down (step 4) a passage can leave every voice ringing, so running out is a
    normal condition rather than an error, and what gets taken has to be chosen
    rather than left to whichever slot happens to be first.
*/
class PianoNode final : public Node
{
public:
    /**
        How many keys can sound at once.

        Sized against measurement rather than taste — see the bounce tool's
        piano polyphony figure. Each voice is up to three waveguides, each
        running an allpass cascade for stiffness, so 48 keys is up to 144
        strings — a different order of cost from six guitar strings.

        The number came from the bounce tool's measurement rather than from
        taste: 24 voices of bass, where the strings are longest and the
        stiffness cascade deepest, rendered at 32x realtime, so the budget was
        never the constraint the plan feared it might be. It is set here at
        48 because that is what the sustain pedal needs — with the dampers up,
        a pedalled passage leaves everything ringing — and the measurement says
        it is affordable. A passage that still exceeds it steals, which is the
        same trade a real piano's dampers make with felt.
    */
    static constexpr int kMaxVoices = 48;

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        for (auto& voice : voices_)
        {
            voice.note.prepare(sampleRate);
            voice.midiNote = -1;
            voice.struckAt = 0;
        }

        applySettings();
    }

    // ---- message thread (atomics, read once per block) ----
    void setDecaySeconds(float seconds) { decaySeconds_.store(seconds, std::memory_order_relaxed); }
    void setBrightness(float value)     { brightness_.store(value, std::memory_order_relaxed); }
    void setHammerHardness(float value) { hammerHardness_.store(value, std::memory_order_relaxed); }
    void setStrikePosition(float value) { strikePosition_.store(value, std::memory_order_relaxed); }
    void setDetuneCents(float value)    { detuneCents_.store(value, std::memory_order_relaxed); }
    void setCoupling(float value)       { coupling_.store(value, std::memory_order_relaxed); }

    /** Scales the whole keyboard's inharmonicity. The *shape* across the range
        is pianoStiffness's and is not a preference; this is how much of it. */
    void setStiffness(float value)      { stiffness_.store(value, std::memory_order_relaxed); }

    /** How far the keyboard is spread across the stereo field, 0..1 — low
        notes left, high notes right, as a piano sounds from the player's seat
        and as most recordings of one are mic'd. Mono-compatible for the same
        reason the guitar's width is: these are distinct notes, not delayed
        copies of one signal. */
    void setWidth(float value)          { width_.store(value, std::memory_order_relaxed); }

    /** How long a damper takes to stop a string, in effect. 1 is a hard stop;
        real felt takes a moment, and a hard stop clicks. */
    void setDamperStrength(float value) { damperStrength_.store(value, std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                 const ProcessContext& context) override
    {
        applySettings();

        // Stopping the transport damps everything, for the same reason it does
        // on a guitar: letting a note ring past its note-off is a musical
        // choice within playback, but ringing on after the user pressed stop
        // is not.
        if (wasPlaying_ && ! context.transport.playing)
            for (auto& voice : voices_)
            {
                voice.note.damp(1.0f);
                voice.midiNote = -1;
            }

        wasPlaying_ = context.transport.playing;

        const int numSamples = buffer.getNumSamples();
        int       position   = 0;

        for (const auto metadata : midi)
        {
            const int eventTime = juce::jlimit(0, numSamples, metadata.samplePosition);
            renderSpan(buffer, position, eventTime - position);
            position = eventTime;

            const auto message = metadata.getMessage();

            if (message.isNoteOn())
                strikeNote(message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isNoteOff())
                releaseNote(message.getNoteNumber());
        }

        renderSpan(buffer, position, numSamples - position);
    }

    /** How many keys are sounding. For meters and for the polyphony
        measurement; not used by the audio path. */
    int soundingVoices() const noexcept
    {
        int count = 0;
        for (const auto& voice : voices_)
            if (voice.note.isRinging())
                ++count;
        return count;
    }

private:
    struct Voice
    {
        PianoNote note;
        int       midiNote = -1;  // -1 when released; the note still rings on
        uint64_t  struckAt = 0;   // for stealing the oldest
        float     gainLeft  = 1.0f;
        float     gainRight = 1.0f;
    };

    void strikeNote(int midiNote, float velocity)
    {
        auto& voice = voiceFor(midiNote);

        voice.midiNote = midiNote;
        voice.struckAt = ++strikeCounter_;

        // Everything that depends on which key this is, set at strike time
        // rather than per block: a voice's pitch, string count and stiffness
        // only change when it is reassigned to a different note.
        voice.note.setStringCount(pianoStringCount(midiNote));
        voice.note.setFrequency(440.0 * std::pow(2.0, (midiNote - 69) / 12.0));
        voice.note.setStiffness(pianoStiffness(midiNote) * stiffnessScale_);
        voice.note.setDetuneCents(detuneCentsValue_);
        voice.note.setCoupling(couplingValue_);
        voice.note.setDecaySeconds(decayForNote(midiNote));
        voice.note.setBrightness(brightnessValue_);
        voice.note.setHammerHardness(hammerHardnessValue_);
        voice.note.setStrikePosition(strikePositionValue_);

        // Low notes left, high notes right.
        const float position = std::clamp((float) (midiNote - 60) / 40.0f, -1.0f, 1.0f) * widthValue_;
        voice.gainLeft  = position <= 0.0f ? 1.0f : 1.0f - position;
        voice.gainRight = position >= 0.0f ? 1.0f : 1.0f + position;

        voice.note.strike(velocity);
    }

    void releaseNote(int midiNote)
    {
        // The top of a real piano has no dampers at all — the strings are so
        // short and quiet that felt would do nothing useful — so those notes
        // ring on past the key release. It is a small thing and it is audible:
        // the top octave of a piano does not stop when you let go.
        if (midiNote >= kLowestUndampedNote)
            return;

        for (auto& voice : voices_)
            if (voice.midiNote == midiNote)
            {
                voice.note.damp(damperStrengthValue_);
                voice.midiNote = -1; // released; it may still be decaying
            }
    }

    /** The voice this note should use: the one already playing it if there is
        one, then any silent one, and only then the oldest still ringing. */
    Voice& voiceFor(int midiNote)
    {
        for (auto& voice : voices_)
            if (voice.midiNote == midiNote)
                return voice; // a repeated key takes its own string back

        for (auto& voice : voices_)
            if (voice.midiNote < 0 && ! voice.note.isRinging())
                return voice;

        // Everything is busy. Take the one struck longest ago: it is the most
        // decayed and the least likely to be missed, and stealing the *newest*
        // would cut off the note someone just played.
        Voice* oldest = &voices_[0];
        for (auto& voice : voices_)
            if (voice.struckAt < oldest->struckAt)
                oldest = &voice;

        return *oldest;
    }

    /** How long a note rings before its damper falls. Bass strings carry far
        more energy and ring far longer than treble ones — a low A sustains for
        the best part of a minute, a top C for a second or two — and a single
        decay time across the keyboard is one of the more obvious ways a
        modelled piano gives itself away. */
    double decayForNote(int midiNote) const
    {
        const double normalised = std::clamp((midiNote - 21) / 87.0, 0.0, 1.0);
        return baseDecaySeconds_ * std::pow(0.12, normalised);
    }

    void renderSpan(juce::AudioBuffer<float>& buffer, int start, int count)
    {
        if (count <= 0)
            return;

        const int channels = buffer.getNumChannels();

        for (auto& voice : voices_)
        {
            if (! voice.note.isRinging())
                continue;

            for (int i = 0; i < count; ++i)
            {
                const float value = voice.note.process() * kOutputScale;

                if (channels >= 2)
                {
                    buffer.addSample(0, start + i, value * voice.gainLeft);
                    buffer.addSample(1, start + i, value * voice.gainRight);
                }
                else if (channels == 1)
                {
                    buffer.addSample(0, start + i, value);
                }
            }
        }
    }

    void applySettings()
    {
        baseDecaySeconds_     = (double) decaySeconds_.load(std::memory_order_relaxed);
        brightnessValue_      = brightness_.load(std::memory_order_relaxed);
        hammerHardnessValue_  = hammerHardness_.load(std::memory_order_relaxed);
        strikePositionValue_  = strikePosition_.load(std::memory_order_relaxed);
        detuneCentsValue_     = detuneCents_.load(std::memory_order_relaxed);
        couplingValue_        = coupling_.load(std::memory_order_relaxed);
        stiffnessScale_       = stiffness_.load(std::memory_order_relaxed);
        widthValue_           = std::clamp(width_.load(std::memory_order_relaxed), 0.0f, 1.0f);
        damperStrengthValue_  = damperStrength_.load(std::memory_order_relaxed);
    }

    /** Above this, a real piano has no dampers. */
    static constexpr int kLowestUndampedNote = 93; // ~A6

    /** Keeps a handful of keys inside range without a limiter downstream, the
        same job GuitarNode's 0.4 does for six strings. A five-note chord
        measured a peak of 2.9 before this existed — a piano is played in
        chords far more than a guitar is, so the headroom has to assume them. */
    static constexpr float kOutputScale = 0.3f;

    std::array<Voice, kMaxVoices> voices_;
    uint64_t                      strikeCounter_ = 0;
    bool                          wasPlaying_    = false;

    std::atomic<float> decaySeconds_   { 20.0f }; // the bottom of the keyboard; scaled up the range
    std::atomic<float> brightness_     { 0.72f };
    std::atomic<float> hammerHardness_ { 0.45f };
    std::atomic<float> strikePosition_ { 0.125f }; // an eighth along — see PianoNote
    std::atomic<float> detuneCents_    { PianoNote::kDefaultDetuneCents };
    std::atomic<float> coupling_       { 0.7f };
    std::atomic<float> stiffness_      { 0.8f };
    std::atomic<float> width_          { 0.35f };
    std::atomic<float> damperStrength_ { 0.85f };

    double baseDecaySeconds_    = 20.0;
    float  brightnessValue_     = 0.72f;
    float  hammerHardnessValue_ = 0.45f;
    float  strikePositionValue_ = 0.125f;
    float  detuneCentsValue_    = PianoNote::kDefaultDetuneCents;
    float  couplingValue_       = 0.7f;
    float  stiffnessScale_      = 0.8f;
    float  widthValue_          = 0.35f;
    float  damperStrengthValue_ = 0.85f;
};

} // namespace looper::engine
