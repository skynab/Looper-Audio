#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/AudioClipSlot.h"
#include "engine/AudioRecorder.h"
#include "engine/ClipData.h"
#include "engine/ClipSlot.h"
#include "engine/DelayEffect.h"
#include "engine/DrumKitNode.h"
#include "engine/FilterEffect.h"
#include "engine/Metronome.h"
#include "engine/MidiFileIO.h"
#include "engine/OfflineRenderer.h"
#include "engine/ReverbEffect.h"
#include "model/AutomationLane.h"
#include "model/Song.h"

// Headless bounce: renders a demo arpeggio to a WAV so the synth + sequencer
// audio path can be verified without an audio device. Also usable as a smoke test.
int main(int argc, char** argv)
{
    using namespace looper::engine;

    // AudioRecorder check: feeds synthetic "input" directly into process() —
    // there's no live microphone in this headless verification, so this can
    // only confirm the capture + armed/finished handoff logic is correct
    // bit-for-bit, not that real hardware input reaches the callback.
    bool recorderWorks = false;
    {
        const double recSampleRate = 44100.0;
        const int    blockSize     = 512;

        std::vector<float> inputBlock((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
            inputBlock[(size_t) i] = (float) i / (float) blockSize; // a ramp, easy to verify exactly
        const float* channelPtrs[1] = { inputBlock.data() };

        AudioRecorder recorder;
        recorder.prepare(recSampleRate, 1, 1.0); // 1 s capacity, mono

        // Not armed yet: must not capture, even while "playing".
        recorder.process(channelPtrs, 1, blockSize, true);
        const bool capturesNothingWhenDisarmed = recorder.recordedSampleCount() == 0;

        // Arm and record 3 blocks while playing.
        recorder.arm();
        recorder.process(channelPtrs, 1, blockSize, true);
        recorder.process(channelPtrs, 1, blockSize, true);
        recorder.process(channelPtrs, 1, blockSize, true);
        const bool capturedThreeBlocks       = recorder.recordedSampleCount() == blockSize * 3;
        const bool notFinishedWhileRecording = ! recorder.isFinished();

        // Disarm; the *next* process() call is what finalizes the take (and is
        // itself not captured, since it's already disarmed by then).
        recorder.disarm();
        recorder.process(channelPtrs, 1, blockSize, true);
        const bool finishedAfterDisarm        = recorder.isFinished();
        const bool lengthUnchangedAfterDisarm = recorder.takeLength() == blockSize * 3;

        bool contentMatches = true;
        const float* captured = recorder.takeBuffer().getReadPointer(0);
        for (int block = 0; block < 3 && contentMatches; ++block)
            for (int i = 0; i < blockSize; ++i)
                if (std::abs(captured[block * blockSize + i] - inputBlock[(size_t) i]) > 1.0e-7f)
                    contentMatches = false;

        // Capacity check: recording longer than the prepared capacity must cap
        // safely (no overflow/crash), keeping only what fits.
        AudioRecorder capRecorder;
        capRecorder.prepare(recSampleRate, 1, 0.001); // ~44 samples of capacity
        capRecorder.arm();
        capRecorder.process(channelPtrs, 1, blockSize, true);
        capRecorder.process(channelPtrs, 1, blockSize, true);
        capRecorder.disarm();
        capRecorder.process(channelPtrs, 1, blockSize, true);
        const bool capacityCapped = capRecorder.isFinished()
                                 && capRecorder.takeLength() > 0 && capRecorder.takeLength() <= 45;

        recorderWorks = capturesNothingWhenDisarmed && capturedThreeBlocks && notFinishedWhileRecording
                     && finishedAfterDisarm && lengthUnchangedAfterDisarm && contentMatches && capacityCapped;
    }

    const double bpm        = 120.0;
    const double sampleRate = 44100.0;
    const double seconds    = 4.0;

    // Track 1: C-E-G-C arpeggio, one note per beat (the piano-roll demo).
    Pattern arp;
    arp.lengthBeats = 4.0;
    const int root     = 60;
    const int arpNotes[] = { 0, 4, 7, 12 };
    for (int i = 0; i < 4; ++i)
        arp.notes.push_back({ (double) i, 0.5, root + arpNotes[i], 0.8f });

    // Track 2: a simple root-note bass on beats 1 and 3, to exercise track summing.
    Pattern bass;
    bass.lengthBeats = 4.0;
    bass.notes.push_back({ 0.0, 1.0, 36, 0.9f });
    bass.notes.push_back({ 2.0, 1.0, 43, 0.9f });

    // The written file is the full two-track mix at unity gain.
    const auto buffer = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, bpm, sampleRate, seconds);

    // Per-track gain check: -6 dB should roughly halve the amplitude (10^(-6/20) ~= 0.501).
    const std::vector<Pattern> one { arp };
    const auto  full      = OfflineRenderer::render(one, std::vector<float> { 0.0f },  bpm, sampleRate, seconds);
    const auto  quiet     = OfflineRenderer::render(one, std::vector<float> { -6.0f }, bpm, sampleRate, seconds);
    const float rmsFull   = full.getRMSLevel(0, 0, full.getNumSamples());
    const float rmsQuiet  = quiet.getRMSLevel(0, 0, quiet.getNumSamples());
    const float gainRatio = rmsFull > 0.0f ? rmsQuiet / rmsFull : 0.0f;

    // Solo check: soloing track 1 (arp) must fully silence track 2 (bass), even
    // though bass is active and unmuted — the render should then match an
    // arp-only render (rmsFull, computed above) rather than the full mix.
    const auto  soloArp            = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, { true, false },
                                                             bpm, sampleRate, seconds);
    const float rmsSoloArp         = soloArp.getRMSLevel(0, 0, soloArp.getNumSamples());
    const bool  soloMatchesArpOnly = std::abs(rmsSoloArp - rmsFull) < 1.0e-4f;

    // Clip-start check: delaying a track's clip start by 2 beats (1s at 120bpm)
    // must produce silence before that point and real signal after it.
    const auto  delayedStart      = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                            std::vector<double> { 2.0 }, bpm, sampleRate, seconds);
    const int   oneSecondSamples  = (int) sampleRate;
    const float rmsBeforeStart    = delayedStart.getRMSLevel(0, 0, oneSecondSamples);
    const float rmsAfterStart     = delayedStart.getRMSLevel(0, oneSecondSamples,
                                                             delayedStart.getNumSamples() - oneSecondSamples);
    const bool  clipStartGates    = rmsBeforeStart < 1.0e-5f && rmsAfterStart > 0.01f;

    // Send-bus check: with a track sending fully into the bus, enabling the
    // send-bus reverb must change the output relative to the send bus being off.
    const auto  noSendBus   = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                      std::vector<double>{}, std::vector<float> { 1.0f },
                                                      false, 0.6f, 0.4f, 0.7f, bpm, sampleRate, seconds);
    const auto  withSendBus = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                      std::vector<double>{}, std::vector<float> { 1.0f },
                                                      true, 0.6f, 0.4f, 0.7f, bpm, sampleRate, seconds);
    const float rmsNoSendBus   = noSendBus.getRMSLevel(0, 0, noSendBus.getNumSamples());
    const float rmsWithSendBus = withSendBus.getRMSLevel(0, 0, withSendBus.getNumSamples());
    const bool  sendBusChanged = std::abs(rmsWithSendBus - rmsNoSendBus) > 1.0e-4f;

    // Send-bus DELAY check: same setup, but with the bus's effect type set to
    // Delay (1) instead of the default Reverb (0) — must differ from both
    // "bus off" and the reverb-based bus above, confirming the engine
    // genuinely switches which effect processes the send bus.
    const auto  withSendBusDelay     = OfflineRenderer::render(one, std::vector<float> { 0.0f }, std::vector<bool>{},
                                                               std::vector<double>{}, std::vector<float> { 1.0f },
                                                               true, 0.6f, 0.4f, 0.7f, bpm, sampleRate, seconds,
                                                               512, OfflineRenderer::GainAutomationFn {},
                                                               1, 250.0f, 0.4f);
    const float rmsWithSendBusDelay  = withSendBusDelay.getRMSLevel(0, 0, withSendBusDelay.getNumSamples());
    const bool  sendBusDelayWorks    = std::abs(rmsWithSendBusDelay - rmsNoSendBus) > 1.0e-4f
                                     && std::abs(rmsWithSendBusDelay - rmsWithSendBus) > 1.0e-4f;

    // Multi-clip check: two clips on one track (0-4 beats, then 6-10 beats,
    // leaving a 2-beat gap and nothing after) must produce sound only inside
    // each clip's own window — real length gating, not the single-clip
    // "loop forever" case checked above. The synth has a 250ms ADSR release
    // tail, so "silence" is checked from 0.5s into the gap/tail onward, well
    // past any legitimate release decay from the last note (whose own note-off
    // already fires before the clip boundary).
    ClipSlot clipA; clipA.pattern = arp; clipA.startBeats = 0.0; clipA.lengthBeats = 4.0;
    ClipSlot clipB; clipB.pattern = arp; clipB.startBeats = 6.0; clipB.lengthBeats = 4.0;
    const auto multiClip = OfflineRenderer::renderClips({ clipA, clipB }, bpm, sampleRate, 6.0);

    const int   halfSec       = (int) sampleRate / 2;
    const float rmsClipA      = multiClip.getRMSLevel(0, 0 * halfSec, 4 * halfSec); // 0-2s: clip A
    const float rmsGap        = multiClip.getRMSLevel(0, 5 * halfSec, 1 * halfSec); // 2.5-3s: late in the gap
    const float rmsClipB      = multiClip.getRMSLevel(0, 6 * halfSec, 4 * halfSec); // 3-5s: clip B
    const float rmsTail       = multiClip.getRMSLevel(0, 11 * halfSec, 1 * halfSec); // 5.5-6s: late in the tail
    const bool  multiClipGates = rmsClipA > 0.01f && rmsGap < 1.0e-5f
                              && rmsClipB > 0.01f && rmsTail < 1.0e-5f;

    // Audio-clip-track check: a decoded audio clip (a plain 440 Hz tone, no
    // synth involved) must play back through a track's audioPlayer, going
    // through the exact same gain pipeline as synth content, with the same
    // clip-start gating. This is the first time a per-track audio clip is
    // actually audible — previously TrackType::Audio tracks were silently inert.
    ClipData sineClip;
    {
        const int n = (int) (2.0 * sampleRate); // 2-second tone
        sineClip.audio.setSize(1, n);
        sineClip.sourceSampleRate = sampleRate;
        sineClip.numChannels      = 1;
        sineClip.lengthSamples    = n;
        float* data = sineClip.audio.getWritePointer(0);
        for (int i = 0; i < n; ++i)
            data[i] = 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate);
    }

    const auto  audioFull       = OfflineRenderer::renderAudioClip(sineClip, 0.0, 0.0f, bpm, sampleRate, 4.0);
    const auto  audioQuiet      = OfflineRenderer::renderAudioClip(sineClip, 0.0, -6.0f, bpm, sampleRate, 4.0);
    const float rmsAudioFull    = audioFull.getRMSLevel(0, 0, audioFull.getNumSamples());
    const float rmsAudioQuiet   = audioQuiet.getRMSLevel(0, 0, audioQuiet.getNumSamples());
    const float audioGainRatio  = rmsAudioFull > 0.0f ? rmsAudioQuiet / rmsAudioFull : 0.0f;

    // Same clip, started 2 beats in (1s at 120bpm): silent before, sounding after.
    const auto  audioDelayed        = OfflineRenderer::renderAudioClip(sineClip, 2.0, 0.0f, bpm, sampleRate, 4.0);
    const float rmsBeforeAudioStart = audioDelayed.getRMSLevel(0, 0, oneSecondSamples);
    const float rmsAfterAudioStart  = audioDelayed.getRMSLevel(0, oneSecondSamples,
                                                               audioDelayed.getNumSamples() - oneSecondSamples);

    const bool audioTrackWorks = rmsAudioFull > 0.01f
                              && audioGainRatio > 0.47f && audioGainRatio < 0.53f
                              && rmsBeforeAudioStart < 1.0e-5f && rmsAfterAudioStart > 0.01f;

    // Multi-clip audio check: two clips on one track (0-4 beats, then 6-10
    // beats, a 2-beat gap, nothing after) must sound only inside each clip's
    // own window — the audio equivalent of multiClipGates above, exercising
    // AudioFilePlayerNode's clip-list scheduling for the first time (a track
    // with one audio clip has always had an unbounded window; this is the
    // first genuine per-clip audio length gating). Audio has no envelope
    // tail (unlike the synth), so it can go silent right at each boundary.
    AudioClipSlot audioClipA;
    audioClipA.clipData    = std::make_shared<ClipData>(sineClip);
    audioClipA.startBeats  = 0.0;
    audioClipA.lengthBeats = 4.0;
    AudioClipSlot audioClipB;
    audioClipB.clipData    = std::make_shared<ClipData>(sineClip);
    audioClipB.startBeats  = 6.0;
    audioClipB.lengthBeats = 4.0;

    const auto  multiAudioClip     = OfflineRenderer::renderAudioClips({ audioClipA, audioClipB },
                                                                       0.0f, bpm, sampleRate, 6.0);
    const float rmsAudioClipA      = multiAudioClip.getRMSLevel(0, 0 * halfSec, 4 * halfSec); // 0-2s: clip A
    const float rmsAudioGap        = multiAudioClip.getRMSLevel(0, 5 * halfSec, 1 * halfSec); // 2.5-3s: the gap
    const float rmsAudioClipB      = multiAudioClip.getRMSLevel(0, 6 * halfSec, 4 * halfSec); // 3-5s: clip B
    const bool  multiClipAudioGates = rmsAudioClipA > 0.01f && rmsAudioGap < 1.0e-5f && rmsAudioClipB > 0.01f;

    // MIDI import/export round-trip check: build a Song with three notes on
    // one track, export it to a temp .mid, re-import it into a fresh Song,
    // and confirm every note (and the tempo) survived — beat/pitch/velocity
    // within the rounding tolerance a real tick-based file format implies.
    bool midiRoundTripWorks = false;
    {
        using namespace looper::model;

        Song original;
        original.bpm = 128.0;
        auto& track = addTrack(original, TrackType::Instrument, "Test");
        Clip  clip;
        clip.id                  = allocateId(original);
        clip.type                = ClipType::Instrument;
        clip.startBeats          = 0.0;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        clip.pattern.notes.push_back({ 0.0, 0.5, 60, 0.8f });
        clip.pattern.notes.push_back({ 1.0, 1.0, 64, 0.6f });
        clip.pattern.notes.push_back({ 2.5, 0.25, 67, 1.0f });
        track.clips.push_back(clip);

        const juce::File midiTemp = juce::File::getCurrentWorkingDirectory().getChildFile("midi_roundtrip_test.mid");
        const bool       exportOk = exportMidiFile(midiTemp, original);

        Song reimported;
        reimported.bpm            = 90.0; // deliberately different, so import setting it is actually verified
        const auto importResult   = importMidiFile(midiTemp, reimported);
        midiTemp.deleteFile();

        midiRoundTripWorks = exportOk && importResult.ok && importResult.tracksImported == 1
                          && importResult.extraTempoEventsIgnored == 0
                          && std::abs(reimported.bpm - 128.0) < 0.5
                          && reimported.tracks.size() == 1
                          && reimported.tracks[0].clips.size() == 1;

        if (midiRoundTripWorks)
        {
            const auto& notes = reimported.tracks[0].clips[0].pattern.notes;
            midiRoundTripWorks = notes.size() == 3
                && std::abs(notes[0].startBeats - 0.0) < 0.01 && std::abs(notes[0].lengthBeats - 0.5) < 0.01
                && notes[0].noteNumber == 60 && std::abs(notes[0].velocity - 0.8f) < 0.01f
                && std::abs(notes[1].startBeats - 1.0) < 0.01 && std::abs(notes[1].lengthBeats - 1.0) < 0.01
                && notes[1].noteNumber == 64 && std::abs(notes[1].velocity - 0.6f) < 0.01f
                && std::abs(notes[2].startBeats - 2.5) < 0.01 && std::abs(notes[2].lengthBeats - 0.25) < 0.01
                && notes[2].noteNumber == 67 && std::abs(notes[2].velocity - 1.0f) < 0.01f;
        }
    }

    // Drum-kit check: a kick+snare pattern (kick on beats 0/2, snare on
    // beats 1/3, at 120bpm = 0.5s/beat) driven through DrumKitNode via the
    // normal sequencer path — each hit plays its full one-shot sample
    // regardless of the note's own lengthBeats (note-off is ignored unless
    // it's a hard stop; see DrumSampleVoice::stopNote), and an unassigned
    // pad (Hat, note 42, triggered but given no sample) must stay silent.
    ClipData drumHit;
    {
        const int n = (int) (0.15 * sampleRate); // short one-shot, well under the 0.5s gap between hits
        drumHit.audio.setSize(1, n);
        drumHit.sourceSampleRate = sampleRate;
        drumHit.numChannels      = 1;
        drumHit.lengthSamples    = n;
        float* data = drumHit.audio.getWritePointer(0);
        for (int i = 0; i < n; ++i)
            data[i] = 0.6f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / sampleRate);
    }

    Pattern drumPattern;
    drumPattern.lengthBeats = 4.0;
    drumPattern.notes.push_back({ 0.0, 0.1, 36, 1.0f }); // kick, beat 0
    drumPattern.notes.push_back({ 1.0, 0.1, 38, 1.0f }); // snare, beat 1
    drumPattern.notes.push_back({ 2.0, 0.1, 36, 1.0f }); // kick, beat 2
    drumPattern.notes.push_back({ 3.0, 0.1, 38, 1.0f }); // snare, beat 3
    drumPattern.notes.push_back({ 0.5, 0.1, 42, 1.0f }); // hat, no sample assigned — must stay silent

    std::vector<DrumPadAssignment> drumPads;
    drumPads.push_back({ 36, std::make_shared<ClipData>(drumHit) });
    drumPads.push_back({ 38, std::make_shared<ClipData>(drumHit) });
    drumPads.push_back({ 42, nullptr }); // deliberately unassigned

    const auto  drumBuffer = OfflineRenderer::renderDrumPattern(drumPads, drumPattern, bpm, sampleRate, 2.0);
    const int   shortWin   = (int) (0.1 * sampleRate);
    const float rmsKick1   = drumBuffer.getRMSLevel(0, 0, shortWin);
    const float rmsSnare1  = drumBuffer.getRMSLevel(0, (int) (0.5 * sampleRate), shortWin);
    const float rmsKick2   = drumBuffer.getRMSLevel(0, (int) (1.0 * sampleRate), shortWin);
    const float rmsSnare2  = drumBuffer.getRMSLevel(0, (int) (1.5 * sampleRate), shortWin);
    // Past kick1's 0.15s tone but before snare1 (0.5s), and overlapping
    // where the unassigned hat note fires (0.25s) — silence here confirms
    // both "the hit ended" and "the unassigned pad produced nothing".
    const float rmsGapAndHat = drumBuffer.getRMSLevel(0, (int) (0.3 * sampleRate), shortWin);

    const bool drumKitWorks = rmsKick1 > 0.01f && rmsSnare1 > 0.01f
                          && rmsKick2 > 0.01f && rmsSnare2 > 0.01f
                          && rmsGapAndHat < 1.0e-5f;

    // Per-pad mix check: the same pattern again, but with the kick pulled
    // down 6dB and panned hard left, and the snare muted. Verifies each of
    // gain/pan/mute independently against the un-mixed render above.
    bool drumPadMixWorks = false;
    {
        std::vector<DrumPadAssignment> mixedPads;

        DrumPadAssignment kick;
        kick.noteNumber = 36;
        kick.clipData   = std::make_shared<ClipData>(drumHit);
        kick.gain       = juce::Decibels::decibelsToGain(-6.0f);
        kick.pan        = -1.0f; // hard left
        mixedPads.push_back(std::move(kick));

        DrumPadAssignment snare;
        snare.noteNumber = 38;
        snare.clipData   = std::make_shared<ClipData>(drumHit);
        snare.muted      = true;
        mixedPads.push_back(std::move(snare));

        const auto  mixed          = OfflineRenderer::renderDrumPattern(mixedPads, drumPattern, bpm, sampleRate, 2.0);
        const float mixedKickLeft  = mixed.getRMSLevel(0, 0, shortWin);
        const float mixedKickRight = mixed.getRMSLevel(1, 0, shortWin);
        const float mixedSnare     = mixed.getRMSLevel(0, (int) (0.5 * sampleRate), shortWin);

        // -6dB is a ~0.5 amplitude ratio against the same hit rendered flat.
        const float gainRatioKick = rmsKick1 > 0.0f ? mixedKickLeft / rmsKick1 : 0.0f;

        drumPadMixWorks = gainRatioKick > 0.47f && gainRatioKick < 0.53f // gain applied
                       && mixedKickRight < 1.0e-5f                        // panned fully off the right
                       && mixedKickLeft > 0.01f                           // ...but still present on the left
                       && mixedSnare < 1.0e-5f;                           // muted pad is silent
    }

    // Per-pad pitch check: the same one-shot transposed up an octave must
    // read through the sample twice as fast, so it ends around half as far
    // in — audible as a shorter, higher hit.
    bool drumPadPitchWorks = false;
    {
        std::vector<DrumPadAssignment> pitchedPads;
        DrumPadAssignment kick;
        kick.noteNumber = 36;
        kick.clipData   = std::make_shared<ClipData>(drumHit);
        kick.pitchRatio = 2.0f; // +12 semitones
        pitchedPads.push_back(std::move(kick));

        Pattern onlyKick;
        onlyKick.lengthBeats = 4.0;
        onlyKick.notes.push_back({ 0.0, 0.1, 36, 1.0f });

        const auto  pitched = OfflineRenderer::renderDrumPattern(pitchedPads, onlyKick, bpm, sampleRate, 1.0);
        const int   win     = (int) (0.02 * sampleRate);
        // The 0.15s source is consumed in ~0.075s at double speed: still
        // sounding just before that, silent just after.
        const float before  = pitched.getRMSLevel(0, (int) (0.05 * sampleRate), win);
        const float after   = pitched.getRMSLevel(0, (int) (0.09 * sampleRate), win);

        drumPadPitchWorks = before > 0.01f && after < 1.0e-5f;
    }

    // Metronome check: at 120bpm a beat lands every 0.5s, so the click must
    // be audible right at each beat and silent between them. Also confirms
    // the accent logic runs without disturbing the beat grid.
    bool metronomeWorks = false;
    {
        Metronome metronome;
        metronome.prepare(sampleRate);
        metronome.setEnabled(true);
        metronome.setLevel(1.0f);

        const int totalSamples = (int) (sampleRate * 2.0);
        juce::AudioBuffer<float> clickBuffer(2, totalSamples);
        clickBuffer.clear();

        constexpr int blockSize = 512;
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = std::min(blockSize, totalSamples - pos);

            ProcessContext context;
            context.sampleRate                 = sampleRate;
            context.numSamples                 = n;
            context.transport.playing          = true;
            context.transport.playheadSamples  = pos;
            context.transport.bpm              = bpm;
            context.transport.timeSigNumerator = 4;
            context.transport.timeSigDenominator = 4;

            // A view onto this block of the output, so the click accumulates
            // into one buffer exactly as it does in the live callback.
            juce::AudioBuffer<float> blockView(clickBuffer.getArrayOfWritePointers(), 2, pos, n);
            metronome.process(blockView, context, false);
        }

        const int   win        = (int) (0.02 * sampleRate);
        const float atBeat0    = clickBuffer.getRMSLevel(0, 0, win);
        const float atBeat1    = clickBuffer.getRMSLevel(0, (int) (0.5 * sampleRate), win);
        const float betweenHit = clickBuffer.getRMSLevel(0, (int) (0.25 * sampleRate), win);

        metronomeWorks = atBeat0 > 0.01f && atBeat1 > 0.01f && betweenHit < 1.0e-6f;
    }

    // ...and that a disabled metronome is completely silent, which is what
    // keeps it out of an export (OfflineRenderer has no metronome at all, so
    // a bounce can never contain one — this guards the live path).
    bool metronomeSilentWhenOff = false;
    {
        Metronome metronome;
        metronome.prepare(sampleRate);
        metronome.setEnabled(false);

        const int totalSamples = (int) (sampleRate * 1.0);
        juce::AudioBuffer<float> quiet(2, totalSamples);
        quiet.clear();

        ProcessContext context;
        context.sampleRate                   = sampleRate;
        context.numSamples                   = totalSamples;
        context.transport.playing            = true;
        context.transport.playheadSamples    = 0;
        context.transport.bpm                = bpm;
        context.transport.timeSigNumerator   = 4;
        context.transport.timeSigDenominator = 4;
        metronome.process(quiet, context, false);

        metronomeSilentWhenOff = quiet.getRMSLevel(0, 0, totalSamples) < 1.0e-9f;
    }

    const juce::File out = juce::File::getCurrentWorkingDirectory()
                               .getChildFile(argc > 1 ? argv[1] : "bounce.wav");

    // Delay check: applying the master delay must change the signal.
    juce::AudioBuffer<float> wet(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        wet.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    DelayEffect delay;
    delay.prepare(sampleRate, 512);
    delay.setEnabled(true);
    delay.setTimeMs(250.0f);
    delay.setFeedback(0.4f);
    delay.setMix(0.5f);
    delay.process(wet);

    const float rmsDry       = buffer.getRMSLevel(0, 0, buffer.getNumSamples());
    const float rmsWet       = wet.getRMSLevel(0, 0, wet.getNumSamples());
    const bool  delayChanged = std::abs(rmsWet - rmsDry) > 1.0e-4f;

    // Filter check: a low-pass well below the note content should reduce the level.
    juce::AudioBuffer<float> filtered(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        filtered.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    FilterEffect filter;
    filter.prepare(sampleRate, 512);
    filter.setEnabled(true);
    filter.setMode(0); // low-pass
    filter.setCutoff(150.0f);
    filter.setResonance(0.707f);
    filter.process(filtered);

    const float rmsFiltered      = filtered.getRMSLevel(0, 0, filtered.getNumSamples());
    const bool  filterAttenuates = rmsFiltered < rmsDry;

    // Reverb check: enabling reverb must change the signal.
    juce::AudioBuffer<float> reverbed(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        reverbed.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    ReverbEffect reverb;
    reverb.prepare(sampleRate, 512);
    reverb.setEnabled(true);
    reverb.setRoomSize(0.7f);
    reverb.setDamping(0.4f);
    reverb.setMix(0.4f);
    reverb.process(reverbed);

    const float rmsReverbed  = reverbed.getRMSLevel(0, 0, reverbed.getNumSamples());
    const bool  reverbChanged = std::abs(rmsReverbed - rmsDry) > 1.0e-4f;

    // Automation check: a -40 dB -> 0 dB master-gain ramp should fade the clip in.
    juce::AudioBuffer<float> automated(buffer.getNumChannels(), buffer.getNumSamples());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        automated.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    looper::model::AutomationLane lane;
    lane.addPoint(0.0, -40.0f);
    lane.addPoint(bpm / 60.0 * seconds, 0.0f);

    const double samplesPerBeat = sampleRate * 60.0 / bpm;
    for (int i = 0; i < automated.getNumSamples(); ++i)
    {
        const double beat = (double) i / samplesPerBeat;
        const float  g    = juce::Decibels::decibelsToGain(lane.valueAt(beat, 0.0f));
        for (int ch = 0; ch < automated.getNumChannels(); ++ch)
            automated.getWritePointer(ch)[i] *= g;
    }

    const int   half            = automated.getNumSamples() / 2;
    const float rmsFirstHalf    = automated.getRMSLevel(0, 0, half);
    const float rmsSecondHalf   = automated.getRMSLevel(0, half, automated.getNumSamples() - half);
    const bool  automationFades = rmsFirstHalf < rmsSecondHalf;

    // Per-track automation check: unlike the master-gain trick above (a plain
    // post-render multiply, since master gain applies uniformly to the whole
    // mix), per-track automation can't be applied after tracks are already
    // summed — this exercises the real OfflineRenderer::render(gainAutomation)
    // path. Track 1 (arp) gets a -40 dB -> 0 dB fade; track 2 (bass) gets none.
    looper::model::AutomationLane arpAutomation;
    arpAutomation.addPoint(0.0, -40.0f);
    arpAutomation.addPoint(bpm / 60.0 * seconds, 0.0f);
    looper::model::AutomationLane noAutomation; // empty: bass keeps its static gain

    const std::vector<looper::model::AutomationLane> perTrackLanes { arpAutomation, noAutomation };
    OfflineRenderer::GainAutomationFn perTrackGainFn =
        [&perTrackLanes](int trackIndex, double beat, float staticGainDb) -> float
    {
        if (trackIndex < 0 || (size_t) trackIndex >= perTrackLanes.size())
            return staticGainDb;
        const auto& trackLane = perTrackLanes[(size_t) trackIndex];
        return trackLane.empty() ? staticGainDb : trackLane.valueAt(beat, staticGainDb);
    };

    // Isolate each track (the other silenced at -100 dB) so the comparison
    // below reflects one track's automation state, not the fixed two-track mix.
    const auto arpAloneAutomated  = OfflineRenderer::render({ arp, bass }, { 0.0f, -100.0f }, std::vector<bool>{},
                                                            std::vector<double>{}, std::vector<float>{},
                                                            false, 0.5f, 0.5f, 0.0f,
                                                            bpm, sampleRate, seconds, 512, perTrackGainFn);
    const auto bassAloneNoAuto    = OfflineRenderer::render({ arp, bass }, { -100.0f, 0.0f }, std::vector<bool>{},
                                                            std::vector<double>{}, std::vector<float>{},
                                                            false, 0.5f, 0.5f, 0.0f,
                                                            bpm, sampleRate, seconds, 512, perTrackGainFn);

    const int   halfArp             = arpAloneAutomated.getNumSamples() / 2;
    const float rmsArpFirstHalf     = arpAloneAutomated.getRMSLevel(0, 0, halfArp);
    const float rmsArpSecondHalf    = arpAloneAutomated.getRMSLevel(0, halfArp, arpAloneAutomated.getNumSamples() - halfArp);
    const bool  perTrackAutoFades   = rmsArpFirstHalf < rmsArpSecondHalf;

    const int   halfBass            = bassAloneNoAuto.getNumSamples() / 2;
    const float rmsBassFirstHalf    = bassAloneNoAuto.getRMSLevel(0, 0, halfBass);
    const float rmsBassSecondHalf   = bassAloneNoAuto.getRMSLevel(0, halfBass, bassAloneNoAuto.getNumSamples() - halfBass);
    // A loose tolerance: two loop iterations of the same pattern aren't
    // perfectly identical (envelope/voice state carries small differences
    // across the loop boundary), but a real automation leak would show up as
    // a multiple, not ~10-15% — the arp check above swings 10x for contrast.
    const bool  nonAutomatedStable  = rmsBassSecondHalf > 0.01f
                                    && std::abs(rmsBassSecondHalf - rmsBassFirstHalf)
                                           < 0.25f * std::max(rmsBassFirstHalf, rmsBassSecondHalf);

    const bool perTrackAutomationWorks = perTrackAutoFades && nonAutomatedStable;

    // The written file is the wet (delayed) mix.
    if (! OfflineRenderer::writeWav(out, wet, sampleRate))
    {
        std::cerr << "Failed to write " << out.getFullPathName() << "\n";
        return 1;
    }

    std::cout << "wrote " << out.getFullPathName()
              << "  frames=" << wet.getNumSamples()
              << "  rmsDry=" << rmsDry
              << "  rmsWet=" << rmsWet
              << "  rmsFiltered=" << rmsFiltered
              << "  gainRatio(-6dB)=" << gainRatio
              << "  delayChanged=" << (delayChanged ? 1 : 0)
              << "  filterAtten=" << (filterAttenuates ? 1 : 0)
              << "  reverbChanged=" << (reverbChanged ? 1 : 0)
              << "  automationFades=" << (automationFades ? 1 : 0)
              << "  perTrackAutomationWorks=" << (perTrackAutomationWorks ? 1 : 0)
              << "  soloMatchesArpOnly=" << (soloMatchesArpOnly ? 1 : 0)
              << "  clipStartGates=" << (clipStartGates ? 1 : 0)
              << "  sendBusChanged=" << (sendBusChanged ? 1 : 0)
              << "  sendBusDelayWorks=" << (sendBusDelayWorks ? 1 : 0)
              << "  multiClipGates=" << (multiClipGates ? 1 : 0)
              << "  audioTrackWorks=" << (audioTrackWorks ? 1 : 0)
              << "  multiClipAudioGates=" << (multiClipAudioGates ? 1 : 0)
              << "  midiRoundTripWorks=" << (midiRoundTripWorks ? 1 : 0)
              << "  drumKitWorks=" << (drumKitWorks ? 1 : 0)
              << "  drumPadMixWorks=" << (drumPadMixWorks ? 1 : 0)
              << "  drumPadPitchWorks=" << (drumPadPitchWorks ? 1 : 0)
              << "  metronomeWorks=" << (metronomeWorks ? 1 : 0)
              << "  metronomeSilentWhenOff=" << (metronomeSilentWhenOff ? 1 : 0)
              << "  recorderWorks=" << (recorderWorks ? 1 : 0) << "\n";

    // Non-silent output, a correct -6 dB gain ratio, a delay that alters the
    // signal, a low-pass that attenuates, a reverb that changes the signal, a
    // gain ramp that fades in, a sample-accurate per-track automation curve
    // that fades one track while leaving an unautomated sibling stable, solo
    // correctly silencing the other track, a clip start that gates playback, a
    // send bus that changes the output whether it's reverb or delay, two
    // MIDI clips on one track each sounding only in their own window, a
    // decoded audio clip playing back through a track, two AUDIO clips on one
    // track likewise each sounding only in their own window, a MIDI file
    // export/import round trip that preserves tempo and every note, a drum
    // kit playing the right pad's one-shot sample at the right times while
    // an unassigned pad stays silent, per-pad gain/pan/mute and transposition
    // each doing what they say against that same kit, and the recorder's capture/handoff
    // logic (fed synthetic input, since there's no live mic here) together
    // confirm
    // the full render/gain/fx/automation/solo/clip/send-bus/audio/midi/drum/record path.
    const bool ok = rmsDry > 0.0f && std::isfinite(rmsDry)
                 && gainRatio > 0.47f && gainRatio < 0.53f
                 && delayChanged && filterAttenuates && reverbChanged && automationFades
                 && perTrackAutomationWorks
                 && soloMatchesArpOnly && clipStartGates && sendBusChanged && sendBusDelayWorks && multiClipGates
                 && audioTrackWorks && multiClipAudioGates && midiRoundTripWorks && drumKitWorks
                 && drumPadMixWorks && drumPadPitchWorks
                 && metronomeWorks && metronomeSilentWhenOff && recorderWorks;
    return ok ? 0 : 2;
}
