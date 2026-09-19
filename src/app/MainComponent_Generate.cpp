#include "MainComponentInternal.h"

#include "engine/Generators.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The Generate menu: tones, chirps, noise, silence and DTMF, rendered to a
// file and placed in the arrangement the way Paste places audio. The
// generators themselves are engine/Generators.h.

namespace soundsplice
{
namespace
{
    const char* generatorName(engine::GeneratorKind kind)
    {
        switch (kind)
        {
            case engine::GeneratorKind::Tone:    return "Tone";
            case engine::GeneratorKind::Chirp:   return "Chirp";
            case engine::GeneratorKind::Noise:   return "Noise";
            case engine::GeneratorKind::Silence: return "Silence";
            case engine::GeneratorKind::Dtmf:    return "DTMF Tones";
            case engine::GeneratorKind::Rhythm:  return "Rhythm Track";
            case engine::GeneratorKind::Pluck:   return "Pluck";
            case engine::GeneratorKind::RoomTone: return "Room Tone";
        }
        return "Audio";
    }

    const juce::StringArray kWaveforms { "Sine", "Square", "Sawtooth", "Triangle" };

    engine::Oscillator::Waveform waveformAt(int index)
    {
        switch (index)
        {
            case 1:  return engine::Oscillator::Waveform::Square;
            case 2:  return engine::Oscillator::Waveform::Saw;
            case 3:  return engine::Oscillator::Waveform::Triangle;
            default: return engine::Oscillator::Waveform::Sine;
        }
    }
}

/** The audio tracks new audio goes on: those in the time selection if it has
    any, else the selected track if it's an audio track, else none (a new
    track is made). */
std::vector<int> MainComponent::generateTargetTracks() const
{
    const auto&      song = history_.current();
    std::vector<int> ids;

    const auto isAudio = [&song](int id)
    {
        const auto* track = model::findTrack(song, id);
        return track != nullptr && track->type == model::TrackType::Audio;
    };

    if (timeSelection_.hasTracks())
    {
        for (int id : timeSelection_.trackIds)
            if (isAudio(id))
                ids.push_back(id);
        return ids;
    }

    if (selectedTrackIndex_ >= 0 && selectedTrackIndex_ < (int) song.tracks.size()
        && song.tracks[(size_t) selectedTrackIndex_].type == model::TrackType::Audio)
        ids.push_back(song.tracks[(size_t) selectedTrackIndex_].id);
    return ids;
}

void MainComponent::showGenerateDialog(engine::GeneratorKind kind)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    if (kind == engine::GeneratorKind::RoomTone && roomTone_ == nullptr)
    {
        showError("Capture some room tone first: select a quiet passage in the audio editor, then Generate > Capture Room Tone");
        return;
    }

    const juce::String prefix = juce::String("generate.") + generatorName(kind) + ".";
    const auto         value  = [this, &prefix](const char* field, double fallback)
    {
        return juce::String(settings_.getDoubleValue(prefix + field, fallback));
    };

    // A time selection sets the length, as it does in Audacity.
    const double bpm       = history_.current().bpm;
    const double selection = timeSelection_.isEmpty() || bpm <= 0.0 ? 0.0
                                                                     : timeSelection_.lengthBeats() * 60.0 / bpm;

    auto* window = new juce::AlertWindow(juce::String("Generate ") + generatorName(kind), {},
                                         juce::MessageBoxIconType::NoIcon, this);

    switch (kind)
    {
        case engine::GeneratorKind::Tone:
            window->addComboBox("waveform", kWaveforms, "Waveform:");
            window->getComboBoxComponent("waveform")->setSelectedItemIndex(
                settings_.getIntValue(prefix + "waveform", 0));
            window->addTextEditor("startHz", value("startHz", 440.0), "Frequency (Hz):");
            window->addTextEditor("startAmplitude", value("startAmplitude", 0.8), "Amplitude (0 to 1):");
            break;

        case engine::GeneratorKind::Chirp:
            window->addComboBox("waveform", kWaveforms, "Waveform:");
            window->getComboBoxComponent("waveform")->setSelectedItemIndex(
                settings_.getIntValue(prefix + "waveform", 0));
            window->addTextEditor("startHz", value("startHz", 440.0), "Start frequency (Hz):");
            window->addTextEditor("endHz", value("endHz", 1320.0), "End frequency (Hz):");
            window->addTextEditor("startAmplitude", value("startAmplitude", 0.8), "Start amplitude (0 to 1):");
            window->addTextEditor("endAmplitude", value("endAmplitude", 0.1), "End amplitude (0 to 1):");
            window->addComboBox("sweep", { "Linear", "Logarithmic" }, "Sweep:");
            window->getComboBoxComponent("sweep")->setSelectedItemIndex(settings_.getIntValue(prefix + "sweep", 0));
            break;

        case engine::GeneratorKind::Noise:
            window->addComboBox("colour", { "White", "Pink", "Brown" }, "Noise:");
            window->getComboBoxComponent("colour")->setSelectedItemIndex(settings_.getIntValue(prefix + "colour", 0));
            window->addTextEditor("startAmplitude", value("startAmplitude", 0.8), "Amplitude (0 to 1):");
            break;

        case engine::GeneratorKind::Dtmf:
            window->addTextEditor("dtmf", settings_.getValue(prefix + "dtmf", "0123456789"),
                                  "Keys (0-9, *, #, A-D):");
            window->addTextEditor("dtmfDuty", value("dtmfDuty", 55.0), "Tone share of each key (%):");
            window->addTextEditor("startAmplitude", value("startAmplitude", 0.8), "Amplitude (0 to 1):");
            break;

        case engine::GeneratorKind::Rhythm:
            window->addTextEditor("rhythmBpm", value("rhythmBpm", history_.current().bpm), "Tempo (bpm):");
            window->addTextEditor("beatsPerBar", juce::String(settings_.getIntValue(prefix + "beatsPerBar",
                                                                                    history_.current().timeSigNumerator)),
                                  "Beats per bar:");
            window->addTextEditor("bars", juce::String(settings_.getIntValue(prefix + "bars", 8)), "Bars:");
            window->addTextEditor("startAmplitude", value("startAmplitude", 0.8), "Amplitude (0 to 1):");
            break;

        case engine::GeneratorKind::Pluck:
            window->addTextEditor("startHz", value("startHz", 220.0), "Pitch (Hz):");
            window->addTextEditor("pluckDecay", value("pluckDecay", 0.5), "Decay (0 rings long, 1 dies fast):");
            window->addTextEditor("startAmplitude", value("startAmplitude", 0.8), "Amplitude (0 to 1):");
            break;

        case engine::GeneratorKind::RoomTone:
            window->setMessage("Noise with the captured room's colour and level, never repeating.");
            break;

        case engine::GeneratorKind::Silence:
            break;
    }

    const double defaultSeconds = kind == engine::GeneratorKind::Dtmf ? 1.0
                                : kind == engine::GeneratorKind::Pluck ? 2.0 : 30.0;
    if (kind == engine::GeneratorKind::Rhythm)
    {
        // A rhythm track's length is its bars, so it has no duration of its own.
        window->addButton("Generate", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    }
    else
    window->addTextEditor("seconds",
                          juce::String(selection > 0.0 ? selection : settings_.getDoubleValue(prefix + "seconds", defaultSeconds), 3),
                          "Duration (seconds):");

    if (kind != engine::GeneratorKind::Rhythm)
    {
        window->addButton("Generate", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    }

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, kind, prefix](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const auto number = [window](const char* name, double fallback)
            {
                const auto* editor = window->getTextEditor(name);
                return editor == nullptr ? fallback : editor->getText().trim().getDoubleValue();
            };
            const auto choice = [window](const char* name)
            {
                const auto* box = window->getComboBoxComponent(name);
                return box == nullptr ? 0 : juce::jmax(0, box->getSelectedItemIndex());
            };

            engine::GeneratorSpec spec;
            spec.kind           = kind;
            spec.waveform       = waveformAt(choice("waveform"));
            spec.startHz        = number("startHz", spec.startHz);
            spec.endHz          = number("endHz", spec.endHz);
            spec.startAmplitude = juce::jlimit(0.0, 1.0, number("startAmplitude", spec.startAmplitude));
            spec.endAmplitude   = juce::jlimit(0.0, 1.0, number("endAmplitude", spec.endAmplitude));
            spec.logarithmic    = choice("sweep") == 1;
            spec.noise          = (engine::NoiseColour) juce::jlimit(0, 2, choice("colour"));
            spec.dtmfDuty       = juce::jlimit(1.0, 100.0, number("dtmfDuty", 55.0)) / 100.0;
            spec.seconds        = number("seconds", 0.0);
            spec.rhythmBpm      = juce::jlimit(20.0, 400.0, number("rhythmBpm", 120.0));
            spec.beatsPerBar    = juce::jlimit(1, 32, (int) std::lround(number("beatsPerBar", 4.0)));
            spec.pluckDecay     = juce::jlimit(0.0, 1.0, number("pluckDecay", 0.5));
            spec.roomTone       = self->roomTone_;

            if (kind == engine::GeneratorKind::Rhythm)
            {
                const int bars = juce::jlimit(1, 999, (int) std::lround(number("bars", 8.0)));
                self->settings_.setValue(prefix + "beatsPerBar", spec.beatsPerBar);
                self->settings_.setValue(prefix + "bars", bars);
                self->settings_.setValue(prefix + "rhythmBpm", spec.rhythmBpm);
                spec.seconds = bars * spec.beatsPerBar * 60.0 / spec.rhythmBpm;
            }
            if (auto* keys = window->getTextEditor("dtmf"))
                spec.dtmf = keys->getText().toStdString();

            if (! (spec.seconds > 0.0) || spec.seconds > 24.0 * 3600.0)
            {
                self->showError("The duration needs to be more than 0 seconds");
                return;
            }
            if ((kind == engine::GeneratorKind::Tone || kind == engine::GeneratorKind::Chirp
                 || kind == engine::GeneratorKind::Pluck)
                && (spec.startHz <= 0.0 || (kind == engine::GeneratorKind::Chirp && spec.endHz <= 0.0)))
            {
                self->showError("Frequencies need to be above 0 Hz");
                return;
            }
            if (kind == engine::GeneratorKind::Dtmf && engine::Generator::dtmfKeys(spec.dtmf).empty())
            {
                self->showError("Type at least one key: 0-9, *, #, or A-D");
                return;
            }

            auto& settings = self->settings_;
            settings.setValue(prefix + "waveform", choice("waveform"));
            settings.setValue(prefix + "sweep", choice("sweep"));
            settings.setValue(prefix + "colour", choice("colour"));
            settings.setValue(prefix + "startHz", spec.startHz);
            settings.setValue(prefix + "endHz", spec.endHz);
            settings.setValue(prefix + "startAmplitude", spec.startAmplitude);
            settings.setValue(prefix + "endAmplitude", spec.endAmplitude);
            settings.setValue(prefix + "dtmf", juce::String(spec.dtmf));
            settings.setValue(prefix + "dtmfDuty", spec.dtmfDuty * 100.0);
            settings.setValue(prefix + "seconds", spec.seconds);
            settings.setValue(prefix + "pluckDecay", spec.pluckDecay);

            self->generateAudio(spec);
        }));
}

/** Renders @p spec to a mono 32-bit float WAV beside the project's other
    audio, on the render thread, then places it as Paste would: over the time
    selection on its audio tracks (replacing what was there), at the playhead
    on the selected audio track (pushing what follows later), or on a new
    track when no audio track is selected. One undo step. */
void MainComponent::generateAudio(const engine::GeneratorSpec& spec)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    const double rate    = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    const auto   name    = juce::String(generatorName(spec.kind));
    const auto   file    = audioDirectoryFor(recordingsDirectory()).getNonexistentChildFile(name.removeCharacters(" "), ".wav");
    auto         written = std::make_shared<bool>(false);

    auto work = [spec, rate, file, written, name](app::OfflineRenderJob& job)
    {
        file.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> output(file.createOutputStream());
        if (output == nullptr)
            return;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(output,
            juce::AudioFormatWriterOptions{}
                .withSampleRate(rate)
                .withNumChannels(1)
                .withBitsPerSample(32)
                .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr)
            return;

        engine::Generator  generator(spec, rate);
        const auto         total = generator.totalFrames();
        std::vector<float> block(1 << 16);

        for (std::int64_t done = 0; done < total;)
        {
            if (job.shouldAbort())
                return;

            const int n = (int) std::min<std::int64_t>((std::int64_t) block.size(), total - done);
            generator.render(block.data(), n);
            const float* channels[] { block.data() };
            if (! writer->writeFromFloatArrays(channels, 1, n))
                return;

            done += n;
            job.report((double) done / (double) total, "Generating " + name.toLowerCase());
        }

        writer.reset();
        *written = total > 0;
    };

    const auto targets   = generateTargetTracks();
    const auto selection = timeSelection_;

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), written, file, spec, targets,
                       selection, name](bool cancelled)
    {
        if (self == nullptr)
            return;

        self->renderJob_.reset();

        if (cancelled || ! *written)
        {
            file.deleteFile();
            if (cancelled)
                self->showStatus("Generate cancelled");
            else
                self->showError("Could not write " + file.getFileName());
            return;
        }

        if (targets.empty())
        {
            if (self->trackCount() >= self->engine_.maxTracks())
            {
                file.deleteFile();
                self->showError("Track limit reached");
                return;
            }
            self->importAudioFileAtBeat(file, self->playheadBeat());
            self->showStatus("Generated " + name.toLowerCase() + " on a new track");
            return;
        }

        const double bpm         = self->history_.current().bpm;
        const double lengthBeats = engine::beatsForSeconds(spec.seconds, bpm);

        model::Clip clip;
        clip.type        = model::ClipType::Audio;
        clip.lengthBeats = lengthBeats;
        clip.audioFile   = file.getFullPathName().toStdString();

        model::RangeClipboard clipboard;
        clipboard.lengthBeats = lengthBeats;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            clipboard.tracks.push_back({ clip });
            clipboard.trackTypes.push_back(model::TrackType::Audio);
        }

        auto replaced     = selection;
        replaced.trackIds = targets;
        if (! selection.hasTracks())
            replaced.startBeats = replaced.endBeats = self->playheadBeat();

        self->history_.edit("Generate " + name.toLowerCase().toStdString(), [replaced, targets, clipboard](model::Song& s)
        {
            model::rangeedit::removeRange(s, replaced, true);
            model::rangeedit::insertClipboard(s, targets, clipboard, replaced.startBeats);
        });

        auto generated     = replaced;
        generated.endBeats = replaced.startBeats + lengthBeats;
        self->setTimeSelection(generated);
        self->refreshAfterArrangementEdit();
        self->showStatus("Generated " + juce::String(spec.seconds, 2) + " s of " + name.toLowerCase());
    };

    renderJob_ = app::OfflineRenderJob::launch("Generate " + name, std::move(work), std::move(onFinished));
}

/** Room tone's source: the audio editor's selection, a passage where nothing
    but the room is heard, measured for Generate > Room Tone to synthesize. */
void MainComponent::captureRoomTone()
{
    const auto* clip = selectedAudioClip();
    ClipAudio   audio;
    int         from = 0, to = 0;
    if (clip == nullptr || audioEditor_.selection().isEmpty() || ! openSelectedClipAudio(audio)
        || ! selectedClipRange(audio, from, to, false))
    {
        showError("Select a quiet passage of the room in the audio editor first");
        return;
    }

    const auto channels = readClipAudio(audio, from, to);
    if (channels.empty())
    {
        showError("Could not read " + juce::File(clip->audioFile).getFileName());
        return;
    }

    std::vector<float> mono(channels[0].size(), 0.0f);
    for (const auto& channel : channels)
        for (size_t i = 0; i < mono.size() && i < channel.size(); ++i)
            mono[i] += channel[i] / (float) channels.size();

    // Measured as it plays, with the clip's gain.
    const float gain = juce::Decibels::decibelsToGain(clip->gainDb);
    for (auto& s : mono)
        s *= gain;

    auto profile = engine::RoomToneProfile::capture(mono);
    if (profile.isEmpty())
    {
        showError("That's too short to capture - select at least a tenth of a second of room");
        return;
    }

    roomTone_ = std::make_shared<const engine::RoomToneProfile>(std::move(profile));
    showStatus("Room tone captured from " + juce::String(audioEditor_.selection().lengthSeconds(), 2)
               + "s - Generate > Room Tone fills a time selection with it");
}

} // namespace soundsplice
