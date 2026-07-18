#pragma once

#include <algorithm>
#include <cmath>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/Pattern.h"
#include "engine/ProcessContext.h"
#include "engine/Sequencer.h"
#include "engine/SynthInstrumentNode.h"

namespace looper::engine
{
/**
    Renders a pattern to audio offline (no audio device), reusing the exact same
    synth + sequencer the live engine uses. This is what the "Bounce" export and
    the headless bounce tool are built on — and it's the first path that lets the
    audio output be inspected without hardware.
*/
class OfflineRenderer
{
public:
    static juce::AudioBuffer<float> render(const Pattern& pattern,
                                           double bpm,
                                           double sampleRate,
                                           double numSeconds,
                                           int    blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        SynthInstrumentNode synth;
        synth.prepare(sampleRate, blockSize);

        Sequencer sequencer;
        sequencer.submitPattern(new Pattern(pattern));

        juce::MidiBuffer          midi;
        juce::AudioBuffer<float>  block(2, blockSize);

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();
            midi.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            ctx.transport.playing         = true;
            ctx.transport.playheadSamples = playhead;
            ctx.transport.bpm             = bpm;

            sequencer.renderBlock(midi, ctx);
            synth.process(block, midi, ctx);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Writes a buffer to a 24-bit WAV. Returns false on failure. */
    static bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        file.deleteFile();

        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer(
            format.createWriterFor(stream.get(), sampleRate,
                                   (unsigned int) buffer.getNumChannels(), 24, {}, 0));
        if (writer == nullptr)
            return false;

        stream.release(); // the writer now owns the stream
        return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    }
};

} // namespace looper::engine
