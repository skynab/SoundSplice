#include "MainComponentInternal.h"

#include "engine/ClipChannels.h"
#include "engine/Loudness.h"

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

} // namespace soundsplice
