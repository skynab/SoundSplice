#include "MainComponentInternal.h"

#include "engine/Repair.h"
#include "engine/SpectralEdit.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Restoration on the audio editor's selection: Repair, Click Removal, Clip Fix,
// Hum Removal, and spectral delete and gain on a box dragged on the spectrogram.
// The DSP is engine/Repair.h and engine/SpectralEdit.h.

namespace soundsplice
{
/** As editSelection, but @p transform also gets up to @p contextFrames of the
    clip's audio either side of the selection, for a model to learn from or a
    filter to settle over, and is told where the selection lies within what it
    was given. Only the selection is written back. */
bool MainComponent::editSelectionInContext(
    const juce::String& label, int contextFrames,
    const std::function<bool(std::vector<std::vector<float>>&, int from, int to, double sampleRate)>& transform)
{
    if (selectedAudioClip() == nullptr)
        return false;

    if (audioEditor_.selection().isEmpty())
    {
        showError("Select part of the clip first");
        return false;
    }

    ClipAudio audio;
    if (! openSelectedClipAudio(audio))
    {
        showError("Could not read that clip");
        return false;
    }

    int from = 0, to = 0;
    if (! selectedClipRange(audio, from, to, false) || to <= from)
    {
        showError("Select part of the clip first");
        return false;
    }

    const int readFrom = juce::jmax(0, from - contextFrames);
    const int readTo   = juce::jmin(audio.window.length(), to + contextFrames);

    auto channels = readClipAudio(audio, readFrom, readTo);
    if (channels.empty())
    {
        showError("Could not read " + audio.file.getFileName());
        return false;
    }

    if (! transform(channels, from - readFrom, to - readFrom, audio.sequence.sampleRate))
        return false;

    for (auto& channel : channels)
    {
        channel.erase(channel.begin() + (to - readFrom), channel.end());
        channel.erase(channel.begin(), channel.begin() + (from - readFrom));
    }

    return replaceClipAudio(label, audio, from, to, channels);
}

/** Repair, as Audacity's: the selection redrawn from what the audio either
    side of it predicts. For a dropout or a click too long for Click Removal,
    so it's kept to short selections, where a prediction can hold. */
void MainComponent::repairAudioSelection()
{
    constexpr double kLongestSeconds = 0.5;

    const bool repaired = editSelectionInContext("Repair", 4096,
        [this](std::vector<std::vector<float>>& channels, int from, int to, double rate)
        {
            if ((double) (to - from) / rate > kLongestSeconds)
            {
                showError("Repair works on short stretches - select half a second or less");
                return false;
            }

            for (auto& channel : channels)
                if (! engine::repair::interpolate(channel, from, to, 4096, 32))
                {
                    showError("Repair needs some audio either side of the selection to work from");
                    return false;
                }
            return true;
        });

    if (repaired)
        showStatus("Repaired");
}

void MainComponent::showClickRemovalDialog()
{
    auto* window = new juce::AlertWindow("Click Removal",
                                         "Finds clicks and pops in the selection and fills each from the audio around it.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addComboBox("sensitivity", { "Gentle (only obvious clicks)", "Normal", "Strong (quieter clicks too)" },
                        "Sensitivity:");
    window->getComboBoxComponent("sensitivity")->setSelectedItemIndex(settings_.getIntValue("clickRemoval.sensitivity", 1));
    window->addTextEditor("width", juce::String(settings_.getDoubleValue("clickRemoval.widthMs", 2.0)),
                          "Longest click (ms):");
    window->addButton("Remove Clicks", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int    sensitivity = juce::jlimit(0, 2, window->getComboBoxComponent("sensitivity")->getSelectedItemIndex());
            const double widthMs     = juce::jlimit(0.1, 20.0, window->getTextEditorContents("width").getDoubleValue());
            self->settings_.setValue("clickRemoval.sensitivity", sensitivity);
            self->settings_.setValue("clickRemoval.widthMs", widthMs);

            static constexpr double kThresholds[] { 12.0, 8.0, 5.0 };
            self->removeClicksInSelection(kThresholds[sensitivity], widthMs);
        }));
}

void MainComponent::removeClicksInSelection(double sensitivity, double maxWidthMs)
{
    int found = 0;
    showBusy("Removing clicks...");

    const bool edited = editSelectionInContext("Click removal", 2048,
        [this, sensitivity, maxWidthMs, &found](std::vector<std::vector<float>>& channels, int from, int to, double rate)
        {
            const int width = juce::jmax(1, (int) std::lround(maxWidthMs * 0.001 * rate));
            for (auto& channel : channels)
                found += engine::repair::removeClicks(channel, from, to, sensitivity, width);

            if (found == 0)
            {
                showStatus("No clicks found");
                return false;
            }
            return true;
        });

    if (edited)
        showStatus("Removed " + juce::String(found) + (found == 1 ? " click" : " clicks"));
}

void MainComponent::showClipFixDialog()
{
    auto* window = new juce::AlertWindow("Clip Fix",
                                         "Redraws clipped peaks in the selection, carrying the waveform on past the level it was cut to. "
                                         "The rebuilt peaks can go over full scale, so the selection can be turned down as well.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("threshold", juce::String(settings_.getDoubleValue("clipFix.thresholdPercent", 95.0)),
                          "Clipped at or above (% of the peak):");
    window->addTextEditor("reduce", juce::String(settings_.getDoubleValue("clipFix.reduceDb", 0.0)),
                          "Then turn down by (dB):");
    window->addButton("Fix", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double threshold = juce::jlimit(50.0, 100.0, window->getTextEditorContents("threshold").getDoubleValue());
            const double reduce    = juce::jlimit(0.0, 24.0, window->getTextEditorContents("reduce").getDoubleValue());
            self->settings_.setValue("clipFix.thresholdPercent", threshold);
            self->settings_.setValue("clipFix.reduceDb", reduce);
            self->fixClippingInSelection(threshold, reduce);
        }));
}

void MainComponent::fixClippingInSelection(double thresholdPercent, double reduceDb)
{
    int fixed = 0;

    const bool edited = editSelectionInContext("Clip fix", 4,
        [this, thresholdPercent, reduceDb, &fixed](std::vector<std::vector<float>>& channels, int from, int to, double)
        {
            for (auto& channel : channels)
            {
                // The level is a share of this channel's own peak in the
                // selection: a recording clipped in the converter may have
                // been turned down since.
                float peak = 0.0f;
                for (int i = from; i < to; ++i)
                    peak = std::max(peak, std::abs(channel[(size_t) i]));

                fixed += engine::repair::fixClipping(channel, from, to, peak * (float) (thresholdPercent / 100.0));
            }

            if (fixed == 0)
            {
                showStatus("No clipping found");
                return false;
            }

            const float gain = juce::Decibels::decibelsToGain((float) -reduceDb);
            if (gain != 1.0f)
                for (auto& channel : channels)
                    for (int i = from; i < to; ++i)
                        channel[(size_t) i] *= gain;
            return true;
        });

    if (edited)
        showStatus("Rebuilt " + juce::String(fixed) + (fixed == 1 ? " clipped peak" : " clipped peaks"));
}

void MainComponent::showHumRemovalDialog()
{
    auto* window = new juce::AlertWindow("Hum Removal",
                                         "Notches out mains hum and its harmonics from the selection.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addComboBox("mains", { "50 Hz (Europe, Asia, Africa, Australia)", "60 Hz (the Americas)" }, "Mains:");
    window->getComboBoxComponent("mains")->setSelectedItemIndex(settings_.getIntValue("humRemoval.mains", 0));
    window->addTextEditor("harmonics", juce::String(settings_.getIntValue("humRemoval.harmonics", 8)),
                          "Harmonics to remove (including the fundamental):");
    window->addComboBox("width", { "Narrow (least of the music)", "Medium", "Wide (hum that wanders)" }, "Notch width:");
    window->getComboBoxComponent("width")->setSelectedItemIndex(settings_.getIntValue("humRemoval.width", 1));
    window->addButton("Remove Hum", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int mains     = juce::jlimit(0, 1, window->getComboBoxComponent("mains")->getSelectedItemIndex());
            const int harmonics = juce::jlimit(1, 40, window->getTextEditorContents("harmonics").getIntValue());
            const int width     = juce::jlimit(0, 2, window->getComboBoxComponent("width")->getSelectedItemIndex());
            self->settings_.setValue("humRemoval.mains", mains);
            self->settings_.setValue("humRemoval.harmonics", harmonics);
            self->settings_.setValue("humRemoval.width", width);

            static constexpr double kQs[] { 60.0, 30.0, 10.0 };
            self->removeHumInSelection(mains == 0 ? 50.0 : 60.0, harmonics, kQs[width]);
        }));
}

void MainComponent::removeHumInSelection(double fundamentalHz, int harmonics, double q)
{
    showBusy("Removing hum...");

    // A second of what comes before, run through the notches first, so they
    // have settled by the start of the selection rather than ringing into it.
    const int context = (int) std::lround(engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0);

    const bool edited = editSelectionInContext("Hum removal", context,
        [fundamentalHz, harmonics, q](std::vector<std::vector<float>>& channels, int, int, double rate)
        {
            for (auto& channel : channels)
                engine::repair::removeHum(channel, rate, fundamentalHz, harmonics, q);
            return true;
        });

    if (edited)
        showStatus("Removed " + juce::String((int) fundamentalHz) + " Hz hum");
}

/** Scales the spectral selection's band by @p gainDb (-inf deletes it), as
    Audacity's Spectral Delete and spectral edits do: the selection's time,
    only its frequencies. */
void MainComponent::scaleSpectralSelection(const juce::String& label, float gain)
{
    const auto band = audioEditor_.frequencyBand();
    if (! band)
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    bool tooShort = false;
    const bool edited = editSelection(label, false,
        [band, gain, &tooShort](std::vector<std::vector<float>>& channels, double rate)
        {
            for (auto& channel : channels)
                if (! engine::spectral::scaleBand(channel, 0, (int) channel.size(), rate, band->first, band->second, gain))
                    tooShort = true;
        });

    if (tooShort)
        showStatus("Part of that was too short to edit: select at least a few milliseconds");
    else if (edited)
        showStatus(label + ": " + juce::String((int) std::lround(band->first)) + " - "
                   + juce::String((int) std::lround(band->second)) + " Hz");
}

void MainComponent::showSpectralGainDialog()
{
    if (! audioEditor_.frequencyBand())
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    auto* window = new juce::AlertWindow("Spectral Gain", "Turns the selected band up or down over the selected time.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("gain", juce::String(settings_.getDoubleValue("spectralGain.db", -12.0)), "Gain (dB):");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double db = juce::jlimit(-96.0, 24.0, window->getTextEditorContents("gain").getDoubleValue());
            self->settings_.setValue("spectralGain.db", db);
            self->scaleSpectralSelection("Spectral gain", juce::Decibels::decibelsToGain((float) db, -96.0f));
        }));
}

} // namespace soundsplice
