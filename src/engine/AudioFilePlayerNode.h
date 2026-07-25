#pragma once

#include <atomic>

#include "engine/ClipData.h"
#include "engine/Interpolation.h"
#include "engine/Node.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Plays an in-RAM audio clip, slaved to the transport, starting at a given
    clip-start offset (in beats) on the timeline — the same "delays when it
    begins" gating Sequencer applies to MIDI clips. Playback does not loop: once
    the file's samples run out it stays silent (the usual behaviour for a
    one-shot audio clip, unlike a looping MIDI pattern). File/device sample-rate
    differences are corrected with linear interpolation.

    Clip hand-off is lock-free and allocation-free on the audio thread:
      - message thread decodes a file into a ClipData and submits the pointer,
      - the audio thread swaps it in and returns the retired clip via a second
        FIFO for the message thread to delete.
*/
class AudioFilePlayerNode final : public Node
{
public:
    ~AudioFilePlayerNode() override
    {
        // Audio is stopped by the time the node is destroyed: safe to free here.
        collectRetiredClips();
        delete current_;

        ClipData* straggler = nullptr;
        while (inbox_.pop(straggler))
            delete straggler;
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override { deviceSampleRate_ = sampleRate; }

    // ---- message thread ----
    /** Hands ownership of @p clip to the audio thread. Deletes it here if the inbox is full. */
    void submitClip(ClipData* clip)
    {
        if (! inbox_.push(clip))
            delete clip;
    }

    void setClipStartBeats(double beats) { clipStartBeats_.store(beats, std::memory_order_relaxed); }

    /** Frees clips the audio thread has retired. Call periodically from the message thread. */
    void collectRetiredClips()
    {
        ClipData* retired = nullptr;
        while (reclaim_.pop(retired))
            delete retired;
    }

    // ---- audio thread ----
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/, const ProcessContext& context) override
    {
        ClipData* incoming = nullptr;
        while (inbox_.pop(incoming))
        {
            if (current_ != nullptr)
                reclaim_.push(current_); // rare drop-on-full leaks until dtor — acceptable here
            current_ = incoming;
        }

        if (current_ == nullptr || ! context.transport.playing || deviceSampleRate_ <= 0.0
            || context.transport.bpm <= 0.0)
            return;

        const double ratio      = current_->sourceSampleRate > 0.0
                                      ? current_->sourceSampleRate / deviceSampleRate_
                                      : 1.0;
        const int    length     = current_->lengthSamples;
        const int    fileChans  = current_->numChannels;
        const int    outChans   = buffer.getNumChannels();
        const int    numSamples = buffer.getNumSamples();

        // Gate on the clip's start beat, exactly like Sequencer's clip-start
        // gating: silent until the transport reaches it, in device-sample
        // terms, then converted to a file-local (possibly resampled) position.
        const double samplesPerBeat        = context.sampleRate * 60.0 / context.transport.bpm;
        const double clipStartDeviceSample = clipStartBeats_.load(std::memory_order_relaxed) * samplesPerBeat;
        const double localDeviceSample     = (double) context.transport.playheadSamples - clipStartDeviceSample;

        if (localDeviceSample + (double) numSamples <= 0.0)
            return; // the whole block is before this clip starts

        double position = localDeviceSample * ratio;

        for (int i = 0; i < numSamples; ++i)
        {
            if (position >= 0.0 && position < (double) length)
            {
                for (int ch = 0; ch < outChans; ++ch)
                {
                    const int    srcCh  = juce::jmin(ch, fileChans - 1);
                    const float* srcPtr = current_->audio.getReadPointer(srcCh);
                    buffer.getWritePointer(ch)[i] += sampleLinear(srcPtr, length, position);
                }
            }

            position += ratio;
        }
    }

private:
    double    deviceSampleRate_ = 0.0;
    ClipData* current_          = nullptr;      // audio-thread owned
    std::atomic<double> clipStartBeats_ { 0.0 };

    rt::SpscRingBuffer<ClipData*> inbox_   { 16 }; // message -> audio
    rt::SpscRingBuffer<ClipData*> reclaim_ { 32 }; // audio -> message
};

} // namespace looper::engine
