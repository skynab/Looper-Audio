#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/AudioClipSlot.h"
#include "engine/ClipData.h"
#include "engine/ClipSlot.h"
#include "engine/InstrumentTrack.h"
#include "engine/Pattern.h"
#include "engine/ProcessContext.h"
#include "engine/ReverbEffect.h"

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
    /** Given a track index and a beat position, returns that track's gain in dB
        at that beat (the last argument is the track's static gainDb, to return
        as a fallback for tracks with no automation of their own). Deliberately
        JUCE- and model-independent (the engine layer doesn't know what
        "automation" is) — the caller supplies a lambda that reads whatever
        automation representation it uses, e.g. model::AutomationLane::valueAt.
        Pass a default-constructed (empty) one to skip this entirely — every
        track then uses its static gainsDb for the whole render, exactly as
        before this parameter existed. */
    using GainAutomationFn = std::function<float(int trackIndex, double beat, float staticGainDb)>;

    /** Renders one instrument track per pattern, at the given per-track gains (dB),
        solo flags, clip start offsets (beats — the track stays silent until the
        transport reaches this point, then plays and loops indefinitely), and
        pre-fader send levels (0..1) into a shared send-bus reverb (always fully
        wet; returnGain scales the wet return before it's summed into the mix).
        Solo follows the same "solo overrides, mute always wins" rule as the live
        engine.

        If @p gainAutomation is set, each track is rendered in isolation at unity
        gain and folded into the mix with a sample-accurate gain curve from the
        callback instead of InstrumentTrack's flat per-block gain — this is the
        only way to get sample-accurate *per-track* automation, since (unlike
        master automation) it can't be applied as a single post-render multiply
        once tracks are already summed. Leaving it unset keeps the original,
        untouched fast path (a plain per-track render() straight into the mix). */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           const std::vector<bool>&    soloFlags,
                                           const std::vector<double>&  clipStartBeats,
                                           const std::vector<float>&   sendLevels,
                                           bool  sendBusEnabled,
                                           float sendRoomSize,
                                           float sendDamping,
                                           float sendReturnGain,
                                           double bpm,
                                           double sampleRate,
                                           double numSeconds,
                                           int    blockSize = 512,
                                           GainAutomationFn gainAutomation = {})
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        std::vector<std::unique_ptr<InstrumentTrack>> tracks;
        for (size_t i = 0; i < patterns.size(); ++i)
        {
            auto track = std::make_unique<InstrumentTrack>();
            track->prepare(sampleRate, blockSize);

            // One clip per track, given an effectively unbounded length so it
            // keeps looping indefinitely from its start — the same semantics
            // this render() has always modelled (see renderClips() below for
            // genuine multi-clip-per-track scheduling).
            ClipSlot slot;
            slot.pattern     = patterns[i];
            slot.startBeats  = i < clipStartBeats.size() ? clipStartBeats[i] : 0.0;
            slot.lengthBeats = 1.0e9;
            track->sequencer.submitClips(new std::vector<ClipSlot> { slot });

            if (i < gainsDb.size())
                track->gainDb.store(gainsDb[i]);
            if (i < soloFlags.size())
                track->solo.store(soloFlags[i]);
            if (i < sendLevels.size())
                track->sendLevel.store(sendLevels[i]);
            tracks.push_back(std::move(track));
        }

        bool anySolo = false;
        for (auto& track : tracks)
            anySolo |= track->solo.load();

        ReverbEffect sendReverb;
        sendReverb.prepare(sampleRate, blockSize);
        sendReverb.setEnabled(true);
        sendReverb.setMix(1.0f); // a return bus is always fully wet
        sendReverb.setRoomSize(sendRoomSize);
        sendReverb.setDamping(sendDamping);

        juce::AudioBuffer<float> block(2, blockSize);
        juce::AudioBuffer<float> sendBus(2, blockSize);
        juce::AudioBuffer<float> trackTemp; // only sized/used when gainAutomation is set
        juce::MidiBuffer         noLiveMidi;

        const double samplesPerBeat = bpm > 0.0 ? sampleRate * 60.0 / bpm : 0.0;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();
            sendBus.setSize(2, n, false, false, true);
            sendBus.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            ctx.transport.playing         = true;
            ctx.transport.playheadSamples = playhead;
            ctx.transport.bpm             = bpm;

            if (! gainAutomation)
            {
                for (auto& track : tracks)
                    track->render(block, sendBus, noLiveMidi, ctx, false, anySolo);
            }
            else
            {
                trackTemp.setSize(2, n, false, false, true);
                for (size_t t = 0; t < tracks.size(); ++t)
                {
                    auto&       track       = tracks[t];
                    const float staticGainDb = track->gainDb.load();

                    // Render this track alone at unity gain (the send bus still
                    // gets its usual, gain-independent pre-fader copy from
                    // inside render()) so we can fold it into the mix ourselves
                    // with a sample-accurate curve instead of one flat gain.
                    trackTemp.clear();
                    track->gainDb.store(0.0f);
                    track->render(trackTemp, sendBus, noLiveMidi, ctx, false, anySolo);
                    track->gainDb.store(staticGainDb);

                    const int channels = juce::jmin(block.getNumChannels(), trackTemp.getNumChannels());
                    for (int i = 0; i < n; ++i)
                    {
                        const double beat = samplesPerBeat > 0.0 ? (double) (playhead + i) / samplesPerBeat : 0.0;
                        const float  g    = juce::Decibels::decibelsToGain(
                                                gainAutomation((int) t, beat, staticGainDb));
                        for (int ch = 0; ch < channels; ++ch)
                            block.getWritePointer(ch)[i] += trackTemp.getReadPointer(ch)[i] * g;
                    }
                }
            }

            if (sendBusEnabled)
            {
                sendReverb.process(sendBus);
                for (int ch = 0; ch < 2; ++ch)
                    block.addFrom(ch, 0, sendBus, ch, 0, n, sendReturnGain);
            }

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Convenience overload: no send bus. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           const std::vector<bool>&    soloFlags,
                                           const std::vector<double>&  clipStartBeats,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, gainsDb, soloFlags, clipStartBeats, std::vector<float>{},
                      false, 0.5f, 0.5f, 0.0f, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload: no solo flags, no clip-start offsets, no send bus. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           const std::vector<bool>&    soloFlags,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, gainsDb, soloFlags, std::vector<double>{}, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload: no solo flags (no track is ever solo-silenced), no clip-start offsets. */
    static juce::AudioBuffer<float> render(const std::vector<Pattern>& patterns,
                                           const std::vector<float>&   gainsDb,
                                           double bpm, double sampleRate, double numSeconds, int blockSize = 512)
    {
        return render(patterns, gainsDb, std::vector<bool>{}, bpm, sampleRate, numSeconds, blockSize);
    }

    /** Convenience overload: patterns at unity gain, no solo. */
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

    /** Renders a single track from an explicit list of ClipSlots — for verifying
        genuine multi-clip-per-track scheduling (silence between clips, each
        clip's own length gating its end). The render() overloads above still
        model one clip per track (all the current UI can create), always with
        an unbounded length. */
    static juce::AudioBuffer<float> renderClips(const std::vector<ClipSlot>& clips,
                                                double bpm, double sampleRate, double numSeconds,
                                                int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        InstrumentTrack track;
        track.prepare(sampleRate, blockSize);
        track.sequencer.submitClips(new std::vector<ClipSlot>(clips));

        juce::AudioBuffer<float> block(2, blockSize);
        juce::AudioBuffer<float> sendBus(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();
            sendBus.setSize(2, n, false, false, true);
            sendBus.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            ctx.transport.playing         = true;
            ctx.transport.playheadSamples = playhead;
            ctx.transport.bpm             = bpm;

            track.render(block, sendBus, noLiveMidi, ctx, false, false);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Renders a single track's audio-clip player alone (bypassing patterns) at
        the given gain and clip-start offset — for verifying that a decoded
        audio clip plays back through the exact same per-track gain/send/peak
        pipeline as synth content, with the same clip-start gating. */
    static juce::AudioBuffer<float> renderAudioClip(const ClipData& clipData, double clipStartBeats,
                                                    float gainDb, double bpm, double sampleRate,
                                                    double numSeconds, int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        InstrumentTrack track;
        track.prepare(sampleRate, blockSize);
        track.gainDb.store(gainDb);
        track.audioPlayer.submitSingleClip(new ClipData(clipData), clipStartBeats);

        juce::AudioBuffer<float> block(2, blockSize);
        juce::AudioBuffer<float> sendBus(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();
            sendBus.setSize(2, n, false, false, true);
            sendBus.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            ctx.transport.playing         = true;
            ctx.transport.playheadSamples = playhead;
            ctx.transport.bpm             = bpm;

            track.render(block, sendBus, noLiveMidi, ctx, false, false);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom(ch, pos, block, ch, 0, n);

            playhead += n;
        }

        return output;
    }

    /** Renders a single track's audio-clip player from an explicit list of
        AudioClipSlots — for verifying genuine multi-clip-per-track audio
        scheduling (silence between clips, each clip's own length gating when
        it ends even if the file has more samples left). renderAudioClip()
        above still models the single-clip case (unbounded length, no gating
        other than clip-start) — the only case the current UI can create. */
    static juce::AudioBuffer<float> renderAudioClips(const std::vector<AudioClipSlot>& clips,
                                                     float gainDb, double bpm, double sampleRate,
                                                     double numSeconds, int blockSize = 512)
    {
        const int totalSamples = (int) std::ceil(numSeconds * sampleRate);
        juce::AudioBuffer<float> output(2, std::max(1, totalSamples));
        output.clear();

        InstrumentTrack track;
        track.prepare(sampleRate, blockSize);
        track.gainDb.store(gainDb);
        track.audioPlayer.submitClips(new std::vector<AudioClipSlot>(clips));

        juce::AudioBuffer<float> block(2, blockSize);
        juce::AudioBuffer<float> sendBus(2, blockSize);
        juce::MidiBuffer         noLiveMidi;

        int64_t playhead = 0;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);
            block.setSize(2, n, false, false, true);
            block.clear();
            sendBus.setSize(2, n, false, false, true);
            sendBus.clear();

            ProcessContext ctx;
            ctx.sampleRate                = sampleRate;
            ctx.numSamples                = n;
            ctx.transport.playing         = true;
            ctx.transport.playheadSamples = playhead;
            ctx.transport.bpm             = bpm;

            track.render(block, sendBus, noLiveMidi, ctx, false, false);

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
