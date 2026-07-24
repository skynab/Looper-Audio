#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/InstrumentTrack.h"
#include "engine/Pattern.h"
#include "engine/ProcessContext.h"

namespace looper::engine
{
/**
    Renders patterns to audio offline (no audio device), reusing the exact same
    InstrumentTrack render path the live engine uses. This is what "Bounce" and
    the headless bounce tool are built on — and the first way the audio output can
    be inspected without hardware.
*/
class OfflineRenderer
{
public:
    /** Renders one instrument track per pattern, at the given per-track gains (dB), summed. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           double bpm,
                                           double sampleRate,
                                           double numSeconds,
                                           int    blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        std::vector<std::unique_ptr<InstrumentTrack>> tracks;
        for (size_t i = 0; i < patterns.size(); ++i)
        {
            auto track = std::make_unique<InstrumentTrack>();
            track->prepare(sampleRate, blockSize);
            track->sequencer.submitPattern(new Pattern(patterns[i]));
            if (i < gainsDb.size())
                track->gainDb.store(gainsDb[i]);
            tracks.push_back(std::move(track));
        }

        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            ctx.transport.playing         = true;
            ctx.transport.playheadSamples = playhead;
            ctx.transport.bpm             = bpm;

            for (auto& track : tracks)
                track->render(block, noLiveMidi, ctx, false);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Convenience overload: patterns at unity gain. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns, double bpm,
                                           double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, std::vector<float>(patterns.size(), 0.0f), bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload for a single pattern. */
    static juce::AudioBuffer<float> render(const Pattern& pattern, double bpm,
                                           double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(std::vector<Pattern> { pattern }, bpm, sampleRate, numSeconds, blockSize);
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
