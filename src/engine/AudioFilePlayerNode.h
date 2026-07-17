#pragma once

#include "engine/ClipData.h"
#include "engine/Interpolation.h"
#include "engine/Node.h"
#include "rt/SpscRingBuffer.h"

namespace looper::engine
{
/**
    Plays an in-RAM audio clip, slaved to the transport.

    The clip sits at timeline position 0, so its read position is derived from the
    transport playhead every block — which makes seeking and looping follow for
    free. File/device sample-rate differences are corrected with linear
    interpolation.

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

        if (current_ == nullptr || ! context.transport.playing || deviceSampleRate_ <= 0.0)
            return;

        const double ratio      = current_->sourceSampleRate > 0.0
                                      ? current_->sourceSampleRate / deviceSampleRate_
                                      : 1.0;
        const int    length     = current_->lengthSamples;
        const int    fileChans  = current_->numChannels;
        const int    outChans   = buffer.getNumChannels();
        const int    numSamples = buffer.getNumSamples();

        double position = (double) context.transport.playheadSamples * ratio;

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

    rt::SpscRingBuffer<ClipData*> inbox_   { 16 }; // message -> audio
    rt::SpscRingBuffer<ClipData*> reclaim_ { 32 }; // audio -> message
};

} // namespace looper::engine
