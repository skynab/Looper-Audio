#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/ClipData.h"
#include "engine/Interpolation.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Auditions one audio file, on its own clock.

    Deliberately *not* the existing AudioFilePlayerNode. That one is slaved to
    the transport — it plays only while the song is rolling and reads its
    position from the song playhead — which is right for a clip on the
    timeline and wrong for an editor. Auditioning a selection means pressing
    play and hearing that selection, whether or not the song is playing and
    without moving the song's playhead.

    So this keeps its own position, in seconds into the file, published as an
    atomic the UI reads to draw a playhead.

    Clip hand-off is the same lock-free arrangement AudioFilePlayerNode uses:
    the message thread submits a decoded ClipData pointer, the audio thread
    swaps it in and returns the retired one through a second FIFO for the
    message thread to delete. The audio thread never touches a refcount.
*/
class AudioPreviewPlayer
{
public:
    ~AudioPreviewPlayer()
    {
        collectRetired();
        delete current_;

        ClipData* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    void prepare(double sampleRate)
    {
        deviceSampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        stop();
    }

    // ---- message thread ----

    /** Hands ownership of @p clip to the audio thread. Stops any audition in
        progress: the position it was at means nothing in a different file. */
    void submitClip(ClipData* clip)
    {
        stop();
        if (! inbox_.push(clip))
            delete clip;
    }

    /** Frees clips the audio thread has retired. Call periodically from the
        message thread. */
    void collectRetired()
    {
        ClipData* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    /** Plays [fromSeconds, toSeconds). A @p toSeconds at or below
        @p fromSeconds means "to the end of the file", which is how "no
        selection" arrives here. */
    void play(double fromSeconds, double toSeconds)
    {
        startSeconds_.store(std::max(0.0, fromSeconds), std::memory_order_relaxed);
        endSeconds_.store(toSeconds, std::memory_order_relaxed);
        positionSeconds_.store(std::max(0.0, fromSeconds), std::memory_order_relaxed);

        // Ordered after the range so the audio thread can't observe a restart
        // with the previous range still in place.
        restart_.store(true, std::memory_order_release);
        playing_.store(true, std::memory_order_release);
    }

    void stop()
    {
        playing_.store(false, std::memory_order_relaxed);
    }

    // ---- lock-free readouts ----
    bool   isPlaying() const noexcept { return playing_.load(std::memory_order_relaxed); }
    double positionSeconds() const noexcept { return positionSeconds_.load(std::memory_order_relaxed); }

    // ---- audio thread ----
    void process(juce::AudioBuffer<float>& buffer)
    {
        ClipData* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_);
            current_ = incoming;
        }

        if (! playing_.load(std::memory_order_acquire) || current_ == nullptr
            || deviceSampleRate_ <= 0.0 || current_->sourceSampleRate <= 0.0)
            return;

        const double sourceRate = current_->sourceSampleRate;
        const int    length     = current_->lengthSamples;
        if (length <= 0)
            return;

        if (restart_.exchange(false, std::memory_order_acquire))
            readPosition_ = startSeconds_.load(std::memory_order_relaxed) * sourceRate;

        // Where to stop, in file samples. An end at or before the start means
        // play to the end of the file.
        const double endSeconds = endSeconds_.load(std::memory_order_relaxed);
        const double startSecs  = startSeconds_.load(std::memory_order_relaxed);
        const double endSample  = endSeconds > startSecs ? std::min((double) length, endSeconds * sourceRate)
                                                         : (double) length;

        const double ratio       = sourceRate / deviceSampleRate_;
        const int    numSamples  = buffer.getNumSamples();
        const int    outChannels = buffer.getNumChannels();
        const int    fileChans   = std::max(1, current_->numChannels);

        for (int i = 0; i < numSamples; ++i)
        {
            if (readPosition_ >= endSample)
            {
                // Reached the end of what was asked for: stop rather than
                // wrap or run on into the rest of the file.
                playing_.store(false, std::memory_order_relaxed);
                break;
            }

            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int    sourceCh = std::min(ch, fileChans - 1);
                const float* samples  = current_->audio.getReadPointer(sourceCh);
                buffer.getWritePointer(ch)[i] += sampleLinear(samples, length, readPosition_);
            }

            readPosition_ += ratio;
        }

        positionSeconds_.store(readPosition_ / sourceRate, std::memory_order_relaxed);
    }

private:
    // One slot each way is enough: submissions are user actions, and a second
    // one arriving before the audio thread has taken the first just replaces
    // it — which is what the caller wanted anyway.
    rt::SpscRingBuffer<ClipData*> inbox_   { 16 }; // message -> audio
    rt::SpscRingBuffer<ClipData*> reclaim_ { 32 }; // audio -> message

    ClipData* current_ = nullptr; // audio-thread owned

    double deviceSampleRate_ = 48000.0;
    double readPosition_     = 0.0; // file samples; audio-thread owned

    std::atomic<bool>   playing_ { false };
    std::atomic<bool>   restart_ { false };
    std::atomic<double> startSeconds_ { 0.0 };
    std::atomic<double> endSeconds_ { 0.0 };
    std::atomic<double> positionSeconds_ { 0.0 };
};

} // namespace looper::engine
