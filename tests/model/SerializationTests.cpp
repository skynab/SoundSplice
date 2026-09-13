#include <catch2/catch_test_macros.hpp>

#include <model/Serialization.h>
#include <model/Song.h>

using namespace soundsplice::model;

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
    s.eq.enabled   = true;
    s.eq.bassDb    = 4.5f;
    s.eq.midDb     = -2.0f;
    s.eq.trebleDb  = 3.0f;
    s.projectRootFolder     = "/Users/test/My SoundSplice Projects"; // with a space, deliberately
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

    const int bassId = addTrack(s, TrackType::Instrument, "Bass").id;
    Clip bassClip;
    bassClip.type             = ClipType::Instrument;
    bassClip.startBeats       = 16.0;
    bassClip.lengthBeats      = 4.0;
    bassClip.pattern.lengthBeats = 4.0;
    bassClip.pattern.notes.push_back({ 0.0, 0.25, 36, 1.0f });
    bassClip.pattern.notes.push_back({ 1.0, 0.25, 38, 0.9f });
    addClip(s, bassId, bassClip);

    // Set solo/mute by index (not the returned reference — a later addTrack can
    // reallocate the vector and invalidate it).
    s.tracks[0].colour    = 0xff36618e; // a track colour, so the round trip has to carry it
    s.tracks[2].colour    = 0xffb0413e;
    s.tracks[0].gainDb    = -4.5f;
    s.tracks[0].solo      = true;
    s.tracks[0].pan       = -0.75f;
    s.tracks[1].gainDb    = 3.25f; // above unity, and positive
    s.tracks[1].pan       = 0.5f;
    s.tracks[1].muted     = true;
    s.tracks[0].laneFor(TrackParam::Gain).addPoint(0.0, -20.0f);
    s.tracks[0].laneFor(TrackParam::Gain).addPoint(4.0, 0.0f);
    s.tracks[0].laneFor(TrackParam::Pan).addPoint(0.0, -1.0f);
    s.tracks[0].laneFor(TrackParam::Pan).addPoint(8.0, 1.0f);

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
    setSessionClip(s, 2, 1, sessionClipB); // bass track, second scene

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
    REQUIRE_FALSE(deserialize("not a SoundSplice file", out));
    REQUIRE_FALSE(deserialize("", out));
    REQUIRE_FALSE(deserialize("SOUNDSPLICE 1\nBPM 120\n", out)); // truncated (missing later records)
}

TEST_CASE("deserialize reports why it failed", "[model][io]")
{
    Song        out;
    std::string error;

    REQUIRE_FALSE(deserialize("not a SoundSplice file", out, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("A file from a newer build is refused, not part-parsed", "[model][io]")
{
    // Reading it with this build's rules would silently drop whatever records
    // it gained — worse than declining to open it.
    const std::string newer = "SOUNDSPLICE " + std::to_string(kFormatVersion + 1) + "\nBPM 120\n";

    Song        out;
    std::string error;
    REQUIRE_FALSE(deserialize(newer, out, &error));
    REQUIRE(error.find("newer") != std::string::npos);
}

TEST_CASE("A Looper-Audio project is not read", "[model][io]")
{
    // SoundSplice started from Looper-Audio but does not open its files: the
    // header is different, and the parse stops there with a reason.
    const std::string looper = "LOOPER 41\nBPM 120\nTSNUM 4\nTSDEN 4\nNEXTID 1\nTRACKS 0\n";

    Song        out;
    std::string error;
    REQUIRE_FALSE(deserialize(looper, out, &error));
    REQUIRE(error.find("SoundSplice") != std::string::npos);
}

TEST_CASE("A track of an unknown type is refused, not guessed at", "[model][io]")
{
    const std::string text =
        "SOUNDSPLICE 1\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 2\n"
        "TRACKS 1\n"
        "TRACK 1 7 0 0 0 0 0 Mystery\n"
        "CLIPS 0\n";

    Song        out;
    std::string error;
    REQUIRE_FALSE(deserialize(text, out, &error));
    REQUIRE(error.find("track type") != std::string::npos);
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

TEST_CASE("A clip's source offset round-trips, and defaults to the file's start", "[model][io]")
{
    Song original = makeSampleSong();
    original.tracks[0].clips[0].type                = ClipType::Audio;
    original.tracks[0].clips[0].audioFile           = "/Users/me/My Recordings/take one.wav";
    original.tracks[0].clips[0].sourceOffsetSeconds = 12.345678901234567;

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored.tracks[0].clips[0].sourceOffsetSeconds
            == original.tracks[0].clips[0].sourceOffsetSeconds);
    REQUIRE(restored == original);

    // CLIPSRC is optional: a file without it plays every clip from the start
    // of its file, which is what a clip meant before offsets existed.
    auto text = serialize(original);
    for (auto at = text.find("CLIPSRC "); at != std::string::npos; at = text.find("CLIPSRC "))
        text.erase(at, text.find('\n', at) - at + 1);

    Song withoutOffsets;
    REQUIRE(deserialize(text, withoutOffsets, &error));
    REQUIRE(withoutOffsets.tracks[0].clips[0].sourceOffsetSeconds == 0.0);
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

