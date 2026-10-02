#include "MainComponentInternal.h"
#include "model/GraphicEq31Bands.h"

#include "engine/ClipChannels.h"
#include "engine/Loudness.h"
#include "app/LoudnessMatch.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Loudness: measuring a clip or selection (EBU R128) and normalizing a clip to
// a loudness target. The measurement itself is engine/Loudness.h.

namespace soundsplice
{
namespace
{
    struct LoudnessTarget
    {
        double      lufs;
        const char* name;
    };

    constexpr LoudnessTarget kLoudnessTargets[] {
        { -14.0, "-14 LUFS  (Spotify, YouTube, Tidal)" },
        { -16.0, "-16 LUFS  (Apple Music, podcasts)" },
        { -18.0, "-18 LUFS" },
        { -23.0, "-23 LUFS  (EBU R128 broadcast)" },
        { -24.0, "-24 LUFS  (ATSC A/85 broadcast)" },
    };

    constexpr double kTruePeakCeiling = -1.0;

    juce::String formatLevel(double value, const char* unit)
    {
        return std::isfinite(value) ? juce::String(value, 1) + " " + unit : juce::String("-inf ") + unit;
    }
}

/** Measures samples [from, to) of the selected clip, as scanClipAudio reads
    them. @p onMeasured runs on the message thread with the report, unless the
    job was cancelled or failed. */
void MainComponent::measureClipLoudness(const juce::String& title, const ClipAudio& audio, int from, int to,
                                        std::function<void(const engine::LoudnessReport&)> onMeasured)
{
    struct LoudnessScan final : ClipScan
    {
        engine::LoudnessMeter meter;

        void prepare(double sampleRate) override { meter.prepare(sampleRate, 2); }
        void process(const float* const* outputs, int frames) override { meter.process(outputs, 2, frames); }
    };

    auto       scan  = std::make_shared<LoudnessScan>();
    const auto count = juce::jmax(0, to - from);

    scanClipAudio(title, "Measuring loudness", audio, from, to, scan,
                  [scan, count, onMeasured = std::move(onMeasured)](double rate)
    {
        onMeasured(engine::LoudnessReport::of(scan->meter, (double) count / rate));
    });
}

/** The loudness of the audio editor's selection, or the whole clip, as heard:
    with the clip's gain. Shown in the Analyser pane. */
void MainComponent::measureLoudnessOfSelection()
{
    const auto* clip = selectedAudioClip();
    ClipAudio   audio;
    if (clip == nullptr || ! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError("Select an audio clip first");
        return;
    }

    int from = 0, to = audio.window.length();
    if (! audioEditor_.selection().isEmpty())
        selectedClipRange(audio, from, to, false);

    const double gainDb = clip->gainDb;
    const bool   whole  = audioEditor_.selection().isEmpty();

    measureClipLoudness("Measure Loudness", audio, from, to, [this, gainDb, whole](const engine::LoudnessReport& raw)
    {
        const auto report = raw.withGain(gainDb);
        analyserPane_.setLoudness(report);
        if (workspace_.isPanelOpen("Analyser"))
            workspace_.revealPanel("Analyser");

        if (! std::isfinite(report.integratedLufs))
            showStatus("Too short or too quiet for an integrated loudness - measure at least 400 ms of sound");
        else
            showStatus(juce::String(whole ? "Clip" : "Selection") + ": "
                       + formatLevel(report.integratedLufs, "LUFS") + ", true peak "
                       + formatLevel(report.truePeakDb, "dBTP"));
    });
}

void MainComponent::showNormalizeLoudnessDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    auto* window = new juce::AlertWindow("Normalize Loudness",
                                         "Sets the clip's gain so its integrated loudness (EBU R128) reaches the target. "
                                         "The audio itself isn't changed.",
                                         juce::MessageBoxIconType::NoIcon, this);

    juce::StringArray names;
    for (const auto& target : kLoudnessTargets)
        names.add(target.name);
    window->addComboBox("target", names, "Target:");

    const double remembered = settings_.getDoubleValue("loudnessTarget", -16.0);
    int          selected   = 1;
    for (int i = 0; i < (int) std::size(kLoudnessTargets); ++i)
        if (std::abs(kLoudnessTargets[i].lufs - remembered) < 0.01)
            selected = i;
    window->getComboBoxComponent("target")->setSelectedItemIndex(selected);

    auto limit = std::make_shared<juce::ToggleButton>("Keep true peak at or under -1 dBTP");
    limit->setToggleState(settings_.getBoolValue("loudnessLimitPeak", true), juce::dontSendNotification);
    limit->setSize(320, 24);
    window->addCustomComponent(limit.get());

    window->addButton("Normalize", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, limit](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int  index    = juce::jlimit(0, (int) std::size(kLoudnessTargets) - 1,
                                               window->getComboBoxComponent("target")->getSelectedItemIndex());
            const bool limiting = limit->getToggleState();

            self->settings_.setValue("loudnessTarget", kLoudnessTargets[index].lufs);
            self->settings_.setValue("loudnessLimitPeak", limiting);
            self->normalizeSelectedClipLoudness(kLoudnessTargets[index].lufs, limiting);
        }));
}

/** Match Loudness: asks for the target, as Normalize Loudness does, for
    every clip app::clipsToMatch picks. */
void MainComponent::showMatchLoudnessDialog()
{
    const auto clips = app::clipsToMatch(history_.current(), timeSelection_, selectedTrackIndex_);
    if (clips.empty())
    {
        showError("Select time over some audio clips, or a track with audio clips, first");
        return;
    }

    auto* window = new juce::AlertWindow("Match Loudness",
                                         "Sets each of the " + juce::String((int) clips.size())
                                             + " clips' gain so its integrated loudness (EBU R128) reaches the target. "
                                               "The audio itself isn't changed.",
                                         juce::MessageBoxIconType::NoIcon, this);

    juce::StringArray names;
    for (const auto& target : kLoudnessTargets)
        names.add(target.name);
    window->addComboBox("target", names, "Target:");

    const double remembered = settings_.getDoubleValue("loudnessTarget", -16.0);
    int          selected   = 1;
    for (int i = 0; i < (int) std::size(kLoudnessTargets); ++i)
        if (std::abs(kLoudnessTargets[i].lufs - remembered) < 0.01)
            selected = i;
    window->getComboBoxComponent("target")->setSelectedItemIndex(selected);

    auto limit = std::make_shared<juce::ToggleButton>("Keep true peaks at or under -1 dBTP");
    limit->setToggleState(settings_.getBoolValue("loudnessLimitPeak", true), juce::dontSendNotification);
    limit->setSize(320, 24);
    window->addCustomComponent(limit.get());

    window->addButton("Match", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window, limit](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int  index    = juce::jlimit(0, (int) std::size(kLoudnessTargets) - 1,
                                               window->getComboBoxComponent("target")->getSelectedItemIndex());
            const bool limiting = limit->getToggleState();
            self->settings_.setValue("loudnessTarget", kLoudnessTargets[index].lufs);
            self->settings_.setValue("loudnessLimitPeak", limiting);
            self->matchLoudness(app::clipsToMatch(self->history_.current(), self->timeSelection_, self->selectedTrackIndex_),
                                kLoudnessTargets[index].lufs, limiting);
        }));
}

/** Measures every clip to match, each as it plays without its gain, on a
    background job, then sets all their gains as one undo step. */
void MainComponent::matchLoudness(const std::vector<app::MatchedClip>& clips, double targetLufs, bool limitTruePeak)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    struct Item
    {
        app::MatchedClip            clip;
        juce::File                  file;
        std::int64_t                start = 0, count = 0;
        engine::ClipChannels        channels = engine::ClipChannels::Both;
        engine::LoudnessReport      report;
        bool                        measured = false;
    };
    auto items = std::make_shared<std::vector<Item>>();

    const auto& song = history_.current();
    for (const auto& match : clips)
    {
        const auto* track = model::findTrack(song, match.trackId);
        const auto  found = std::find_if(track->clips.begin(), track->clips.end(),
                                         [&](const model::Clip& c) { return c.id == match.clipId; });
        ClipAudio audio;
        if (found == track->clips.end() || ! openClipAudio(*found, audio) || audio.window.isEmpty())
            continue;
        items->push_back({ match, audio.file, audio.window.start, audio.window.length(), found->channels, {}, false });
    }
    if (items->empty())
    {
        showError("None of those clips could be read");
        return;
    }

    auto work = [items](app::OfflineRenderJob& job)
    {
        juce::AudioFormatManager formats;
        engine::sequencefile::registerFormats(formats);

        std::int64_t total = 0, done = 0;
        for (const auto& item : *items)
            total += item.count;

        for (auto& item : *items)
        {
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(item.file));
            if (reader == nullptr || reader->sampleRate <= 0.0)
                continue;

            engine::LoudnessMeter meter;
            meter.prepare(reader->sampleRate, 2);
            const int                fileChannels = juce::jmax(1, (int) reader->numChannels);
            constexpr int            kChunk       = 1 << 16;
            juce::AudioBuffer<float> buffer(fileChannels, kChunk);

            for (std::int64_t at = 0; at < item.count; at += kChunk)
            {
                if (job.shouldAbort())
                    return;
                const int n = (int) juce::jmin<std::int64_t>(kChunk, item.count - at);
                if (! reader->read(buffer.getArrayOfWritePointers(), fileChannels, item.start + at, n))
                    break;
                const float* outputs[2] {
                    buffer.getReadPointer(engine::sourceChannelFor(item.channels, 0, fileChannels)),
                    buffer.getReadPointer(engine::sourceChannelFor(item.channels, 1, fileChannels)),
                };
                meter.process(outputs, 2, n);
                done += n;
                job.report((double) done / (double) juce::jmax<std::int64_t>(1, total), "Measuring loudness");
            }

            item.report   = engine::LoudnessReport::of(meter, (double) item.count / reader->sampleRate);
            item.measured = true;
        }
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), items, targetLufs,
                       limitTruePeak](bool cancelled)
    {
        if (self == nullptr)
            return;
        self->renderJob_.reset();
        if (cancelled)
        {
            self->showStatus("Match Loudness cancelled");
            return;
        }

        struct Gain
        {
            app::MatchedClip clip;
            float            gainDb = 0.0f;
        };
        std::vector<Gain> gains;
        int quiet = 0, limited = 0;
        for (const auto& item : *items)
        {
            engine::LoudnessGain gain;
            if (! item.measured
                || ! engine::loudnessGainFor(item.report.integratedLufs, item.report.truePeakDb, targetLufs,
                                             kTruePeakCeiling, limitTruePeak, gain))
            {
                ++quiet;
                continue;
            }
            limited += gain.limited ? 1 : 0;
            gains.push_back({ item.clip, (float) juce::jlimit(-60.0, 60.0, gain.gainDb) });
        }

        if (gains.empty())
        {
            self->showError("Those clips are too short or too quiet to measure - each needs at least 400 ms of sound");
            return;
        }

        self->history_.edit("Match loudness", [gains](model::Song& s)
        {
            for (const auto& gain : gains)
                if (auto* track = model::findTrack(s, gain.clip.trackId))
                    for (auto& clip : track->clips)
                        if (clip.id == gain.clip.clipId)
                            clip.gainDb = gain.gainDb;
        });
        self->refreshAfterArrangementEdit();

        auto message = "Matched " + juce::String((int) gains.size()) + " clips to " + formatLevel(targetLufs, "LUFS");
        if (limited > 0)
            message += " - " + juce::String(limited) + " held back by the -1 dBTP ceiling";
        if (quiet > 0)
            message += " - " + juce::String(quiet) + " too quiet or short to measure, left as they were";
        self->showStatus(message);
    };

    renderJob_ = app::OfflineRenderJob::launch("Match Loudness", std::move(work), std::move(onFinished));
}

/** Loudness normalization, non-destructive like Normalize: measures the whole
    clip as it plays, without its gain, then sets the gain that brings it to
    @p targetLufs, held back if that would take the true peak over -1 dBTP. */
void MainComponent::normalizeSelectedClipLoudness(double targetLufs, bool limitTruePeak)
{
    const auto* clip = selectedAudioClip();
    ClipAudio   audio;
    if (clip == nullptr || ! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError("Select an audio clip first");
        return;
    }

    const int clipId = clip->id;

    measureClipLoudness("Normalize Loudness", audio, 0, audio.window.length(),
                        [this, clipId, targetLufs, limitTruePeak](const engine::LoudnessReport& report)
    {
        engine::LoudnessGain gain;
        if (! engine::loudnessGainFor(report.integratedLufs, report.truePeakDb, targetLufs, kTruePeakCeiling,
                                      limitTruePeak, gain))
        {
            showError("That clip is too short or too quiet to measure - it needs at least 400 ms of sound");
            return;
        }

        const auto where = app::OpenFiles::locate(history_.current(), clipId);
        if (! where.isValid())
        {
            showError("The clip was removed while it was being measured");
            return;
        }

        const auto gainDb = (float) juce::jlimit(-60.0, 60.0, gain.gainDb);
        history_.edit("Normalize loudness", [where, gainDb](model::Song& s)
        {
            s.tracks[(size_t) where.track].clips[(size_t) where.clip].gainDb = gainDb;
        });

        syncEngineTracks();
        refreshAudioEditorForSelected();
        refreshAutomationPaneForSelected();

        auto message = "Normalized to " + formatLevel(gain.reachesLufs, "LUFS") + " (gain "
                     + juce::String(gainDb > 0.0f ? "+" : "") + juce::String(gainDb, 1) + " dB)";
        if (gain.limited)
            message += " - held back by the -1 dBTP ceiling";
        showStatus(message);
    });
}

/** The average spectrum of the audio editor's selection, or the whole clip,
    read a chunk at a time. Nothing if there's no clip or it can't be read. */
std::optional<engine::SpectrumAverager> MainComponent::measureSpectrumOfSelection()
{
    ClipAudio audio;
    if (selectedAudioClip() == nullptr || ! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError("Select an audio clip first");
        return std::nullopt;
    }

    int from = 0, to = audio.window.length();
    if (! audioEditor_.selection().isEmpty())
        selectedClipRange(audio, from, to, false);

    engine::SpectrumAverager averager(audio.sequence.sampleRate);
    constexpr int            kChunk = 1 << 18;
    for (int at = from; at < to; at += kChunk)
    {
        const auto channels = readClipAudio(audio, at, std::min(to, at + kChunk));
        if (channels.empty())
            break;
        averager.append(channels);
    }

    if (averager.isEmpty())
    {
        showError("That's too short to measure - select at least a tenth of a second");
        return std::nullopt;
    }
    return averager;
}

void MainComponent::setMatchEqReference()
{
    showBusy("Measuring...");
    const auto spectrum = measureSpectrumOfSelection();
    if (! spectrum)
        return;

    matchEqReference_     = spectrum->bandLevelsDb();
    matchEqReferenceName_ = juce::File(selectedAudioClip()->audioFile).getFileNameWithoutExtension();
    showStatus("Match EQ reference set from " + matchEqReferenceName_
               + " - select another clip and choose Edit > Match EQ to Reference");
}

/** Adds a 31-band graphic EQ to the selected track that turns this clip's
    average tone into the reference's. An effect rather than an edit to the
    audio, so it can be bypassed, adjusted or removed like any other. */
void MainComponent::matchEqToReference()
{
    if (! matchEqReference_)
    {
        showError("Set a Match EQ reference first (Edit > Set as Match EQ Reference)");
        return;
    }

    showBusy("Matching...");
    const auto spectrum = measureSpectrumOfSelection();
    if (! spectrum)
        return;

    const auto gains = engine::matcheq::gains(*matchEqReference_, spectrum->bandLevelsDb(), spectrum->sampleRate());
    const int  index = selectedTrackIndex_;
    if (index < 0 || index >= trackCount())
        return;

    history_.edit("Match EQ", [index, gains](model::Song& s)
    {
        auto slot = model::makeEffectSlot(model::EffectKind::GraphicEq31);
        model::setGraphicEq31Gains(slot.graphicEq31, gains);
        s.tracks[(size_t) index].effectChain.push_back(std::move(slot));
    });

    syncEngineTracks();
    refreshEffectChainForSelected();
    refreshAutomationPaneForSelected();

    float largest = 0.0f;
    for (float gain : gains)
        largest = std::max(largest, std::abs(gain));
    showStatus("Added a 31-band EQ matching " + matchEqReferenceName_ + " (up to "
               + juce::String(largest, 1) + " dB) to the end of the track's effects");
}

} // namespace soundsplice
