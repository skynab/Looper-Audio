#pragma once

#include <atomic>

#include <juce_audio_basics/juce_audio_basics.h>

namespace looper::engine
{
/**
    Captures audio input into a pre-allocated RAM buffer while armed and the
    transport is playing. The buffer is sized once up front (prepare()) for a
    maximum take length, so the audio thread never allocates; if a take runs
    past that capacity, further audio is silently dropped (a documented v1
    limit — proper disk streaming, already on the roadmap, removes it).

    Hand-off is RT-safe without a lock-free queue: the *same* pre-allocated
    buffer is reused take after take, but access is time-sliced rather than
    concurrent. arm() (message thread) is the only thing that starts writes;
    process() (audio thread) is the only thing that performs them; the message
    thread only reads the buffer after observing isFinished() == true, which
    process() itself sets — on its own thread — the instant it notices
    recording has stopped, so there's no window where both sides touch the
    buffer at once.
*/
class AudioRecorder
{
public:
    void prepare(double sampleRate, int numChannels, double maxRecordSeconds)
    {
        sampleRate_ = sampleRate;
        take_.setSize(juce::jmax(1, numChannels), juce::jmax(1, (int) (maxRecordSeconds * sampleRate)));
        take_.clear();
    }

    // ---- message thread ----
    /** Starts a new take. Only call once any previous take has been read out
        (isFinished() observed true) — arm() reuses the same buffer. */
    void arm()
    {
        take_.clear();
        writePosition_.store(0, std::memory_order_relaxed);
        finished_.store(false, std::memory_order_relaxed);
        armed_.store(true, std::memory_order_release);
    }

    /** Signals the audio thread to stop capturing; the take finishes on the
        next block it processes (or immediately if the transport already isn't
        playing). */
    void disarm() { armed_.store(false, std::memory_order_release); }

    bool isArmed() const noexcept { return armed_.load(std::memory_order_relaxed); }

    /** True once the audio thread has confirmed it will no longer touch the
        take buffer — only then is it safe to read takeBuffer()/takeLength(). */
    bool isFinished() const noexcept { return finished_.load(std::memory_order_acquire); }

    /** Samples captured so far — safe to poll live for a recording-time readout. */
    int recordedSampleCount() const noexcept { return writePosition_.load(std::memory_order_relaxed); }

    // ---- message thread, only after isFinished() ----
    const juce::AudioBuffer<float>& takeBuffer() const noexcept { return take_; }
    int    takeLength() const noexcept { return writePosition_.load(std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_; }

    // ---- audio thread ----
    /** @p inputChannelData may be nullptr (no input device) or have fewer
        channels than take_ — handled gracefully either way. */
    void process(const float* const* inputChannelData, int numInputChannels,
                int numSamples, bool transportPlaying) noexcept
    {
        const bool armedNow     = armed_.load(std::memory_order_acquire);
        const bool recordingNow = armedNow && transportPlaying;

        if (wasRecording_ && ! recordingNow)
            finished_.store(true, std::memory_order_release);
        wasRecording_ = recordingNow;

        if (! recordingNow || inputChannelData == nullptr)
            return;

        const int pos      = writePosition_.load(std::memory_order_relaxed);
        const int capacity = take_.getNumSamples();
        if (pos >= capacity)
            return; // out of room; drop further audio (documented v1 limit)

        const int n        = juce::jmin(numSamples, capacity - pos);
        const int channels = juce::jmin(numInputChannels, take_.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
            if (inputChannelData[ch] != nullptr)
                take_.copyFrom(ch, pos, inputChannelData[ch], n);

        writePosition_.store(pos + n, std::memory_order_relaxed);
    }

private:
    juce::AudioBuffer<float> take_;
    std::atomic<int>  writePosition_ { 0 };
    std::atomic<bool> armed_    { false };
    std::atomic<bool> finished_ { true }; // true initially: no take pending
    bool              wasRecording_ = false; // audio-thread only
    double            sampleRate_   = 0.0;
};

} // namespace looper::engine
