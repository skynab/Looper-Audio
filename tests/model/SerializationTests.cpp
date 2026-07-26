#include <catch2/catch_test_macros.hpp>

#include <model/Serialization.h>
#include <model/Song.h>

using namespace looper::model;

static Song makeSampleSong()
{
    Song s;
    s.bpm                = 128.0;
    s.timeSigNumerator   = 3;
    s.timeSigDenominator = 4;
    s.delay.enabled      = true;
    s.delay.timeMs       = 250.0f;
    s.delay.feedback     = 0.4f;
    s.delay.mix          = 0.5f;
    s.filter.enabled     = true;
    s.filter.mode        = 1;
    s.filter.cutoff      = 800.0f;
    s.filter.resonance   = 1.2f;
    s.reverb.enabled     = true;
    s.reverb.roomSize    = 0.7f;
    s.reverb.damping     = 0.4f;
    s.reverb.mix         = 0.25f;
    s.sendBus.enabled       = true;
    s.sendBus.effectType    = SendBusEffectType::Delay;
    s.sendBus.roomSize      = 0.6f;
    s.sendBus.damping       = 0.3f;
    s.sendBus.delayTimeMs   = 250.0f;
    s.sendBus.delayFeedback = 0.4f;
    s.sendBus.returnLevel   = 0.45f;
    s.projectRootFolder     = "/Users/test/My Looper Projects"; // with a space, deliberately
    s.masterGainDb.addPoint(0.0, -40.0f);
    s.masterGainDb.addPoint(4.0, 0.0f);
    s.masterGainDb.addPoint(8.0, -6.0f);

    const int synthId = addTrack(s, TrackType::Instrument, "Synth Lead").id; // name with a space

    Clip midiClip;
    midiClip.type             = ClipType::Instrument;
    midiClip.lengthBeats      = 4.0;
    midiClip.pattern.lengthBeats = 4.0;
    midiClip.pattern.notes.push_back({ 0.0, 0.5, 60, 0.8f });
    midiClip.pattern.notes.push_back({ 1.0, 0.25, 64, 0.9f });
    midiClip.pattern.notes.push_back({ 2.5, 1.0, 67, 0.6f });
    addClip(s, synthId, midiClip);

    const int voxId = addTrack(s, TrackType::Audio, "Vox").id;
    Clip audioClip;
    audioClip.type      = ClipType::Audio;
    audioClip.audioFile = "takes/vocal 01.wav";
    addClip(s, voxId, audioClip);

    const int drumId = addTrack(s, TrackType::Drum, "Drums").id; // auto-populates the default pads
    Clip drumClip;
    drumClip.type             = ClipType::Instrument;
    drumClip.lengthBeats      = 4.0;
    drumClip.pattern.lengthBeats = 4.0;
    drumClip.pattern.notes.push_back({ 0.0, 0.25, 36, 1.0f }); // kick on beat 1
    drumClip.pattern.notes.push_back({ 1.0, 0.25, 38, 0.9f }); // snare on beat 2
    addClip(s, drumId, drumClip);

    // Set solo/mute by index (not the returned reference — a later addTrack can
    // reallocate the vector and invalidate it).
    s.tracks[0].solo      = true;
    s.tracks[0].sendLevel = 0.65f;
    s.tracks[1].muted     = true;
    s.tracks[0].gainAutomation.addPoint(0.0, -20.0f);
    s.tracks[0].gainAutomation.addPoint(4.0, 0.0f);
    s.tracks[2].drumKit.pads[0].samplePath = "samples/Kick 808.wav"; // with a space, deliberately
    s.tracks[2].drumKit.pads[0].gainDb         = -2.5f;
    s.tracks[2].drumKit.pads[0].pitchSemitones = -3.0f;
    s.tracks[2].drumKit.pads[0].solo           = true;
    s.tracks[2].drumKit.pads[1].pan            = 0.4f;
    s.tracks[2].drumKit.pads[1].muted          = true;

    s.tracks[0].synthSettings.waveform        = 2; // square
    s.tracks[0].synthSettings.attackMs        = 12.0f;
    s.tracks[0].synthSettings.decayMs         = 300.0f;
    s.tracks[0].synthSettings.sustain         = 0.5f;
    s.tracks[0].synthSettings.releaseMs       = 400.0f;
    s.tracks[0].synthSettings.filterEnabled   = true;
    s.tracks[0].synthSettings.filterMode      = 1;
    s.tracks[0].synthSettings.filterCutoff    = 2500.0f;
    s.tracks[0].synthSettings.filterResonance = 1.5f;
    s.tracks[0].synthSettings.gainDb          = -3.0f;

    s.tracks[0].insertFilter.enabled   = true;
    s.tracks[0].insertFilter.mode      = 2;
    s.tracks[0].insertFilter.cutoff    = 3200.0f;
    s.tracks[0].insertFilter.resonance = 0.9f;
    s.tracks[1].insertDelay.enabled    = true;
    s.tracks[1].insertDelay.timeMs     = 180.0f;
    s.tracks[1].insertDelay.feedback   = 0.55f;
    s.tracks[1].insertDelay.mix        = 0.4f;
    s.tracks[2].insertReverb.enabled   = true;
    s.tracks[2].insertReverb.roomSize  = 0.8f;
    s.tracks[2].insertReverb.damping   = 0.2f;
    s.tracks[2].insertReverb.mix       = 0.35f;

    return s;
}

TEST_CASE("Song survives a serialize/deserialize round trip", "[model][io]")
{
    const Song original = makeSampleSong();

    const std::string text = serialize(original);
    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == original);
}

TEST_CASE("An empty song round-trips", "[model][io]")
{
    const Song original;
    Song restored;
    REQUIRE(deserialize(serialize(original), restored));
    REQUIRE(restored == original);
}

TEST_CASE("deserialize rejects malformed input", "[model][io]")
{
    Song out;
    REQUIRE_FALSE(deserialize("not a looper file", out));
    REQUIRE_FALSE(deserialize("", out));
    REQUIRE_FALSE(deserialize("LOOPER 1\nBPM 120\n", out)); // truncated (missing later records)
}

TEST_CASE("deserialize reports why it failed", "[model][io]")
{
    Song        out;
    std::string error;

    REQUIRE_FALSE(deserialize("not a looper file", out, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("A file from a newer build is refused, not part-parsed", "[model][io]")
{
    // Reading it with this build's rules would silently drop whatever records
    // it gained — worse than declining to open it.
    const std::string newer = "LOOPER " + std::to_string(kFormatVersion + 1) + "\nBPM 120\n";

    Song        out;
    std::string error;
    REQUIRE_FALSE(deserialize(newer, out, &error));
    REQUIRE(error.find("newer") != std::string::npos);
}

TEST_CASE("A project from before per-track synths still opens", "[model][io]")
{
    // A v11 file: no SYNTH record, and DPAD in its old note/label/path shape.
    // This is exactly what was on disk before those two format bumps, and it
    // must still load — with the new fields at their defaults.
    const std::string v11 =
        "LOOPER 11\n"
        "BPM 100\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 5\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "TRACKS 1\n"
        "TRACK 1 2 0 0 0 0 Drums\n"
        "TAUTO 0\n"
        "DRUMKIT 1\n"
        "DPAD 36 Kick samples/Kick 808.wav\n"
        "CLIPS 1\n"
        "CLIP 2 0 0 4 4 \n"
        "NOTES 1\n"
        "NOTE 0 0.25 36 1\n";

    Song        restored;
    std::string error;
    REQUIRE(deserialize(v11, restored, &error));

    REQUIRE(restored.bpm == 100.0);
    REQUIRE(restored.tracks.size() == 1);

    const auto& track = restored.tracks[0];
    REQUIRE(track.type == TrackType::Drum);
    REQUIRE(track.clips.size() == 1);
    REQUIRE(track.clips[0].pattern.notes.size() == 1);

    // The pad's path survives (spaces and all) and the v13 mix fields default
    // to a no-op, so the kit sounds as it did before they existed.
    REQUIRE(track.drumKit.pads.size() == 1);
    REQUIRE(track.drumKit.pads[0].samplePath == "samples/Kick 808.wav");
    REQUIRE(track.drumKit.pads[0].gainDb == 0.0f);
    REQUIRE(track.drumKit.pads[0].pan == 0.0f);
    REQUIRE_FALSE(track.drumKit.pads[0].muted);

    // And the synth settings this file predates are the defaults.
    REQUIRE(track.synthSettings == SynthSettings{});

    // Likewise the insert effects, added later still: all off, so a project
    // from before they existed sounds exactly as it did.
    REQUIRE_FALSE(track.insertFilter.enabled);
    REQUIRE_FALSE(track.insertDelay.enabled);
    REQUIRE_FALSE(track.insertReverb.enabled);
}

TEST_CASE("A current-format file still round-trips after the version work", "[model][io]")
{
    const Song original = makeSampleSong();

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored == original);
}
