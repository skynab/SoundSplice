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
    s.eq.enabled   = true;
    s.eq.bassDb    = 4.5f;
    s.eq.midDb     = -2.0f;
    s.eq.trebleDb  = 3.0f;
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
    s.tracks[0].colour    = 0xff36618e; // a track colour, so the round trip has to carry it
    s.tracks[2].colour    = 0xffb0413e;
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

        // A drive pedal too, with every field off its default so a version
        // that failed to write one would show up as an inequality.
        EffectSlot driveSlot;
        driveSlot.kind           = EffectKind::Drive;
        driveSlot.enabled        = true;
        driveSlot.drive.enabled  = true;
        driveSlot.drive.drive    = 17.5f;
        driveSlot.drive.tone     = 0.72f;
        driveSlot.drive.level    = 0.44f;
        driveSlot.drive.hardClip = true;
        driveSlot.drive.cabinet  = false;
        driveSlot.drive.asymmetry  = 0.35f;
        driveSlot.drive.oversample = true;

        EffectSlot compSlot;
        compSlot.kind                    = EffectKind::Compressor;
        compSlot.enabled                 = true;
        compSlot.compressor.enabled      = true;
        compSlot.compressor.thresholdDb  = -23.5f;
        compSlot.compressor.ratio        = 6.5f;
        compSlot.compressor.attackMs     = 3.5f;
        compSlot.compressor.releaseMs    = 275.0f;
        compSlot.compressor.makeUpDb     = 4.5f;

        EffectSlot tremSlot;
        tremSlot.kind            = EffectKind::Tremolo;
        tremSlot.enabled         = true;
        tremSlot.tremolo.enabled = true;
        tremSlot.tremolo.rateHz  = 6.25f;
        tremSlot.tremolo.depth   = 0.85f;

        EffectSlot chorusSlot;
        chorusSlot.kind           = EffectKind::Chorus;
        chorusSlot.enabled        = true;
        chorusSlot.chorus.enabled = true;
        chorusSlot.chorus.rateHz  = 1.75f;
        chorusSlot.chorus.depth   = 0.68f;
        chorusSlot.chorus.mix     = 0.42f;

        EffectSlot wobbleSlot;
        wobbleSlot.kind                 = EffectKind::Wobble;
        wobbleSlot.enabled              = true;
        wobbleSlot.wobble.enabled       = true;
        wobbleSlot.wobble.rateBeats     = 0.5f;
        wobbleSlot.wobble.depth         = 0.88f;
        wobbleSlot.wobble.baseCutoffHz  = 310.0f;
        wobbleSlot.wobble.resonance     = 1.2f;
        wobbleSlot.wobble.mix           = 0.95f;

        s.tracks[0].effectChain = { filterSlot, pluginSlot, delaySlot, driveSlot, compSlot,
                                    tremSlot, chorusSlot, wobbleSlot };
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
    guitar.pickupResonanceHz = 4200.0f;
    guitar.pickupQ           = 2.25f;

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
    REQUIRE(guitar.pickupResonanceHz == 4200.0f);
    REQUIRE(guitar.pickupQ == 2.25f);
}

TEST_CASE("An effect chain round-trips with its order and mixed kinds", "[model][io]")
{
    const Song original = makeSampleSong();

    Song restored;
    REQUIRE(deserialize(serialize(original), restored));

    const auto& chain = restored.tracks[0].effectChain;
    REQUIRE(chain.size() == 8);
    REQUIRE(chain[0].kind == EffectKind::Filter);
    REQUIRE(chain[1].kind == EffectKind::Plugin);
    REQUIRE(chain[2].kind == EffectKind::Delay);
    REQUIRE(chain[3].kind == EffectKind::Drive);

    // Every drive field, including the two booleans — a pedal that came back
    // with its cabinet switched on when it was saved off is a different sound.
    REQUIRE(chain[3].drive.drive == 17.5f);
    REQUIRE(chain[3].drive.tone == 0.72f);
    REQUIRE(chain[3].drive.level == 0.44f);
    REQUIRE(chain[3].drive.hardClip);
    REQUIRE_FALSE(chain[3].drive.cabinet);
    REQUIRE(chain[3].drive.asymmetry == 0.35f);
    REQUIRE(chain[3].drive.oversample);

    REQUIRE(chain[4].kind == EffectKind::Compressor);
    REQUIRE(chain[4].compressor.thresholdDb == -23.5f);
    REQUIRE(chain[4].compressor.ratio == 6.5f);
    REQUIRE(chain[4].compressor.attackMs == 3.5f);
    REQUIRE(chain[4].compressor.releaseMs == 275.0f);
    REQUIRE(chain[4].compressor.makeUpDb == 4.5f);

    REQUIRE(chain[5].kind == EffectKind::Tremolo);
    REQUIRE(chain[5].tremolo.rateHz == 6.25f);
    REQUIRE(chain[5].tremolo.depth == 0.85f);

    REQUIRE(chain[6].kind == EffectKind::Chorus);
    REQUIRE(chain[6].chorus.rateHz == 1.75f);
    REQUIRE(chain[6].chorus.depth == 0.68f);
    REQUIRE(chain[6].chorus.mix == 0.42f);

    REQUIRE(chain[7].kind == EffectKind::Wobble);
    REQUIRE(chain[7].wobble.rateBeats == 0.5f);
    REQUIRE(chain[7].wobble.depth == 0.88f);
    REQUIRE(chain[7].wobble.baseCutoffHz == 310.0f);
    REQUIRE(chain[7].wobble.resonance == 1.2f);
    REQUIRE(chain[7].wobble.mix == 0.95f);

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

    // The master EQ arrived in v25; a file this old has no EQ line, so it
    // reads as flat/disabled rather than failing to parse.
    REQUIRE(restored.eq == EqSettings {});

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

TEST_CASE("The mastering rack round-trips", "[model][io]")
{
    Song original = makeSampleSong();
    auto& m = original.mastering;
    m.enabled            = true;
    m.lowShelfHz         = 95.0f;
    m.lowShelfDb         = 2.5f;
    m.peakHz             = 2200.0f;
    m.peakDb             = -3.5f;
    m.peakQ              = 1.4f;
    m.highShelfHz        = 9500.0f;
    m.highShelfDb        = 1.5f;
    m.exciterAmount      = 0.35f;
    m.exciterCrossoverHz = 4200.0f;
    m.width              = 1.25f;
    m.reverbAmount       = 0.15f;
    m.reverbRoomSize     = 0.65f;
    m.maximizerInputDb   = 6.0f;
    m.maximizerCeilingDb = -0.7f;
    m.maximizerReleaseMs = 140.0f;
    m.outputGainDb       = -1.5f;

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored.mastering == original.mastering);
    REQUIRE(restored == original);
}

TEST_CASE("A project from before the mastering rack opens neutral", "[model][io]")
{
    // A v28 file has no MASTERING record. Every field's default is a no-op,
    // so an old project must open sounding exactly as it did — in particular
    // the rack must come back *disabled*, and the filter frequencies must be
    // real values rather than zeros, which would be broken filters.
    const std::string v28 =
        "LOOPER 28\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 1\n"
        "TRACK 1 0 0 0 0 0 Synth\n"
        "TAUTOS 0\n"
        "FXCHAIN 0\n"
        "SESSION 0\n"
        "CLIPS 1\n"
        "CLIP 2 0 0 4 4 \n"
        "CLIPGAIN 0\n"
        "NOTES 0\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(v28, song, &error));
    REQUIRE(song.mastering == MasteringSettings {});
    REQUIRE_FALSE(song.mastering.enabled);
    REQUIRE(song.mastering.peakQ > 0.0f);
    REQUIRE(song.mastering.lowShelfHz > 0.0f);
}

TEST_CASE("A v29 guitar track opens with a real pickup, not a 0Hz one", "[model][io]")
{
    // v29's GUITAR line ends after muteOnNoteOff. The two pickup fields are
    // read from the same istringstream, so they must be seeded with the
    // defaults before the extraction - left at 0 they would give a 0Hz, 0-Q
    // resonance, i.e. a broken filter on every project made before v30.
    const std::string v29 =
        "LOOPER 29\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "MASTERING 0 0 0 0 0 0 0 0 0 0 200 0.707 1000 0.707 4000 0.707\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 1\n"
        "TRACK 1 0 0 0 0 2 Guitar\n"
        "TAUTOS 0\n"
        "GUITAR 40 45 50 55 59 64 3 0.7 0.22 0.6 0.25\n"
        "FXCHAIN 0\n"
        "SESSION 0\n"
        "CLIPS 1\n"
        "CLIP 2 0 0 4 4 \n"
        "CLIPGAIN 0\n"
        "NOTES 0\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(v29, song, &error));
    REQUIRE(song.tracks.size() == 1);

    const auto& guitar = song.tracks.front().guitarSettings;
    REQUIRE(guitar.muteOnNoteOff == 0.25f); // the last field the old line had
    REQUIRE(guitar.pickupResonanceHz == GuitarSettings {}.pickupResonanceHz);
    REQUIRE(guitar.pickupQ == GuitarSettings {}.pickupQ);
}

TEST_CASE("Note articulation round-trips", "[model][io]")
{
    Song original = makeSampleSong();
    REQUIRE_FALSE(original.tracks.empty());

    auto& notes = original.tracks.front().clips.front().pattern.notes;
    REQUIRE(notes.size() >= 2);

    notes[0].articulation = looper::engine::Articulation::PalmMute;
    notes[1].articulation = looper::engine::Articulation::Normal;

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));

    const auto& back = restored.tracks.front().clips.front().pattern.notes;
    REQUIRE(back.size() == notes.size());
    CHECK(back[0].articulation == looper::engine::Articulation::PalmMute);
    CHECK(back[1].articulation == looper::engine::Articulation::Normal);

    // And the whole song still compares equal, which is what history dedup
    // relies on — a field that round-trips but breaks operator== would make
    // every save look like an edit.
    CHECK(restored == original);
}

TEST_CASE("A v30 note opens as an open note", "[model][io]")
{
    // v30's NOTE line ends after the velocity. Absent must mean Normal: every
    // note written before articulations existed was played open, and reading
    // one as a palm mute would silently rewrite old parts.
    const std::string v30 =
        "LOOPER 30\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "MASTERING 0 0 0 0 0 0 0 0 0 0 200 0.707 1000 0.707 4000 0.707\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 1\n"
        "TRACK 1 0 0 0 0 0 Synth\n"
        "TAUTOS 0\n"
        "FXCHAIN 0\n"
        "SESSION 0\n"
        "CLIPS 1\n"
        "CLIP 2 0 0 4 4 \n"
        "CLIPGAIN 0\n"
        "NOTES 1\n"
        "NOTE 0 1 60 0.8\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(v30, song, &error));
    REQUIRE(song.tracks.size() == 1);

    const auto& notes = song.tracks.front().clips.front().pattern.notes;
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].noteNumber == 60);
    CHECK(notes[0].articulation == looper::engine::Articulation::Normal);
}

TEST_CASE("Per-clip gain round-trips", "[model][io]")
{
    Song original = makeSampleSong();
    REQUIRE_FALSE(original.tracks.empty());
    REQUIRE_FALSE(original.tracks[0].clips.empty());
    original.tracks[0].clips[0].gainDb = -4.5f;

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored.tracks[0].clips[0].gainDb == -4.5f);
    REQUIRE(restored == original);
}

TEST_CASE("Clip gain survives an audio path containing spaces", "[model][io]")
{
    // The reason CLIPGAIN is its own record rather than another field on the
    // CLIP line: audioFile is read with getline to the end of the line, so
    // anything written after it would be swallowed into the path. A path
    // with spaces is the case that would have exposed it.
    Song original = makeSampleSong();
    original.tracks[0].clips[0].type      = ClipType::Audio;
    original.tracks[0].clips[0].audioFile = "/Users/me/My Recordings/take one.wav";
    original.tracks[0].clips[0].gainDb    = 3.0f;

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored.tracks[0].clips[0].audioFile == "/Users/me/My Recordings/take one.wav");
    REQUIRE(restored.tracks[0].clips[0].gainDb == 3.0f);
}

TEST_CASE("A project from before per-clip gain opens at unity", "[model][io]")
{
    // A v27 file: CLIP goes straight to NOTES with no CLIPGAIN between them.
    // Silence is not a safe default for a gain, so the check that matters is
    // that the missing record leaves 0 dB rather than -inf or garbage.
    const std::string v27 =
        "LOOPER 27\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 1\n"
        "TRACK 1 0 0 0 0 0 Synth\n"
        "TAUTOS 0\n"
        "FXCHAIN 0\n"
        "SESSION 0\n"
        "CLIPS 1\n"
        "CLIP 2 0 0 4 4 \n"
        "NOTES 0\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(v27, song, &error));
    REQUIRE(song.tracks.size() == 1);
    REQUIRE(song.tracks[0].clips.size() == 1);
    REQUIRE(song.tracks[0].clips[0].gainDb == 0.0f);
}

TEST_CASE("Tempo changes round-trip", "[model][io]")
{
    Song original = makeSampleSong();
    original.bpm = 128.0;
    original.tempoChanges = { { 16.0, 90.0 }, { 32.0, 160.0 } };

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));

    CHECK(restored.bpm == 128.0);
    REQUIRE(restored.tempoChanges.size() == 2);
    CHECK(restored.tempoChanges[0].beat == 16.0);
    CHECK(restored.tempoChanges[0].bpm == 90.0);
    CHECK(restored.tempoChanges[1].beat == 32.0);
    CHECK(restored.tempoChanges[1].bpm == 160.0);
}

TEST_CASE("A v31 project is one tempo for the whole song", "[model][io]")
{
    // No TEMPOS record at all. Absent has to mean "one tempo", which is what
    // BPM alone always meant — so an old project migrates by definition.
    const std::string v31 =
        "LOOPER 31\n"
        "BPM 137\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "MASTERING 0 0 0 0 0 0 0 0 0 0 200 0.707 1000 0.707 4000 0.707\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 0\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(v31, song, &error));

    CHECK(song.bpm == 137.0);
    CHECK(song.tempoChanges.empty());
}

TEST_CASE("A tempo change with a nonsense value is dropped, not loaded", "[model][io]")
{
    // These come from a document. A zero or negative tempo divides by zero
    // deep inside playback, and beat 0 belongs to BPM.
    const std::string bad =
        "LOOPER 32\n"
        "BPM 120\n"
        "TEMPOS 4\n"
        "TEMPOAT 8 0\n"
        "TEMPOAT 12 -40\n"
        "TEMPOAT 0 200\n"
        "TEMPOAT 16 90\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "MASTERING 0 0 0 0 0 0 0 0 0 0 200 0.707 1000 0.707 4000 0.707\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 0\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(bad, song, &error));

    REQUIRE(song.tempoChanges.size() == 1);
    CHECK(song.tempoChanges[0].beat == 16.0);
    CHECK(song.tempoChanges[0].bpm == 90.0);
}

TEST_CASE("The song's tempo map joins its starting tempo to its changes", "[model][tempo]")
{
    // Two fields, one map. Anything assembling its own would be the second
    // definition of how they combine, which is how the two drift.
    Song song;
    song.bpm = 100.0;
    song.tempoChanges = { { 8.0, 140.0 }, { 24.0, 70.0 } };

    const auto map = tempoMapFor(song);

    REQUIRE(map.size() == 3);
    CHECK(map[0].beat == 0.0);
    CHECK(map[0].bpm == 100.0);
    CHECK(map[1].beat == 8.0);
    CHECK(map[2].beat == 24.0);
}

TEST_CASE("The tempo at a beat is the last change at or before it", "[model][tempo]")
{
    Song song;
    song.bpm = 100.0;
    song.tempoChanges = { { 8.0, 140.0 }, { 24.0, 70.0 } };

    CHECK(tempoAtBeat(song, 0.0) == 100.0);
    CHECK(tempoAtBeat(song, 7.99) == 100.0);
    CHECK(tempoAtBeat(song, 8.0) == 140.0);   // a change owns its own instant
    CHECK(tempoAtBeat(song, 23.9) == 140.0);
    CHECK(tempoAtBeat(song, 24.0) == 70.0);
    CHECK(tempoAtBeat(song, 1000.0) == 70.0);

    // Before the start reads as the starting tempo rather than as nothing.
    CHECK(tempoAtBeat(song, -5.0) == 100.0);
}

TEST_CASE("A song with no changes has a one-entry map", "[model][tempo]")
{
    Song song;
    song.bpm = 118.0;

    const auto map = tempoMapFor(song);
    REQUIRE(map.size() == 1);
    CHECK(map[0].beat == 0.0);
    CHECK(map[0].bpm == 118.0);
    CHECK(tempoAtBeat(song, 999.0) == 118.0);
}

TEST_CASE("A tempo ramp round-trips", "[model][io]")
{
    Song original = makeSampleSong();
    original.bpm = 90.0;
    original.tempoChanges = { { 8.0, 150.0, true }, { 24.0, 100.0, false } };

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));

    REQUIRE(restored.tempoChanges.size() == 2);
    CHECK(restored.tempoChanges[0].ramp);
    CHECK_FALSE(restored.tempoChanges[1].ramp);
    CHECK(restored == original);
}

TEST_CASE("A v32 tempo change is a step, not a ramp", "[model][io]")
{
    // v32's TEMPOAT line ends after the tempo. Absent has to mean "step",
    // which is what every change written before ramps existed was — reading
    // one as a ramp would silently reshape an existing arrangement.
    const std::string v32 =
        "LOOPER 32\n"
        "BPM 120\n"
        "TEMPOS 1\n"
        "TEMPOAT 16 80\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 3\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "SENDBUS 0 0 0.5 0.5 300 0.35 0.5\n"
        "EQ 0 0 0 0\n"
        "MASTERING 0 0 0 0 0 0 0 0 0 0 200 0.707 1000 0.707 4000 0.707\n"
        "PROJECTROOT \n"
        "AUTO 0\n"
        "SCENES 0\n"
        "TRACKS 0\n";

    Song        song;
    std::string error;
    REQUIRE(deserialize(v32, song, &error));

    REQUIRE(song.tempoChanges.size() == 1);
    CHECK(song.tempoChanges[0].bpm == 80.0);
    CHECK_FALSE(song.tempoChanges[0].ramp);
}

TEST_CASE("Clip warp settings round-trip", "[model][io]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Audio, "Loop").id;

    Clip clip;
    clip.type        = ClipType::Audio;
    clip.audioFile   = "/loops/break 174.wav"; // a space, since CLIP's path is rest-of-line
    clip.startBeats  = 8.0;
    clip.lengthBeats = 16.0;
    clip.sourceBpm   = 174.0;
    clip.warpEnabled = true;
    addClip(s, trackId, clip);

    Song restored;
    REQUIRE(deserialize(serialize(s), restored));
    REQUIRE(restored.tracks.size() == 1);
    REQUIRE(restored.tracks[0].clips.size() == 1);

    const auto& out = restored.tracks[0].clips[0];
    REQUIRE(out.warpEnabled);
    REQUIRE(out.sourceBpm == 174.0);
    // The record sits between CLIPGAIN and NOTES, so the path either side of
    // it has to survive intact too.
    REQUIRE(out.audioFile == "/loops/break 174.wav");
}

TEST_CASE("A clip with a known tempo need not be warped", "[model][io]")
{
    // The two fields are independent on purpose: "set the project tempo from
    // this clip" wants the tempo without the stretching.
    Song s;
    const int trackId = addTrack(s, TrackType::Audio, "Loop").id;

    Clip clip;
    clip.type        = ClipType::Audio;
    clip.sourceBpm   = 92.5;
    clip.warpEnabled = false;
    addClip(s, trackId, clip);

    Song restored;
    REQUIRE(deserialize(serialize(s), restored));

    const auto& out = restored.tracks[0].clips[0];
    REQUIRE_FALSE(out.warpEnabled);
    REQUIRE(out.sourceBpm == 92.5);
}

TEST_CASE("A file written before warping existed reads as unwarped", "[model][io]")
{
    // The compatibility claim in the format notes: no CLIPWARP record means
    // the clip plays at its own rate, which is how those files always played.
    Song s;
    const int trackId = addTrack(s, TrackType::Audio, "Loop").id;

    Clip clip;
    clip.type      = ClipType::Audio;
    clip.audioFile = "/loops/old.wav";
    addClip(s, trackId, clip);

    // Strip the record the way an older writer would simply never have emitted.
    std::string text = serialize(s);
    const auto  start = text.find("CLIPWARP");
    REQUIRE(start != std::string::npos);
    text.erase(start, text.find('\n', start) - start + 1);
    REQUIRE(text.find("CLIPWARP") == std::string::npos);

    Song restored;
    REQUIRE(deserialize(text, restored));

    const auto& out = restored.tracks[0].clips[0];
    REQUIRE_FALSE(out.warpEnabled);
    REQUIRE(out.sourceBpm == 0.0);
    REQUIRE(out.audioFile == "/loops/old.wav");
}

TEST_CASE("A compressor's sidechain routing round-trips", "[model][io]")
{
    Song s;
    const int kickId = addTrack(s, TrackType::Drum, "Kick").id;
    const int bassId = addTrack(s, TrackType::Instrument, "Bass").id;

    EffectSlot ducker;
    ducker.kind                          = EffectKind::Compressor;
    ducker.enabled                       = true;
    ducker.compressor.enabled            = true;
    ducker.compressor.sidechainTrackId   = kickId;
    findTrack(s, bassId)->effectChain.push_back(ducker);

    Song restored;
    REQUIRE(deserialize(serialize(s), restored));

    const auto* bass = findTrack(restored, bassId);
    REQUIRE(bass != nullptr);
    REQUIRE(bass->effectChain.size() == 1);
    // The *id*, not an index — which is the whole point: it has to survive
    // the track order changing.
    REQUIRE(bass->effectChain[0].compressor.sidechainTrackId == kickId);
}

TEST_CASE("A compressor with no sidechain round-trips as unrouted", "[model][io]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "Lead").id;

    EffectSlot plain;
    plain.kind               = EffectKind::Compressor;
    plain.enabled            = true;
    plain.compressor.enabled = true;
    findTrack(s, trackId)->effectChain.push_back(plain);

    Song restored;
    REQUIRE(deserialize(serialize(s), restored));
    REQUIRE(findTrack(restored, trackId)->effectChain[0].compressor.sidechainTrackId == -1);
}

TEST_CASE("A file written before sidechains reads as unrouted", "[model][io]")
{
    // v35 appended the field to FXSLOT's positional line; an older file simply
    // ends sooner, and the extraction leaves the default in place.
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "Lead").id;

    EffectSlot ducker;
    ducker.kind                        = EffectKind::Compressor;
    ducker.enabled                     = true;
    ducker.compressor.enabled          = true;
    ducker.compressor.sidechainTrackId = 7;
    findTrack(s, trackId)->effectChain.push_back(ducker);

    // Truncate the line where a pre-v35 writer would have stopped: before the
    // sidechain field and everything appended after it.
    //
    // Written as "drop the last N fields" rather than "drop the last field",
    // because it *was* the latter and silently stopped testing anything the
    // moment v39 and v40 appended two more — it then stripped the cabinet-IR
    // flag and asserted a sidechain that was still present. Any future
    // appended field has to be counted here too.
    constexpr int kFieldsAfterV34 = 3; // sidechain id, drive stages, cabinet IR

    std::string text  = serialize(s);
    const auto  start = text.find("FXSLOT");
    REQUIRE(start != std::string::npos);

    auto lineEnd = text.find('\n', start);
    for (int i = 0; i < kFieldsAfterV34; ++i)
    {
        const auto lastSpace = text.rfind(' ', lineEnd);
        REQUIRE(lastSpace != std::string::npos);
        REQUIRE(lastSpace > start);
        text.erase(lastSpace, lineEnd - lastSpace);
        lineEnd = text.find('\n', start);
    }

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(findTrack(restored, trackId)->effectChain[0].compressor.sidechainTrackId == -1);
}

TEST_CASE("Group bus routing round-trips", "[model][io]")
{
    Song s;
    const int busId  = addTrack(s, TrackType::Bus, "Drum Bus").id;
    const int kickId = addTrack(s, TrackType::Drum, "Kick").id;
    findTrack(s, kickId)->outputBusId = busId;

    Song restored;
    REQUIRE(deserialize(serialize(s), restored));

    const auto* bus = findTrack(restored, busId);
    REQUIRE(bus != nullptr);
    REQUIRE(bus->type == TrackType::Bus);

    // The id, not an index — the whole reason it is stored that way.
    REQUIRE(findTrack(restored, kickId)->outputBusId == busId);
    // A bus itself goes to the master.
    REQUIRE(bus->outputBusId == -1);
}

TEST_CASE("A file written before group buses reads as feeding the master", "[model][io]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "Lead").id;
    findTrack(s, trackId)->outputBusId = 3;

    std::string text  = serialize(s);
    const auto  start = text.find("TRACKBUS");
    REQUIRE(start != std::string::npos);
    text.erase(start, text.find('\n', start) - start + 1);
    REQUIRE(text.find("TRACKBUS") == std::string::npos);

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(findTrack(restored, trackId)->outputBusId == -1);
}

TEST_CASE("Sustain-pedal movements round-trip", "[model][io]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "Piano").id;

    Clip clip;
    clip.type = ClipType::Instrument;
    clip.pattern.lengthBeats = 8.0;
    clip.pattern.notes.push_back({ 0.0, 1.0, 60, 0.8f });
    clip.pattern.pedals.push_back({ 0.0, true });
    clip.pattern.pedals.push_back({ 3.5, false });
    clip.pattern.pedals.push_back({ 4.0, true });
    addClip(s, trackId, clip);

    Song restored;
    REQUIRE(deserialize(serialize(s), restored));

    const auto& pedals = findTrack(restored, trackId)->clips[0].pattern.pedals;
    REQUIRE(pedals.size() == 3);
    REQUIRE(pedals[0].beat == 0.0);
    REQUIRE(pedals[0].down);
    REQUIRE(pedals[1].beat == 3.5);
    REQUIRE_FALSE(pedals[1].down);
    REQUIRE(pedals[2].down);

    // The notes either side of the new record have to survive it.
    REQUIRE(findTrack(restored, trackId)->clips[0].pattern.notes.size() == 1);
}

TEST_CASE("A file written before pedals reads without them", "[model][io]")
{
    Song s;
    const int trackId = addTrack(s, TrackType::Instrument, "Piano").id;

    Clip clip;
    clip.type = ClipType::Instrument;
    clip.pattern.notes.push_back({ 0.0, 1.0, 60, 0.8f });
    addClip(s, trackId, clip);

    std::string text  = serialize(s);
    const auto  start = text.find("PEDALS");
    REQUIRE(start != std::string::npos);
    text.erase(start, text.find('\n', start) - start + 1);

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(findTrack(restored, trackId)->clips[0].pattern.pedals.empty());
    REQUIRE(findTrack(restored, trackId)->clips[0].pattern.notes.size() == 1);
}
