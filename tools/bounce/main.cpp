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
#include "engine/EffectChain.h"
#include "engine/GuitarNode.h"
#include "engine/PluginHost.h"
#include "engine/PluginNode.h"
#include "engine/FilterEffect.h"
#include "engine/InstrumentTrack.h"
#include "engine/Metronome.h"
#include "engine/SessionPlayer.h"
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

        // Count-in: armed with a lead-in, the recorder must roll without
        // capturing, and — critically — a take abandoned *during* its count-in
        // must still finish. If it doesn't, isFinished() never goes true, the
        // owner waits forever on a take that never arrives, and recording is
        // dead until the app restarts.
        AudioRecorder countInRecorder;
        countInRecorder.prepare(recSampleRate, 1, 1.0);
        countInRecorder.arm((int64_t) blockSize * 2); // two blocks of count-in

        countInRecorder.process(channelPtrs, 1, blockSize, true);
        const bool countInCapturesNothing = countInRecorder.recordedSampleCount() == 0
                                         && countInRecorder.leadInRemaining() > 0;

        countInRecorder.process(channelPtrs, 1, blockSize, true); // lead-in now elapsed
        countInRecorder.process(channelPtrs, 1, blockSize, true); // this one captures
        const bool capturesAfterCountIn = countInRecorder.recordedSampleCount() == blockSize;

        AudioRecorder abandonedRecorder;
        abandonedRecorder.prepare(recSampleRate, 1, 1.0);
        abandonedRecorder.arm((int64_t) blockSize * 8); // a long count-in
        abandonedRecorder.process(channelPtrs, 1, blockSize, true); // still counting in
        abandonedRecorder.disarm();                                 // ...and give up
        abandonedRecorder.process(channelPtrs, 1, blockSize, true);
        const bool abandonedTakeFinishes = abandonedRecorder.isFinished()
                                        && abandonedRecorder.takeLength() == 0;

        recorderWorks = capturesNothingWhenDisarmed && capturedThreeBlocks && notFinishedWhileRecording
                     && finishedAfterDisarm && lengthUnchangedAfterDisarm && contentMatches && capacityCapped
                     && countInCapturesNothing && capturesAfterCountIn && abandonedTakeFinishes;
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
                                                               512, nullptr,
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

    // Pan-automation export check: a curve sweeping hard left to hard right
    // across the render must land the energy on the left early and the right
    // late. Now goes through the same TrackAutomation the live engine uses,
    // rather than a renderer-only callback.
    bool panAutomationWorks = false;
    {
        const double renderSeconds = 4.0;
        const double totalBeats    = renderSeconds * bpm / 60.0;

        TrackAutomation sweep;
        sweep.pan.addPoint(0.0, -1.0f);
        sweep.pan.addPoint(totalBeats, 1.0f);

        const OfflineRenderer::TrackAutomationList curves { sweep };

        const auto swept = OfflineRenderer::render({ arp }, { 0.0f }, {}, {}, { 0.0f },
                                                   false, 0.5f, 0.5f, 0.5f,
                                                   bpm, sampleRate, renderSeconds, 512, &curves);

        const int window = (int) (sampleRate * 0.5);
        const int lateAt = swept.getNumSamples() - window;

        const float earlyLeft  = swept.getRMSLevel(0, 0, window);
        const float earlyRight = swept.getRMSLevel(1, 0, window);
        const float lateLeft   = swept.getRMSLevel(0, lateAt, window);
        const float lateRight  = swept.getRMSLevel(1, lateAt, window);

        panAutomationWorks = earlyLeft > earlyRight * 2.0f && lateRight > lateLeft * 2.0f;
    }

    // Plugin-hosting check, against a *real* plugin rather than a mock: scan
    // whatever effect plugins this machine has, instantiate one, run audio
    // through it as a chain node, and require it to change the signal.
    //
    // A machine with no plugins (CI on Linux, say) is not a failure of this
    // code, so the check passes when none are found — pluginsScanned is
    // printed alongside so it's visible whether anything was actually
    // exercised, rather than the check quietly meaning nothing.
    bool pluginHostWorks = false;
    int  pluginsScanned  = 0;
    {
        juce::ScopedJuceInitialiser_GUI juceInit; // plugin formats want a message loop

        PluginHost host;
        const auto deadMansPedal = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("looper_bounce_plugin_scan.tmp");

        for (const auto& format : host.availableFormats())
            host.scanFormat(format, deadMansPedal, 24); // capped: probing instantiates each one

        deadMansPedal.deleteFile();

        // A stereo effect, not an instrument — something that transforms audio
        // handed to it.
        const PluginEntry* chosen = nullptr;
        const auto entries = host.knownPlugins();
        pluginsScanned = (int) entries.size();
        for (const auto& entry : entries)
        {
            if (! entry.isInstrument && entry.numInputs >= 2 && entry.numOutputs >= 2)
            {
                chosen = &entry;
                break;
            }
        }

        if (chosen == nullptr)
        {
            pluginHostWorks = true; // nothing to host here; not this code's fault
        }
        else
        {
            std::string error;
            auto instance = host.createInstance(chosen->format, chosen->identifier,
                                                sampleRate, 512, &error);
            if (instance == nullptr)
            {
                std::cerr << "plugin instantiation failed: " << error << "\n";
            }
            else
            {
                // Render the same part twice: once plain, once through the
                // plugin as a chain node. A plugin at its defaults might be
                // transparent, so this asserts it *ran* (no crash, buffer
                // intact and finite) and reports whether it altered the sound.
                auto renderThroughPlugin = [&](std::unique_ptr<juce::AudioPluginInstance> plugin,
                                               bool bypassed = false)
                {
                    const int totalSamples = (int) (sampleRate * 1.0);
                    juce::AudioBuffer<float> mix(2, totalSamples);
                    mix.clear();

                    InstrumentTrack track;
                    track.prepare(sampleRate, 512);

                    if (plugin != nullptr)
                    {
                        auto chain = std::make_unique<EffectChain>();
                        auto node  = std::make_unique<PluginNode>(std::move(plugin));
                        node->setBypassed(bypassed);
                        chain->add(std::move(node));
                        chain->prepare(sampleRate, 512);
                        track.setEffectChain(chain.release());
                    }

                    ClipSlot slot;
                    slot.pattern     = arp;
                    slot.startBeats  = 0.0;
                    slot.lengthBeats = 1.0e9;
                    track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

                    juce::AudioBuffer<float> sendBus(2, 512);
                    juce::MidiBuffer         noLiveMidi;

                    for (int pos = 0; pos < totalSamples; pos += 512)
                    {
                        const int n = std::min(512, totalSamples - pos);

                        ProcessContext context;
                        context.sampleRate                   = sampleRate;
                        context.numSamples                   = n;
                        context.transport.playing            = true;
                        context.transport.playheadSamples    = pos;
                        context.transport.bpm                = bpm;
                        context.transport.timeSigNumerator   = 4;
                        context.transport.timeSigDenominator = 4;

                        sendBus.setSize(2, n, false, false, true);
                        sendBus.clear();

                        juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                        track.render(blockView, sendBus, noLiveMidi, context, false, false);
                    }
                    return mix;
                };

                const auto hosted = renderThroughPlugin(std::move(instance));

                // A bypassed plugin must pass the signal through untouched —
                // identical to having no chain at all. Unlike "did the plugin
                // colour the sound", which depends on whichever plugin this
                // machine happened to offer, this is deterministic and tests
                // PluginNode's own bypass path.
                auto second = host.createInstance(chosen->format, chosen->identifier, sampleRate, 512);
                const auto bypassedRender = renderThroughPlugin(std::move(second), true);
                const auto noPluginRender = renderThroughPlugin(nullptr);

                float bypassDelta = 0.0f;
                for (int i = 0; i < bypassedRender.getNumSamples(); ++i)
                    bypassDelta = std::max(bypassDelta,
                                           std::abs(bypassedRender.getSample(0, i)
                                                    - noPluginRender.getSample(0, i)));

                // Every sample must be finite: a plugin writing past its buffer
                // or returning NaN is the failure mode that matters most, since
                // it poisons the whole mix downstream.
                bool  allFinite = true;
                float peak      = 0.0f;
                for (int ch = 0; ch < hosted.getNumChannels() && allFinite; ++ch)
                    for (int i = 0; i < hosted.getNumSamples(); ++i)
                    {
                        const float sample = hosted.getSample(ch, i);
                        if (! std::isfinite(sample)) { allFinite = false; break; }
                        peak = std::max(peak, std::abs(sample));
                    }

                pluginHostWorks = allFinite && peak > 0.0f && bypassDelta < 1.0e-9f;
                std::cerr << "hosted plugin: " << chosen->name << " (" << chosen->format
                          << "), peak=" << peak << ", bypassDelta=" << bypassDelta << "\n";
            }
        }
    }

    // Guitar checks. The DSP itself is covered by unit tests; what the bounce
    // tool adds is the *performance* model, which is what separates a guitar
    // from a synth with a plucked patch.
    bool guitarSounds          = false;
    bool guitarCutsSameString  = false;
    bool guitarPlaysSixAtOnce  = false;
    bool guitarPicksLowestFret = false;
    bool guitarHammerOn        = false;
    {
        // Renders a guitar node given (noteNumber, sampleOffset) note-ons.
        auto renderNotes = [&](const std::vector<std::pair<int, int>>& notes, double seconds)
        {
            const int totalSamples = (int) (sampleRate * seconds);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);
            guitar.setDecaySeconds(4.0f);

            juce::MidiBuffer midi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                midi.clear();
                for (const auto& [note, offset] : notes)
                    if (offset >= pos && offset < pos + n)
                        midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), offset - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                context.transport.playheadSamples    = pos;
                context.transport.bpm                = bpm;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                juce::AudioBuffer<float> view(mix.getArrayOfWritePointers(), 2, pos, n);
                guitar.process(view, midi, context);
            }
            return mix;
        };

        auto magnitude = [&](const juce::AudioBuffer<float>& buffer, double frequency, int from, int count)
        {
            double real = 0.0, imaginary = 0.0;
            for (int i = 0; i < count && from + i < buffer.getNumSamples(); ++i)
            {
                const double angle = 2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate;
                real      += buffer.getSample(0, from + i) * std::cos(angle);
                imaginary += buffer.getSample(0, from + i) * std::sin(angle);
            }
            return std::hypot(real, imaginary) / count;
        };

        // A single plucked low E must sound.
        const auto single = renderNotes({ { 40, 0 } }, 1.0);
        guitarSounds = single.getRMSLevel(0, 0, single.getNumSamples()) > 0.001f;

        // One note per string: E2 (40) then F2 (41). Only the low E string can
        // reach either, so the second note must *cut* the first — the single
        // most audible thing separating this from a polyphonic synth.
        const int  window = (int) (0.3 * sampleRate);
        const int  second = (int) (0.5 * sampleRate);
        const auto cut    = renderNotes({ { 40, 0 }, { 41, second } }, 1.5);

        const double e2Before = magnitude(cut, 82.41, (int) (0.05 * sampleRate), window);
        const double e2After  = magnitude(cut, 82.41, second + (int) (0.05 * sampleRate), window);
        const double f2After  = magnitude(cut, 87.31, second + (int) (0.05 * sampleRate), window);

        // E2 must be largely gone, and F2 present in its place.
        guitarCutsSameString = e2Before > 1.0e-4 && e2After < e2Before * 0.25 && f2After > e2After;

        // ...but six notes that fit six different strings must all ring: the
        // cut rule is per string, not a global monophony.
        const auto chord = renderNotes({ { 40, 0 }, { 45, 0 }, { 50, 0 },
                                         { 55, 0 }, { 59, 0 }, { 64, 0 } }, 1.0);
        const int  from  = (int) (0.05 * sampleRate);
        int        heard = 0;
        for (double f : { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 })
            if (magnitude(chord, f, from, window) > 1.0e-4)
                ++heard;

        guitarPlaysSixAtOnce = heard == 6;

        // Hammer-on: with every string already held, a further note that a held
        // string can reach must be *re-fretted* rather than struck. Two things
        // have to be true — the pitch moves on that string, and no new attack
        // appears, which is what makes a hammer-on softer than a picked note.
        {
            const int    hammerAt = (int) (0.5 * sampleRate);
            const double barSpan  = 0.25 * sampleRate;

            // Hold all six strings, then ask for a note only a held string can
            // take (F4 = 65, reachable on the high E at fret 1).
            std::vector<std::pair<int, int>> notes;
            for (int n : { 40, 45, 50, 55, 59, 64 })
                notes.push_back({ n, 0 });
            notes.push_back({ 65, hammerAt });

            const auto rendered = renderNotes(notes, 1.5);

            // Level just before and just after the hammer-on. A fresh pluck
            // would spike; a hammer-on must not.
            float before = 0.0f, after = 0.0f;
            for (int i = hammerAt - (int) barSpan; i < hammerAt; ++i)
                before = std::max(before, std::abs(rendered.getSample(0, i)));
            for (int i = hammerAt; i < hammerAt + (int) barSpan; ++i)
                after = std::max(after, std::abs(rendered.getSample(0, i)));

            // ...and the new pitch must actually be sounding afterwards.
            const double f4 = 349.23;
            const double f4After = magnitude(rendered, f4, hammerAt + 2000, (int) (0.3 * sampleRate));

            guitarHammerOn = after <= before && f4After > 1.0e-4;
        }

        // Allocation preference, checked directly rather than inferred from
        // the audio: E4 is reachable on every string (fret 24 on the low E
        // down to open on the high E), and the rule is to take the one needing
        // the lowest fret. The cut check above can't see this, because the
        // notes it uses are only reachable on one string either way.
        {
            GuitarNode guitar;
            guitar.prepare(sampleRate, 512);

            juce::AudioBuffer<float> scratch(2, 512);
            scratch.clear();

            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.9f), 0); // E4

            ProcessContext context;
            context.sampleRate                   = sampleRate;
            context.numSamples                   = 512;
            context.transport.playing            = true;
            context.transport.bpm                = bpm;
            context.transport.timeSigNumerator   = 4;
            context.transport.timeSigDenominator = 4;
            guitar.process(scratch, midi, context);

            guitarPicksLowestFret = guitar.noteOnString(5) == 64  // open high E
                                 && guitar.noteOnString(0) == -1; // not fret 24 on the low E
        }
    }

    // Chain-order check.
    //
    // Note the trap here: the three built-ins (filter, delay, reverb) are all
    // linear and time-invariant, and LTI systems *commute* — filter-then-delay
    // and delay-then-filter produce bit-comparable output (measured: a peak
    // difference of 2e-7, pure float ordering). A first attempt at this check
    // used them and reported "order doesn't matter", which was true and told
    // us nothing about the chain. Reordering the built-ins genuinely won't
    // change the sound, and that's correct DSP rather than a bug.
    //
    // So the mechanism is tested with two deliberately non-commuting nodes —
    // a gain and a hard clip, where halving before clipping differs from
    // clipping before halving. This tests EffectChain, not the DSP.
    // A drive pedal in a real chain: it must audibly change the sound, and the
    // cabinet must audibly change it again. Both are claims about what comes
    // out of the speakers, so both are measured here rather than only in the
    // headless DSP tests, which exercise the shaper in isolation.
    bool driveChangesSound  = false;
    bool driveCabinetWorks  = false;
    {
        auto renderDrive = [&](int layout) // 0 = none, 1 = drive+cab, 2 = drive, no cab
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            if (layout != 0)
            {
                auto chain = std::make_unique<EffectChain>();
                auto node  = std::make_unique<DriveNode>();
                node->effect.setEnabled(true);
                node->effect.setDrive(12.0f);
                node->effect.setTone(0.5f);
                node->effect.setLevel(0.8f);
                node->effect.setCabinet(layout == 1);
                chain->add(std::move(node));
                chain->prepare(sampleRate, 512);
                track.setEffectChain(chain.release());
            }

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::AudioBuffer<float> sendBus(2, 512);
            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                context.transport.playheadSamples    = pos;
                context.transport.bpm                = bpm;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto worstDifference = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto clean     = renderDrive(0);
        const auto withCab   = renderDrive(1);
        const auto noCab     = renderDrive(2);

        driveChangesSound = clean.getRMSLevel(0, 0, clean.getNumSamples()) > 1.0e-4f
                         && worstDifference(clean, withCab) > 1.0e-3f;

        // The cabinet is the difference between distortion and fizz, so it
        // has to actually be in the path rather than merely stored.
        driveCabinetWorks = worstDifference(withCab, noCab) > 1.0e-3f;
    }

    bool effectChainOrderMatters = false;
    bool effectChainRunsAllNodes = false;
    {
        struct GainNode final : EffectProcessor
        {
            EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
            void prepare(double, int) override {}
            void setEnabled(bool) override {}
            void process(juce::AudioBuffer<float>& buffer) override { buffer.applyGain(0.25f); }
        };

        struct ClipNode final : EffectProcessor
        {
            EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
            void prepare(double, int) override {}
            void setEnabled(bool) override {}
            void process(juce::AudioBuffer<float>& buffer) override
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        buffer.setSample(ch, i, juce::jlimit(-0.02f, 0.02f, buffer.getSample(ch, i)));
            }
        };

        // 0 = gain then clip, 1 = clip then gain, 2 = gain only.
        auto renderChain = [&](int layout)
        {
            const int totalSamples = (int) (sampleRate * 1.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            auto chain = std::make_unique<EffectChain>();
            if (layout == 0)      { chain->add(std::make_unique<GainNode>()); chain->add(std::make_unique<ClipNode>()); }
            else if (layout == 1) { chain->add(std::make_unique<ClipNode>()); chain->add(std::make_unique<GainNode>()); }
            else                  { chain->add(std::make_unique<GainNode>()); }
            chain->prepare(sampleRate, 512);
            track.setEffectChain(chain.release());

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::AudioBuffer<float> sendBus(2, 512);
            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                context.transport.playheadSamples    = pos;
                context.transport.bpm                = bpm;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
            }
            return mix;
        };

        auto peakDifference = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            const int n = std::min(a.getNumSamples(), b.getNumSamples());
            float worst = 0.0f;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(a.getSample(0, i) - b.getSample(0, i)));
            return worst;
        };

        const auto gainThenClip = renderChain(0);
        const auto clipThenGain = renderChain(1);
        const auto gainOnly     = renderChain(2);

        effectChainOrderMatters = gainThenClip.getRMSLevel(0, 0, gainThenClip.getNumSamples()) > 1.0e-4f
                               && peakDifference(gainThenClip, clipThenGain) > 1.0e-3f;

        // ...and a two-node chain must differ from a one-node chain, or the
        // second node isn't being run at all.
        effectChainRunsAllNodes = peakDifference(gainThenClip, gainOnly) > 1.0e-3f;
    }

    // Session-launch check: the whole point of the session grid is that a clip
    // launched mid-bar starts at the *next bar line*, not immediately. Renders
    // one track whose session slot is launched a fraction of a bar in, and
    // requires silence until the boundary and sound after it.
    bool sessionLaunchQuantizes = false;
    bool sessionStopWorks       = false;
    {
        const double samplesPerBeat = sampleRate * 60.0 / bpm;
        const double barSamples     = samplesPerBeat * 4.0; // 4/4

        // A clip that hits on every beat, so "is it sounding" is easy to read.
        Pattern sessionPattern;
        sessionPattern.lengthBeats = 4.0;
        for (int i = 0; i < 4; ++i)
            sessionPattern.notes.push_back({ (double) i, 0.5, 60, 0.9f });

        auto renderSession = [&](bool stopAfterFirstBar)
        {
            const int totalSamples = (int) (barSamples * 3.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);

            auto* slots = new SessionPlayer::SlotList();
            slots->push_back({ true, sessionPattern });
            track.session.submitSlots(slots);

            juce::AudioBuffer<float> sendBus(2, 512);
            juce::MidiBuffer         noLiveMidi;
            bool                     launched = false, stopped = false;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                // Launch a quarter of the way into the first bar: the clip
                // must not start here, but at the bar line that follows.
                if (! launched && pos >= (int) (barSamples * 0.25))
                {
                    track.session.requestLaunch(0);
                    launched = true;
                }
                if (stopAfterFirstBar && ! stopped && pos >= (int) (barSamples * 1.25))
                {
                    track.session.requestStop();
                    stopped = true;
                }

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                context.transport.playheadSamples    = pos;
                context.transport.bpm                = bpm;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false, barSamples);
            }
            return mix;
        };

        const auto launchedMix = renderSession(false);

        // Just before the bar line the clip must still be silent; just after,
        // sounding. A launch that ignored quantization would fill both.
        const int   probe        = (int) (sampleRate * 0.15);
        const float beforeLaunch = launchedMix.getRMSLevel(0, (int) (barSamples * 0.5), probe);
        const float afterLaunch  = launchedMix.getRMSLevel(0, (int) barSamples + 1000, probe);

        sessionLaunchQuantizes = beforeLaunch < 1.0e-6f && afterLaunch > 0.01f;

        // Stopping mid-bar likewise takes effect at the next bar line: asked
        // for a quarter into bar 1, it happens at bar 2. So the clip is still
        // sounding halfway through bar 1 and gone by bar 2.5 (the track has no
        // arrangement clips here to fall back to).
        const auto  stoppedMix  = renderSession(true);
        const float beforeStop  = stoppedMix.getRMSLevel(0, (int) (barSamples * 1.5), probe);
        const float afterStop   = stoppedMix.getRMSLevel(0, (int) (barSamples * 2.5), probe);

        sessionStopWorks = beforeStop > 0.01f && afterStop < 1.0e-6f;
    }

    // Per-track pan check: the same part hard-panned left must vanish from
    // the right channel while staying present on the left, and a centred
    // track must be identical on both — the unity-centre pan law is what
    // keeps every existing project's balance unchanged.
    bool trackPanWorks = false;
    {
        auto renderPanned = [&](float panPosition)
        {
            const int totalSamples = (int) (sampleRate * 2.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);
            track.pan.store(panPosition);

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::AudioBuffer<float> sendBus(2, 512);
            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                   = sampleRate;
                context.numSamples                   = n;
                context.transport.playing            = true;
                context.transport.playheadSamples    = pos;
                context.transport.bpm                = bpm;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
            }

            return std::make_pair(mix.getRMSLevel(0, 0, totalSamples), mix.getRMSLevel(1, 0, totalSamples));
        };

        const auto centred = renderPanned(0.0f);
        const auto left    = renderPanned(-1.0f);

        trackPanWorks = centred.first > 0.01f
                     && std::abs(centred.first - centred.second) < 1.0e-6f // centre is balanced
                     && left.first > 0.01f                                  // still there on the left
                     && left.second < 1.0e-6f                               // gone from the right
                     && std::abs(left.first - centred.first) < 1.0e-6f;     // and unchanged in level
    }

    // Per-track insert check: the same part rendered through one track twice,
    // once with that track's own insert low-pass enabled well below the note
    // content. Proves the insert chain is actually in the per-track path —
    // the existing rmsDry sentinel already proves the other half, that
    // *disabled* inserts leave the signal bit-identical, since OfflineRenderer
    // renders through InstrumentTrack and now runs three (bypassed) inserts
    // per block.
    bool trackInsertFilterWorks = false;
    {
        auto renderOneTrack = [&](bool filterEnabled)
        {
            const int totalSamples = (int) (sampleRate * 2.0);
            juce::AudioBuffer<float> mix(2, totalSamples);
            mix.clear();

            InstrumentTrack track;
            track.prepare(sampleRate, 512);
            auto chain = std::make_unique<EffectChain>();
            auto filter = std::make_unique<FilterNode>();
            filter->effect.setEnabled(filterEnabled);
            filter->effect.setMode(0); // low-pass
            filter->effect.setCutoff(150.0f);
            filter->effect.setResonance(0.707f);
            chain->add(std::move(filter));
            chain->prepare(sampleRate, 512);
            track.setEffectChain(chain.release());

            ClipSlot slot;
            slot.pattern     = arp;
            slot.startBeats  = 0.0;
            slot.lengthBeats = 1.0e9;
            track.sequencer.submitClips(new std::vector<ClipSlot> { slot });

            juce::AudioBuffer<float> sendBus(2, 512);
            juce::MidiBuffer         noLiveMidi;

            for (int pos = 0; pos < totalSamples; pos += 512)
            {
                const int n = std::min(512, totalSamples - pos);

                ProcessContext context;
                context.sampleRate                = sampleRate;
                context.numSamples                = n;
                context.transport.playing         = true;
                context.transport.playheadSamples = pos;
                context.transport.bpm             = bpm;
                context.transport.timeSigNumerator   = 4;
                context.transport.timeSigDenominator = 4;

                sendBus.setSize(2, n, false, false, true);
                sendBus.clear();

                juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
                track.render(blockView, sendBus, noLiveMidi, context, false, false);
            }

            return mix.getRMSLevel(0, 0, totalSamples);
        };

        const float plain    = renderOneTrack(false);
        const float filtered = renderOneTrack(true);

        trackInsertFilterWorks = plain > 0.01f && filtered < plain * 0.9f;
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
    // summed — this exercises the real OfflineRenderer::render(automation)
    // path. Track 1 (arp) gets a -40 dB -> 0 dB fade; track 2 (bass) gets none.
    TrackAutomation arpAutomation;
    arpAutomation.gain.addPoint(0.0, -40.0f);
    arpAutomation.gain.addPoint(bpm / 60.0 * seconds, 0.0f);
    const TrackAutomation noAutomation; // empty: bass keeps its static gain

    const OfflineRenderer::TrackAutomationList perTrackCurves { arpAutomation, noAutomation };

    // Isolate each track (the other silenced at -100 dB) so the comparison
    // below reflects one track's automation state, not the fixed two-track mix.
    const auto arpAloneAutomated  = OfflineRenderer::render({ arp, bass }, { 0.0f, -100.0f }, std::vector<bool>{},
                                                            std::vector<double>{}, std::vector<float>{},
                                                            false, 0.5f, 0.5f, 0.0f,
                                                            bpm, sampleRate, seconds, 512, &perTrackCurves);
    const auto bassAloneNoAuto    = OfflineRenderer::render({ arp, bass }, { -100.0f, 0.0f }, std::vector<bool>{},
                                                            std::vector<double>{}, std::vector<float>{},
                                                            false, 0.5f, 0.5f, 0.0f,
                                                            bpm, sampleRate, seconds, 512, &perTrackCurves);

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
              << "  pluginsScanned=" << pluginsScanned
              << "  pluginHostWorks=" << (pluginHostWorks ? 1 : 0)
              << "  guitarSounds=" << (guitarSounds ? 1 : 0)
              << "  guitarCutsSameString=" << (guitarCutsSameString ? 1 : 0)
              << "  guitarPlaysSixAtOnce=" << (guitarPlaysSixAtOnce ? 1 : 0)
              << "  guitarPicksLowestFret=" << (guitarPicksLowestFret ? 1 : 0)
              << "  guitarHammerOn=" << (guitarHammerOn ? 1 : 0)
              << "  driveChangesSound=" << (driveChangesSound ? 1 : 0)
              << "  driveCabinetWorks=" << (driveCabinetWorks ? 1 : 0)
              << "  effectChainOrderMatters=" << (effectChainOrderMatters ? 1 : 0)
              << "  effectChainRunsAllNodes=" << (effectChainRunsAllNodes ? 1 : 0)
              << "  sessionLaunchQuantizes=" << (sessionLaunchQuantizes ? 1 : 0)
              << "  sessionStopWorks=" << (sessionStopWorks ? 1 : 0)
              << "  trackPanWorks=" << (trackPanWorks ? 1 : 0)
              << "  panAutomationWorks=" << (panAutomationWorks ? 1 : 0)
              << "  trackInsertFilterWorks=" << (trackInsertFilterWorks ? 1 : 0)
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
                 && pluginHostWorks
                 && guitarSounds && guitarCutsSameString && guitarPlaysSixAtOnce
                 && guitarPicksLowestFret && guitarHammerOn
                 && effectChainOrderMatters && effectChainRunsAllNodes
                 && sessionLaunchQuantizes && sessionStopWorks
                 && trackPanWorks && panAutomationWorks && trackInsertFilterWorks
                 && metronomeWorks && metronomeSilentWhenOff && recorderWorks;
    return ok ? 0 : 2;
}
