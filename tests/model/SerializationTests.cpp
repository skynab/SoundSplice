#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>

#include <model/EffectParams.h>
#include <model/Serialization.h>
#include <model/Song.h>

using namespace soundsplice::model;

static Song makeSampleSong()
{
    Song s;
    s.bpm                = 128.0;
    s.timeSigNumerator   = 3;
    s.timeSigDenominator = 4;
    {
        auto delay           = makeEffectSlot(EffectKind::Delay);
        delay.delay.timeMs   = 250.0f;
        delay.delay.feedback = 0.4f;
        delay.delay.mix      = 0.5f;
        auto limiter         = makeEffectSlot(EffectKind::Limiter);
        limiter.enabled      = false; // bypassed, which has to survive too
        s.masterEffects      = { delay, limiter };
    }
    s.projectRootFolder     = "/Users/test/My SoundSplice Projects"; // with a space, deliberately
    s.masterGainDb.addPoint(0.0, -40.0f);
    s.masterGainDb.addPoint(4.0, 0.0f);
    s.masterGainDb.addPoint(8.0, -6.0f);

    const int leadId = addTrack(s, TrackType::Audio, "Synth Lead").id; // name with a space

    Clip leadClip;
    leadClip.startBeats  = 8.0; // not at the origin: see below
    leadClip.lengthBeats = 4.0;
    leadClip.audioFile   = "lead.wav";
    addClip(s, leadId, leadClip);

    const int voxId = addTrack(s, TrackType::Audio, "Vox").id;
    Clip audioClip;
    audioClip.startBeats = 2.5; // deliberately off the bar line
    audioClip.audioFile  = "takes/vocal 01.wav";
    addClip(s, voxId, audioClip);

    const int bassId = addTrack(s, TrackType::Audio, "Bass").id;
    Clip bassClip;
    bassClip.startBeats  = 16.0;
    bassClip.lengthBeats = 4.0;
    bassClip.audioFile   = "bass.wav";
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

    // An effect chain with a plugin sandwiched between two built-ins, so the
    // round trip has to preserve both the ordering and the mixed kinds.
    {
        EffectSlot filterSlot;
        filterSlot.kind             = EffectKind::Filter;
        filterSlot.enabled          = true;
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
        delaySlot.delay.timeMs   = 180.0f;
        delaySlot.delay.feedback = 0.55f;
        delaySlot.delay.mix      = 0.4f;

        // A drive pedal too, with every field off its default so a version
        // that failed to write one would show up as an inequality.
        EffectSlot driveSlot;
        driveSlot.kind           = EffectKind::Drive;
        driveSlot.enabled        = true;
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
        compSlot.compressor.thresholdDb  = -23.5f;
        compSlot.compressor.ratio        = 6.5f;
        compSlot.compressor.attackMs     = 3.5f;
        compSlot.compressor.releaseMs    = 275.0f;
        compSlot.compressor.makeUpDb     = 4.5f;

        EffectSlot tremSlot;
        tremSlot.kind            = EffectKind::Tremolo;
        tremSlot.enabled         = true;
        tremSlot.tremolo.rateHz  = 6.25f;
        tremSlot.tremolo.depth   = 0.85f;

        EffectSlot chorusSlot;
        chorusSlot.kind           = EffectKind::Chorus;
        chorusSlot.enabled        = true;
        chorusSlot.chorus.rateHz  = 1.75f;
        chorusSlot.chorus.depth   = 0.68f;
        chorusSlot.chorus.mix     = 0.42f;

        EffectSlot wobbleSlot;
        wobbleSlot.kind                 = EffectKind::Wobble;
        wobbleSlot.enabled              = true;
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
        reverbSlot.reverb.roomSize = 0.8f;
        reverbSlot.reverb.damping  = 0.2f;
        reverbSlot.reverb.mix      = 0.35f;
        s.tracks[2].effectChain = { reverbSlot };
    }

    return s;
}

/** @p song as a version 1 file saved it: every built-in's settings on the
    FXSLOT line by position (detail::kPositionalEffectParams), no FXPARAMS.
    Only the first @p fields of them, as a file from before the rest existed. */
static std::string asPositionalFile(const Song& song, size_t fields)
{
    std::istringstream in(serialize(song));
    std::string        out, line;
    while (std::getline(in, line))
    {
        if (line.rfind("FXPARAMS", 0) == 0)
        {
            // Every value by name, the unwritten ones at their defaults.
            EffectSlot slot;
            detail::readEffectParams(line.substr(8), slot);

            std::string values;
            for (size_t i = 0; i < fields; ++i)
            {
                const std::string_view name = detail::kPositionalEffectParams[i];
                const auto*            effect = descriptorFor(name.substr(0, name.find('.')));
                const auto*            param  = paramFor(*effect, name.substr(name.find('.') + 1));
                values += " " + detail::num(paramValue(slot, *param));
            }
            out.pop_back(); // onto the end of the FXSLOT line just written
            out += values + '\n';
            continue;
        }
        if (line.rfind("SOUNDSPLICE", 0) == 0)
            line = "SOUNDSPLICE 1";
        out += line + '\n';
    }
    return out;
}

TEST_CASE("Song survives a serialize/deserialize round trip", "[model][io]")
{
    const Song original = makeSampleSong();

    const std::string text = serialize(original);
    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == original);
}

TEST_CASE("A project with Windows line endings opens the same", "[model][io]")
{
    // As git with autocrlf, or JUCE's File::replaceWithText on Windows,
    // leaves it. The audio file path runs to the end of its line, so a \r
    // left on it named a file that doesn't exist.
    Song original = makeSampleSong();
    original.tracks[0].clips[0].audioFile = "C:\\Audio\\take one.wav";

    std::string crlf;
    for (const char c : serialize(original))
    {
        if (c == '\n')
            crlf += '\r';
        crlf += c;
    }

    Song restored;
    REQUIRE(deserialize(crlf, restored));
    REQUIRE(restored.tracks[0].clips[0].audioFile == "C:\\Audio\\take one.wav");
    REQUIRE(restored == original);
}

TEST_CASE("Project info round-trips, line breaks flattened", "[model][io]")
{
    Song original = makeSampleSong();
    original.info.title    = "Episode 12: The One With Spaces";
    original.info.artist   = "The Host";
    original.info.comment  = "first line";
    original.info.coverArt = "C:/Art/cover art.png";

    Song restored;
    REQUIRE(deserialize(serialize(original), restored));
    REQUIRE(restored.info == original.info);

    original.info.comment = "two\nlines";
    REQUIRE(deserialize(serialize(original), restored));
    REQUIRE(restored.info.comment == "two lines");
    REQUIRE(restored.tracks == original.tracks); // nothing after it was disturbed
}

TEST_CASE("A clip's transcript round-trips with everything around it", "[model][io]")
{
    Song original = makeSampleSong();
    auto& clip    = original.tracks[0].clips[0];
    clip.audioFile = "voice.wav";
    clip.essential.role = SoundRole::Dialogue;     // written before the words
    clip.essential.amounts["noise"] = 4.0f;
    clip.transcript = { { "Hello,", 0.25, 0.6, 0.97f }, { "rock 'n' roll", 0.6, 1.4, 0.5f } };
    clip.envelope.addPoint(0.5, 0.8f);              // and after them

    Song restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored.tracks[0].clips[0].transcript == clip.transcript);
    REQUIRE(restored == original);
}

TEST_CASE("A project's video and where it starts round-trip", "[model][io]")
{
    Song original = makeSampleSong();
    original.videoFile          = "C:/Video/Episode 12 final.mp4";
    original.videoOffsetSeconds = -1.5;

    Song restored;
    REQUIRE(deserialize(serialize(original), restored));
    REQUIRE(restored.videoFile == original.videoFile);
    REQUIRE(restored.videoOffsetSeconds == -1.5);
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

TEST_CASE("The master chain round-trips", "[model][io]")
{
    const Song original = makeSampleSong();
    Song       restored;
    REQUIRE(deserialize(serialize(original), restored));
    REQUIRE(restored.masterEffects == original.masterEffects);
    REQUIRE(restored.masterEffects.size() == 2);
    REQUIRE_FALSE(restored.masterEffects[1].enabled);
}

TEST_CASE("A version 2 file's master effects become the master chain", "[model][io]")
{
    const std::string text =
        "SOUNDSPLICE 2\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 1\n"
        "FILTER 1 1 800 1.2\n"
        "DELAY 0 250 0.4 0.5\n" // off: not carried over
        "REVERB 1 0.7 0.4 0.25\n"
        "EQ 1 4.5 -2 3\n"
        "TRACKS 0\n";

    Song out;
    REQUIRE(deserialize(text, out));
    REQUIRE(out.masterEffects.size() == 3);

    // In the order they ran: filter, (delay,) reverb, EQ.
    const auto& filter = out.masterEffects[0];
    REQUIRE(filter.kind == EffectKind::Filter);
    REQUIRE(filter.enabled);
    REQUIRE(filter.filter.mode == 1);
    REQUIRE(filter.filter.cutoff == 800.0f);
    REQUIRE(filter.filter.resonance == 1.2f);

    const auto& reverb = out.masterEffects[1];
    REQUIRE(reverb.kind == EffectKind::Reverb);
    REQUIRE(reverb.reverb.roomSize == 0.7f);
    REQUIRE(reverb.reverb.damping == 0.4f);
    REQUIRE(reverb.reverb.mix == 0.25f);

    // The master EQ's fixed bands, as an EQ pedal: the same three filters.
    const auto& eq = out.masterEffects[2];
    REQUIRE(eq.kind == EffectKind::Eq);
    REQUIRE(eq.enabled);
    REQUIRE(eq.eqPedal.lowShelfHz == EqSettings::bassHz);
    REQUIRE(eq.eqPedal.lowShelfDb == 4.5f);
    REQUIRE(eq.eqPedal.midHz == EqSettings::midHz());
    REQUIRE(eq.eqPedal.midDb == -2.0f);
    REQUIRE(eq.eqPedal.midQ == 0.7f);
    REQUIRE(eq.eqPedal.highShelfHz == EqSettings::trebleHz);
    REQUIRE(eq.eqPedal.highShelfDb == 3.0f);

    // Saved again, it's a current file with the chain, and reads back the same.
    const auto saved = serialize(out);
    REQUIRE(saved.find("MASTERFX 3") != std::string::npos);
    REQUIRE(saved.find("\nFILTER ") == std::string::npos);
    Song again;
    REQUIRE(deserialize(saved, again));
    REQUIRE(again.masterEffects == out.masterEffects);
}

TEST_CASE("A version 2 file with its master effects off has an empty master chain", "[model][io]")
{
    const std::string text =
        "SOUNDSPLICE 2\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 1\n"
        "FILTER 0 0 1000 0.707\n"
        "DELAY 0 300 0.35 0.3\n"
        "REVERB 0 0.5 0.5 0.3\n"
        "EQ 0 0 0 0\n"
        "TRACKS 0\n";

    Song out;
    REQUIRE(deserialize(text, out));
    REQUIRE(out.masterEffects.empty());
}

TEST_CASE("An earlier file's MIDI is read past, and its MIDI track becomes audio", "[model][io]")
{
    // A version 3 file as that build wrote it: a MIDI track with its synth,
    // a session grid and note clips, beside an audio track.
    const std::string text =
        "SOUNDSPLICE 3\n"
        "BPM 120\n"
        "TSNUM 4\n"
        "TSDEN 4\n"
        "NEXTID 10\n"
        "SCENES 2\n"
        "SCENE Intro\n"
        "SCENE Chorus B\n"
        "TRACKS 2\n"
        "TRACK 1 0 -3 0 0 0 0 Synth Lead\n"
        "SYNTH 2 12 300 0.5 400 1 1 2500 1.5 -3 0 0 0 1 0 0 0.3 1 12\n"
        "FXCHAIN 1\n"
        "FXSLOT 4 1\n"
        "SESSION 2\n"
        "SSLOT 1\n"
        "CLIP 5 0 0 4 4 \n"
        "PEDALS 1\n"
        "PEDAL 0 1\n"
        "NOTES 1\n"
        "NOTE 0 0.5 62 0.7\n"
        "SSLOT 0\n"
        "CLIPS 2\n"
        "CLIP 2 0 8 4 4 \n"
        "CLIPGAIN 0\n"
        "NOTES 2\n"
        "NOTE 0 0.5 60 0.8\n"
        "NOTE 1 0.25 64 0.9\n"
        "CLIP 3 1 12 4 0 takes/vocal 01.wav\n"
        "CLIPGAIN -2\n"
        "NOTES 0\n"
        "TRACK 4 1 0 0 0 0 0 Vox\n"
        "SESSION 2\n"
        "SSLOT 0\n"
        "SSLOT 0\n"
        "CLIPS 1\n"
        "CLIP 6 1 2.5 4 0 vocal.wav\n"
        "PEDALS 0\n"
        "NOTES 0\n";

    Song        out;
    std::string error;
    REQUIRE(deserialize(text, out, &error));
    REQUIRE(out.tracks.size() == 2);

    // The MIDI track keeps everything but its notes: its name, level and
    // effects, and the audio clip it held.
    const auto& lead = out.tracks[0];
    REQUIRE(lead.type == TrackType::Audio);
    REQUIRE(lead.name == "Synth Lead");
    REQUIRE(lead.gainDb == -3.0f);
    REQUIRE(lead.effectChain.size() == 1);
    REQUIRE(lead.clips.size() == 1);
    REQUIRE(lead.clips[0].id == 3);
    REQUIRE(lead.clips[0].startBeats == 12.0);
    REQUIRE(lead.clips[0].audioFile == "takes/vocal 01.wav");
    REQUIRE(lead.clips[0].gainDb == -2.0f);

    const auto& vox = out.tracks[1];
    REQUIRE(vox.type == TrackType::Audio);
    REQUIRE(vox.clips.size() == 1);
    REQUIRE(vox.clips[0].startBeats == 2.5);
    REQUIRE(vox.clips[0].audioFile == "vocal.wav");

    // Saved again, it's a current file with none of it.
    const auto saved = serialize(out);
    for (const char* gone : { "SCENES", "SYNTH", "SESSION", "NOTES", "PEDALS" })
        REQUIRE(saved.find(gone) == std::string::npos);
    Song again;
    REQUIRE(deserialize(saved, again));
    REQUIRE(again == out);
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

TEST_CASE("Clip fades round-trip, and an unknown shape reads as linear", "[model][io]")
{
    using soundsplice::engine::FadeShape;

    Song original = makeSampleSong();
    auto& clip = original.tracks[0].clips[0];
    clip.audioFile        = "/Users/me/My Recordings/take one.wav";
    clip.fades.inSeconds  = 0.25;
    clip.fades.inShape    = FadeShape::EqualPower;
    clip.fades.outSeconds = 1.5;
    clip.fades.outShape   = FadeShape::SCurve;

    Song        restored;
    std::string error;
    REQUIRE(deserialize(serialize(original), restored, &error));
    REQUIRE(restored.tracks[0].clips[0].fades == original.tracks[0].clips[0].fades);
    REQUIRE(restored == original);

    // A shape from a newer build plays as a straight fade here rather than
    // making the whole project unreadable.
    auto text = serialize(original);
    const auto at = text.find("CLIPFADE 0.25 1 1.5 2");
    REQUIRE(at != std::string::npos);
    text.replace(at, text.find('\n', at) - at, "CLIPFADE 0.25 99 1.5 2");

    Song unknownShape;
    REQUIRE(deserialize(text, unknownShape, &error));
    REQUIRE(unknownShape.tracks[0].clips[0].fades.inShape == FadeShape::Linear);
    REQUIRE(unknownShape.tracks[0].clips[0].fades.inSeconds == 0.25);
    REQUIRE(unknownShape.tracks[0].clips[0].fades.outShape == FadeShape::SCurve);
}

TEST_CASE("Utility effects round-trip, and older files load them at their defaults", "[model][io]")
{
    Song      song;
    const int id    = addTrack(song, TrackType::Audio, "Vox").id;
    auto&     chain = findTrack(song, id)->effectChain;

    auto amplify           = makeEffectSlot(EffectKind::Amplify);
    amplify.amplify.gainDb = -7.5f;
    auto invert            = makeEffectSlot(EffectKind::Invert);
    invert.invert.left     = false;
    auto dc                = makeEffectSlot(EffectKind::DcOffset);
    dc.dcOffset.cutoffHz   = 12.5f;
    auto limiter           = makeEffectSlot(EffectKind::Limiter);
    limiter.limiter.inputGainDb = 6.5f;
    limiter.limiter.ceilingDb   = -2.5f;
    limiter.limiter.releaseMs   = 40.0f;
    auto phaser               = makeEffectSlot(EffectKind::Phaser);
    phaser.phaser.stagePairs  = 5;
    phaser.phaser.feedback    = -0.35f;
    auto flanger              = makeEffectSlot(EffectKind::Flanger);
    flanger.flanger.delayMs   = 2.25f;
    auto tone                 = makeEffectSlot(EffectKind::BassTreble);
    tone.bassTreble.trebleDb  = -4.5f;
    auto stereo               = makeEffectSlot(EffectKind::StereoTool);
    stereo.stereoTool.width   = 0.4f;
    stereo.stereoTool.swap    = true;
    auto graphic               = makeEffectSlot(EffectKind::GraphicEq);
    graphic.graphicEq.band1k   = -3.5f;
    graphic.graphicEq.band16k  = 6.0f;
    auto deEss                 = makeEffectSlot(EffectKind::DeEsser);
    deEss.deEsser.frequencyHz  = 7250.0f;
    auto expander              = makeEffectSlot(EffectKind::Expander);
    expander.expander.ratio    = 3.5f;
    auto ring                  = makeEffectSlot(EffectKind::RingMod);
    ring.ringMod.frequencyHz   = 123.0f;
    auto wah                   = makeEffectSlot(EffectKind::Wah);
    wah.wah.resonance          = 7.5f;
    auto echo          = makeEffectSlot(EffectKind::Echo);
    echo.echo.taps     = 6;
    echo.echo.pingPong = true;
    auto multiband                     = makeEffectSlot(EffectKind::Multiband);
    multiband.multiband.lowHz          = 150.0f;
    multiband.multiband.highThresholdDb = -18.5f;
    multiband.multiband.midRatio       = 4.5f;
    auto parametric                       = makeEffectSlot(EffectKind::ParametricEq);
    parametric.parametricEq.band1Type     = 4;
    parametric.parametricEq.band1Hz       = 60.0f;
    parametric.parametricEq.band4GainDb   = -7.5f;
    parametric.parametricEq.band6Q        = 2.25f;
    auto dynamics                  = makeEffectSlot(EffectKind::Dynamics);
    dynamics.dynamics.points       = 4;
    dynamics.dynamics.point3InDb   = -33.5f;
    dynamics.dynamics.point4OutDb  = -12.0f;
    dynamics.dynamics.detector     = 1;
    dynamics.dynamics.makeUpDb     = 2.5f;
    auto graphic31               = makeEffectSlot(EffectKind::GraphicEq31);
    graphic31.graphicEq31.band1  = -6.0f;
    graphic31.graphicEq31.band18 = 3.5f;
    graphic31.graphicEq31.band31 = 12.0f;
    auto mixer                          = makeEffectSlot(EffectKind::ChannelMixer);
    mixer.channelMixer.rightToLeft      = -0.25f;
    mixer.channelMixer.midSide          = 3;
    auto vocoder               = makeEffectSlot(EffectKind::Vocoder);
    vocoder.vocoder.carrier    = 2;
    vocoder.vocoder.bands      = 24;
    vocoder.vocoder.pitchHz    = 82.5f;
    auto convolution                   = makeEffectSlot(EffectKind::Convolution);
    convolution.convolution.irFile     = "C:/Impulses/Big Church (stereo).wav";
    convolution.convolution.mix        = 0.45f;
    convolution.convolution.preDelayMs = 30.0f;
    chain = { amplify, invert, dc, limiter, phaser, flanger, tone, stereo, graphic, deEss, expander, ring, wah, echo,
              multiband, parametric, dynamics, graphic31, convolution, vocoder, mixer };

    Song restored;
    REQUIRE(deserialize(serialize(song), restored));
    REQUIRE(restored.tracks[0].effectChain == chain);

    // A file from before these existed: every FXSLOT line stops after the
    // pedals' values, the 45 that came before the utility effects.
    Song old;
    REQUIRE(deserialize(asPositionalFile(song, 45), old));
    REQUIRE(old.tracks[0].effectChain.size() == 21);
    REQUIRE(old.tracks[0].effectChain[3].kind == EffectKind::Limiter);

    const EffectSlot defaults;
    for (const auto& slot : old.tracks[0].effectChain)
    {
        REQUIRE(slot.amplify == defaults.amplify);
        REQUIRE(slot.invert == defaults.invert);
        REQUIRE(slot.dcOffset == defaults.dcOffset);
        REQUIRE(slot.limiter == defaults.limiter);
        REQUIRE(slot.phaser == defaults.phaser);
        REQUIRE(slot.flanger == defaults.flanger);
        REQUIRE(slot.bassTreble == defaults.bassTreble);
        REQUIRE(slot.stereoTool == defaults.stereoTool);
        REQUIRE(slot.graphicEq == defaults.graphicEq);
        REQUIRE(slot.deEsser == defaults.deEsser);
        REQUIRE(slot.expander == defaults.expander);
        REQUIRE(slot.ringMod == defaults.ringMod);
        REQUIRE(slot.wah == defaults.wah);
        REQUIRE(slot.echo == defaults.echo);
        REQUIRE(slot.multiband == defaults.multiband);
        REQUIRE(slot.parametricEq == defaults.parametricEq);
        REQUIRE(slot.dynamics == defaults.dynamics);
        REQUIRE(slot.graphicEq31 == defaults.graphicEq31);
        REQUIRE(slot.convolution.mix == defaults.convolution.mix);
        REQUIRE(slot.vocoder == defaults.vocoder);
        REQUIRE(slot.channelMixer == defaults.channelMixer);
    }
}

TEST_CASE("A version 1 file's positional effect settings load in full", "[model][io]")
{
    // The table that reads them is frozen: these are the old line's first and
    // last fields and each group's first, as that build wrote them.
    const auto& order = detail::kPositionalEffectParams;
    REQUIRE(std::size(order) == 197);
    REQUIRE(std::string(order[0]) == "filter.mode");
    REQUIRE(std::string(order[18]) == "compressor.threshold");
    REQUIRE(std::string(order[45]) == "amplify.gain");
    REQUIRE(std::string(order[52]) == "phaser.rate");
    REQUIRE(std::string(order[69]) == "graphicEq.band31");
    REQUIRE(std::string(order[93]) == "echo.time");
    REQUIRE(std::string(order[98]) == "multiband.lowCrossover");
    REQUIRE(std::string(order[111]) == "parametricEq.band1Type");
    REQUIRE(std::string(order[135]) == "dynamics.points");
    REQUIRE(std::string(order[152]) == "graphicEq31.band1");
    REQUIRE(std::string(order[183]) == "convolution.mix");
    REQUIRE(std::string(order[186]) == "vocoder.carrier");
    REQUIRE(std::string(order[192]) == "channelMixer.leftToLeft");
    REQUIRE(std::string(order[196]) == "channelMixer.midSide");

    // Every parameter of every built-in at the top of its range, in one chain.
    Song  song;
    auto& track = addTrack(song, TrackType::Audio, "Effects");
    for (const auto& effect : builtInEffects())
    {
        auto slot = makeEffectSlot(effect.kind);
        for (const auto& param : effect.params)
            setParamValue(slot, param, param.max);
        track.effectChain.push_back(slot);
    }

    Song old;
    REQUIRE(deserialize(asPositionalFile(song, std::size(order)), old));
    REQUIRE(old.tracks[0].effectChain == song.tracks[0].effectChain);
}

TEST_CASE("Effect settings are saved by name, and unknown names are skipped", "[model][io]")
{
    Song      song;
    const int id     = addTrack(song, TrackType::Audio, "Vox").id;
    auto      filter = makeEffectSlot(EffectKind::Filter);
    filter.filter.cutoff = 440.0f;
    auto delay           = makeEffectSlot(EffectKind::Delay);
    delay.filter.mode    = 2; // another kind's setting, kept for switching back
    findTrack(song, id)->effectChain = { filter, delay };

    auto text = serialize(song);
    REQUIRE(text.find("FXPARAMS filter.mode=0 filter.cutoff=440 filter.resonance=") != std::string::npos);
    REQUIRE(text.find("FXPARAMS filter.mode=2 delay.time=300 ") != std::string::npos);

    // A name from a newer build, or one no longer used, doesn't stop the rest.
    const auto at = text.find("FXPARAMS ");
    text.insert(at + 9, "ghost.level=3 filter.ghost=1 nonsense filter.cutoff=oops ");

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored.tracks[0].effectChain == findTrack(song, id)->effectChain);
}

TEST_CASE("A clip's spectral edits round-trip", "[model][serialization]")
{
    Song song;
    Track track;
    track.type = TrackType::Audio;
    Clip clip;
    clip.audioFile = "C:/audio/take 1.wav";

    soundsplice::engine::SpectralRegion region;
    region.startSeconds = 1.25;
    region.endSeconds   = 2.5;
    region.lowHz        = 3000.0;
    region.highHz       = 7500.5;
    region.gainDb       = -9.5f;
    clip.spectralEdits.push_back(region);
    region.gainDb = soundsplice::engine::SpectralRegion::kSilenceDb;
    clip.spectralEdits.push_back(region);

    clip.autoFadeOut = true; // made by an automatic crossfade
    track.clips.push_back(clip);
    song.tracks.push_back(track);

    Song back;
    REQUIRE(deserialize(serialize(song), back));
    REQUIRE(back.tracks.at(0).clips.at(0).spectralEdits == song.tracks[0].clips[0].spectralEdits);
    REQUIRE(back.tracks.at(0).clips.at(0).autoFadeOut);
    REQUIRE_FALSE(back.tracks.at(0).clips.at(0).autoFadeIn);
    REQUIRE(back.tracks.at(0).clips.at(0).audioFile == "C:/audio/take 1.wav");
}

TEST_CASE("Effect automation round-trips with its slot", "[model][io]")
{
    Song      song;
    const int id = addTrack(song, TrackType::Audio, "Vox").id;

    auto filter = makeEffectSlot(EffectKind::Filter);
    filter.automation["cutoff"].addPoint(0.0, 200.0f);
    filter.automation["cutoff"].addPoint(8.0, 8000.0f);
    filter.automation["resonance"].addPoint(4.0, 2.5f);
    filter.automation["mode"]; // an empty lane isn't written

    auto eq = makeEffectSlot(EffectKind::ParametricEq);
    eq.automation["band3Gain"].addPoint(1.5, -6.0f);
    findTrack(song, id)->effectChain = { makeEffectSlot(EffectKind::Delay), filter, eq };

    const auto text = serialize(song);
    REQUIRE(text.find("FXLANE mode") == std::string::npos);

    Song restored;
    REQUIRE(deserialize(text, restored));
    const auto& chain = restored.tracks[0].effectChain;
    REQUIRE(chain[0].automation.empty());
    REQUIRE(chain[1].lane("cutoff") != nullptr);
    REQUIRE(chain[1].lane("cutoff")->valueAt(4.0) == 4100.0f);
    REQUIRE(chain[1].lane("resonance")->points().size() == 1);
    REQUIRE(chain[1].lane("mode") == nullptr);
    REQUIRE(chain[2].lane("band3Gain")->valueAt(0.0) == -6.0f);

    // An empty lane never reaches the file, so compare without it.
    findTrack(song, id)->effectChain[1].automation.erase("mode");
    REQUIRE(restored.tracks[0].effectChain == findTrack(song, id)->effectChain);
}

TEST_CASE("Automation curve shapes round-trip, and an unknown one reads as linear", "[model][io]")
{
    Song      song;
    const int id = addTrack(song, TrackType::Audio, "Vox").id;
    auto&     track = *findTrack(song, id);
    track.laneFor(TrackParam::Gain).addPoint(0.0, -12.0f, CurveShape::SCurve);
    track.laneFor(TrackParam::Gain).addPoint(4.0, 0.0f);
    song.masterGainDb.addPoint(2.0, -3.0f, CurveShape::Hold);
    song.masterGainDb.addPoint(6.0, 0.0f);
    auto filter = makeEffectSlot(EffectKind::Filter);
    filter.automation["cutoff"].addPoint(0.0, 200.0f, CurveShape::FastStart);
    filter.automation["cutoff"].addPoint(4.0, 800.0f);
    track.effectChain = { filter };

    auto text = serialize(song);
    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);

    // A linear point's record is exactly what it was before shapes existed.
    REQUIRE(text.find("TAPT 4 0\n") != std::string::npos);

    const auto at = text.find("TAPT 0 -12 4");
    REQUIRE(at != std::string::npos);
    text.replace(at, 12, "TAPT 0 -12 99");
    Song unknown;
    REQUIRE(deserialize(text, unknown));
    REQUIRE(unknown.tracks[0].lane(TrackParam::Gain)->points()[0].shape == CurveShape::Linear);
}

TEST_CASE("A clip's own effects round-trip, and a clip without any writes none", "[model][io]")
{
    Song      song;
    const int id = addTrack(song, TrackType::Audio, "Vox").id;

    Clip clip;
    clip.audioFile = "takes/line one.wav";
    auto eq        = makeEffectSlot(EffectKind::ParametricEq);
    eq.parametricEq.band3GainDb = -4.5f;
    auto reverb    = makeEffectSlot(EffectKind::Convolution);
    reverb.convolution.irFile = "C:/Impulses/Small Room.wav";
    reverb.enabled            = false;
    clip.effects = { eq, reverb };
    addClip(song, id, clip);

    Clip plain;
    plain.audioFile = "takes/line two.wav";
    plain.startBeats = 16.0;
    addClip(song, id, plain);

    const auto text = serialize(song);
    REQUIRE(text.find("CLIPFX 2") != std::string::npos);
    REQUIRE(text.find("CLIPFX", text.find("line two.wav")) == std::string::npos);

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[0].clips[0].effects[1].convolution.irFile == "C:/Impulses/Small Room.wav");
    REQUIRE(restored.tracks[0].effectChain.empty()); // the track's own chain is untouched
}

TEST_CASE("A clip's takes round-trip, names and files with spaces included", "[model][io]")
{
    Song      song;
    const int id = addTrack(song, TrackType::Audio, "Vox").id;

    Clip clip;
    clip.audioFile = "takes/pass two.wav";
    clip.takes     = { { "takes/pass one.wav", -0.25, "Pass 1" }, { "takes/pass two.wav", 0.0, "Pass 2 (keeper)" } };
    clip.activeTake = 1;
    addClip(song, id, clip);

    Song restored;
    REQUIRE(deserialize(serialize(song), restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[0].clips[0].takes[0].audioFile == "takes/pass one.wav");
}

TEST_CASE("A track's own record input round-trips, and the default writes nothing", "[model][io]")
{
    Song song;
    addTrack(song, TrackType::Audio, "Kick");
    addTrack(song, TrackType::Audio, "Snare");
    song.tracks[1].recordInput    = 3;
    song.tracks[1].recordChannels = 1;

    const auto text = serialize(song);
    REQUIRE(text.find("TRACKINPUT 3 1") != std::string::npos);
    REQUIRE(text.find("TRACKINPUT") == text.rfind("TRACKINPUT")); // only the one with its own

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[0].recordInput == -1);
}

TEST_CASE("A track's own automation mode round-trips, and following the mix writes nothing", "[model][io]")
{
    Song song;
    addTrack(song, TrackType::Audio, "Vocal");
    addTrack(song, TrackType::Audio, "Bed");
    song.tracks[0].automationMode = 2; // Latch

    const auto text = serialize(song);
    REQUIRE(text.find("TRACKAUTOMODE 2") != std::string::npos);
    REQUIRE(text.find("TRACKAUTOMODE") == text.rfind("TRACKAUTOMODE"));

    Song restored;
    REQUIRE(deserialize(text, restored));
    REQUIRE(restored == song);
    REQUIRE(restored.tracks[1].automationMode == -1);
}
