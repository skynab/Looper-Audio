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
    midiClip.startBeats       = 8.0; // not at the origin: see below
    midiClip.lengthBeats      = 4.0;
    midiClip.pattern.lengthBeats = 4.0;
    midiClip.pattern.notes.push_back({ 0.0, 0.5, 60, 0.8f });
    midiClip.pattern.notes.push_back({ 1.0, 0.25, 64, 0.9f });
    midiClip.pattern.notes.push_back({ 2.5, 1.0, 67, 0.6f });
    addClip(s, synthId, midiClip);

    const int voxId = addTrack(s, TrackType::Audio, "Vox").id;
    Clip audioClip;
    audioClip.type       = ClipType::Audio;
    audioClip.startBeats = 2.5; // deliberately off the bar line
    audioClip.audioFile  = "takes/vocal 01.wav";
    addClip(s, voxId, audioClip);

    const int drumId = addTrack(s, TrackType::Drum, "Drums").id; // auto-populates the default pads
    Clip drumClip;
    drumClip.type             = ClipType::Instrument;
    drumClip.startBeats       = 16.0;
    drumClip.lengthBeats      = 4.0;
    drumClip.pattern.lengthBeats = 4.0;
    drumClip.pattern.notes.push_back({ 0.0, 0.25, 36, 1.0f }); // kick on beat 1
    drumClip.pattern.notes.push_back({ 1.0, 0.25, 38, 0.9f }); // snare on beat 2
    addClip(s, drumId, drumClip);

    // Set solo/mute by index (not the returned reference — a later addTrack can
    // reallocate the vector and invalidate it).
    s.tracks[0].gainDb    = -4.5f;
    s.tracks[0].solo      = true;
    s.tracks[0].sendLevel = 0.65f;
    s.tracks[0].pan       = -0.75f;
    s.tracks[1].gainDb    = 3.25f; // above unity, and positive
    s.tracks[1].pan       = 0.5f;
    s.tracks[1].muted     = true;
    s.tracks[0].laneFor(TrackParam::Gain).addPoint(0.0, -20.0f);
    s.tracks[0].laneFor(TrackParam::Gain).addPoint(4.0, 0.0f);
    s.tracks[0].laneFor(TrackParam::Pan).addPoint(0.0, -1.0f);
    s.tracks[0].laneFor(TrackParam::Pan).addPoint(8.0, 1.0f);
    s.tracks[1].laneFor(TrackParam::SendLevel).addPoint(2.0, 0.25f);
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

    // An effect chain with a plugin sandwiched between two built-ins, so the
    // round trip has to preserve both the ordering and the mixed kinds.
    {
        EffectSlot filterSlot;
        filterSlot.kind             = EffectKind::Filter;
        filterSlot.enabled          = true;
        filterSlot.filter.enabled   = true;
        filterSlot.filter.mode      = 2;
        filterSlot.filter.cutoff    = 3200.0f;
        filterSlot.filter.resonance = 0.9f;

        EffectSlot pluginSlot;
        pluginSlot.kind              = EffectKind::Plugin;
        pluginSlot.enabled           = true;
        pluginSlot.plugin.format     = PluginFormat::VST3;
        pluginSlot.plugin.identifier = "/Library/Audio/Plug-Ins/VST3/Some EQ.vst3";
        pluginSlot.plugin.name       = "Some EQ";       // with a space, deliberately
        pluginSlot.plugin.state      = "YmFzZTY0LXN0YXRl";

        EffectSlot delaySlot;
        delaySlot.kind           = EffectKind::Delay;
        delaySlot.enabled        = true;
        delaySlot.delay.enabled  = true;
        delaySlot.delay.timeMs   = 180.0f;
        delaySlot.delay.feedback = 0.55f;
        delaySlot.delay.mix      = 0.4f;

        s.tracks[0].effectChain = { filterSlot, pluginSlot, delaySlot };
    }

    {
        EffectSlot reverbSlot;
        reverbSlot.kind            = EffectKind::Reverb;
        reverbSlot.enabled         = true;
        reverbSlot.reverb.enabled  = true;
        reverbSlot.reverb.roomSize = 0.8f;
        reverbSlot.reverb.damping  = 0.2f;
        reverbSlot.reverb.mix      = 0.35f;
        s.tracks[2].effectChain = { reverbSlot };
    }

    // A session grid: two scenes, with clips in some cells and not others —
    // the empty ones matter as much, since the slot index is the scene.
    addScene(s, "Intro");
    addScene(s, "Chorus B");        // with a space, deliberately

    Clip sessionClipA;
    sessionClipA.type                = ClipType::Instrument;
    sessionClipA.lengthBeats         = 4.0;
    sessionClipA.pattern.lengthBeats = 4.0;
    sessionClipA.pattern.notes.push_back({ 0.0, 0.5, 62, 0.7f });
    setSessionClip(s, 0, 0, sessionClipA);

    Clip sessionClipB;
    sessionClipB.type                = ClipType::Instrument;
    sessionClipB.lengthBeats         = 8.0;
    sessionClipB.pattern.lengthBeats = 8.0;
    setSessionClip(s, 2, 1, sessionClipB); // drum track, second scene

    // A guitar track in drop-D with non-default tone, so the round trip has to
    // carry both the tuning array and the scalars.
    const int guitarId = addTrack(s, TrackType::Guitar, "Gtr").id;
    Clip guitarClip;
    guitarClip.type                = ClipType::Instrument;
    guitarClip.startBeats          = 12.0;
    guitarClip.lengthBeats         = 4.0;
    guitarClip.pattern.lengthBeats = 4.0;
    guitarClip.pattern.notes.push_back({ 0.0, 1.0, 40, 0.9f });
    addClip(s, guitarId, guitarClip);

    auto& guitar = s.tracks[3].guitarSettings;
    guitar.tuning        = { 38, 45, 50, 55, 59, 64 }; // drop D
    guitar.decaySeconds  = 4.5f;
    guitar.brightness    = 0.35f;
    guitar.pickPosition  = 0.11f;
    guitar.pickHardness  = 0.9f;
    guitar.muteOnNoteOff = 0.25f;

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

TEST_CASE("Guitar settings round-trip, tuning included", "[model][io]")
{
    const Song original = makeSampleSong();

    Song restored;
    REQUIRE(deserialize(serialize(original), restored));

    REQUIRE(restored.tracks[3].type == TrackType::Guitar);

    const auto& guitar = restored.tracks[3].guitarSettings;
    REQUIRE(guitar.tuning[0] == 38); // drop D survives
    REQUIRE(guitar.tuning[5] == 64);
    REQUIRE(guitar.decaySeconds == 4.5f);
    REQUIRE(guitar.pickPosition == 0.11f);
    REQUIRE(guitar.muteOnNoteOff == 0.25f);
}

TEST_CASE("An effect chain round-trips with its order and mixed kinds", "[model][io]")
{
    const Song original = makeSampleSong();

    Song restored;
    REQUIRE(deserialize(serialize(original), restored));

    const auto& chain = restored.tracks[0].effectChain;
    REQUIRE(chain.size() == 3);
    REQUIRE(chain[0].kind == EffectKind::Filter);
    REQUIRE(chain[1].kind == EffectKind::Plugin);
    REQUIRE(chain[2].kind == EffectKind::Delay);

    // The plugin's free-form fields survive intact, spaces and all — the
    // document has to be able to say which plugin it wanted even on a machine
    // that doesn't have it.
    REQUIRE(chain[1].plugin.format == PluginFormat::VST3);
    REQUIRE(chain[1].plugin.name == "Some EQ");
    REQUIRE(chain[1].plugin.identifier == "/Library/Audio/Plug-Ins/VST3/Some EQ.vst3");
    REQUIRE(chain[1].plugin.state == "YmFzZTY0LXN0YXRl");
}

TEST_CASE("A project with the old fixed effect trio migrates into the chain", "[model][io]")
{
    // v17 and earlier stored TFX: one filter, one delay, one reverb, always in
    // that order. They must come back as three slots in the same order, or an
    // existing project's effects would silently rearrange.
    const std::string v17 =
        "LOOPER 17\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "TRACKS 1\n"
        "TRACK 1 0 0 0 0 0 0 Lead\n"
        "TAUTOS 0\n"
        "DRUMKIT 0\n"
        "SYNTH 0 5 120 0.7 250 0 0 1000 0.707 0\n"
        "TFX 1 1 900 1.4 1 275 0.5 0.45 0 0.5 0.5 0.3\n"
        "CLIPS 0\n";

    Song        restored;
    std::string error;
    REQUIRE(deserialize(v17, restored, &error));

    const auto& chain = restored.tracks[0].effectChain;
    REQUIRE(chain.size() == 3);
    REQUIRE(chain[0].kind == EffectKind::Filter);
    REQUIRE(chain[1].kind == EffectKind::Delay);
    REQUIRE(chain[2].kind == EffectKind::Reverb);

    // The filter and delay were on, the reverb off — and their settings come
    // across, so the track sounds as it did.
    REQUIRE(chain[0].enabled);
    REQUIRE(chain[0].filter.mode == 1);
    REQUIRE(chain[0].filter.cutoff == 900.0f);
    REQUIRE(chain[1].enabled);
    REQUIRE(chain[1].delay.timeMs == 275.0f);
    REQUIRE_FALSE(chain[2].enabled);
}

TEST_CASE("The session grid round-trips, empty cells included", "[model][io]")
{
    const Song original = makeSampleSong();

    Song restored;
    REQUIRE(deserialize(serialize(original), restored));

    REQUIRE(restored.scenes.size() == 2);
    REQUIRE(restored.scenes[1].name == "Chorus B");

    // Every track's column stays the same length as the scene list, so the
    // grid can't go ragged on a round trip.
    for (const auto& track : restored.tracks)
        REQUIRE(track.sessionSlots.size() == restored.scenes.size());

    const auto* filled = sessionClip(restored, 0, 0);
    REQUIRE(filled != nullptr);
    REQUIRE(filled->pattern.notes.size() == 1);
    REQUIRE(filled->pattern.notes[0].noteNumber == 62);

    REQUIRE(sessionClip(restored, 0, 1) == nullptr); // deliberately empty
    REQUIRE(sessionClip(restored, 2, 1) != nullptr);
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
        "TAUTO 2\n"
        "TAPT 0 -12\n"
        "TAPT 4 0\n"
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
    // v11 predates inserts entirely, so there's no chain at all.
    REQUIRE(track.effectChain.empty());

    // Guitar settings arrived in v19; a file this old gets the defaults, which
    // are standard tuning.
    REQUIRE(track.guitarSettings == GuitarSettings {});

    // The session grid arrived in v17; a file this old simply has none.
    REQUIRE(restored.scenes.empty());
    REQUIRE(track.sessionSlots.empty());

    // Pan joined TRACK in v15; this file predates it, so it reads as centred.
    REQUIRE(track.pan == 0.0f);

    // Its single unkeyed gain lane (all v15-and-earlier files had exactly
    // one, always gain) must land in the Gain lane rather than be dropped.
    const auto* gainLane = track.lane(TrackParam::Gain);
    REQUIRE(gainLane != nullptr);
    REQUIRE(gainLane->points().size() == 2);
    REQUIRE(track.lane(TrackParam::Pan) == nullptr);
}

TEST_CASE("A current-format file still round-trips after the version work", "[model][io]")
{
    const Song original = makeSampleSong();

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored == original);
}
