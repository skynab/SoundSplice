#include "MainComponentInternal.h"

#include "engine/AdaptiveNoiseReduction.h"
#include "engine/HqStretch.h"
#include "engine/PitchDetection.h"
#include "engine/CenterChannel.h"

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
    // A painted or lassoed shape: each bin by how far it's inside.
    if (const auto brush = audioEditor_.spectralBrush())
    {
        const double selectionStart = audioEditor_.selection().startSeconds;
        bool         tooShort       = false;
        const bool   edited = editSelection(label, false,
            [&](std::vector<std::vector<float>>& channels, double rate)
            {
                const auto maskAt = [&](double sample, double hz)
                { return brush->amountAt(selectionStart + sample / rate, hz); };
                for (auto& channel : channels)
                    if (! engine::spectral::scaleMask(channel, 0, (int) channel.size(), rate, gain, maskAt))
                        tooShort = true;
            });

        if (tooShort)
            showStatus("Part of that was too short to edit: paint over at least a few milliseconds");
        else if (edited)
            showStatus(label + " on what was " + (brush->isLasso() ? "lassoed" : "painted"));
        return;
    }

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

/** Spectral repair, as Audition's spot healing does it for a box: the box's
    frequencies over its time rebuilt from what they do either side of it. */
void MainComponent::repairSpectralSelection()
{
    if (const auto brush = audioEditor_.spectralBrush())
    {
        repairPaintedSpectrum(*brush);
        return;
    }

    const auto band = audioEditor_.frequencyBand();
    if (! band)
    {
        showError("Drag a box on the spectrogram over the sound to remove first (View > Spectrogram)");
        return;
    }

    // Half a second either side to learn from: long enough to average over,
    // short enough to still be the same sound.
    const int context = (int) std::lround((engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0) * 0.5);
    bool      refused = false;

    const bool edited = editSelectionInContext("Spectral repair", context,
        [band, context, &refused](std::vector<std::vector<float>>& channels, int from, int to, double rate)
        {
            for (auto& channel : channels)
                if (! engine::spectral::healBand(channel, from, to, rate, band->first, band->second, context))
                    refused = true;
            return ! refused;
        });

    if (refused)
        showError("Spectral repair needs the box to be at least a few milliseconds long, with audio either side of it");
    else if (edited)
        showStatus("Repaired " + juce::String((int) std::lround(band->first)) + " - "
                   + juce::String((int) std::lround(band->second)) + " Hz");
}

/** Runs @p edit, a spectral edit of one channel over the whole of what it's
    given, on the spectral selection: the box's time, shaped by its band. */
void MainComponent::applySpectralEdit(const juce::String& label,
                                      const std::function<bool(std::vector<float>&, double rate, double lowHz,
                                                               double highHz)>& edit)
{
    const auto band = audioEditor_.frequencyBand();
    if (! band)
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    bool tooShort = false;
    const bool edited = editSelection(label, false,
        [&](std::vector<std::vector<float>>& channels, double rate)
        {
            for (auto& channel : channels)
                if (! edit(channel, rate, band->first, band->second))
                    tooShort = true;
        });

    if (tooShort)
        showStatus("Part of that was too short to edit: select at least a few milliseconds");
    else if (edited)
        showStatus(label + " applied");
}

void MainComponent::showSpectralEqDialog()
{
    if (! audioEditor_.frequencyBand())
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    auto* window = new juce::AlertWindow("Spectral EQ",
                                         "A bell across the selected band, strongest at its middle, over the selected time.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("gain", juce::String(settings_.getDoubleValue("spectralEq.db", -9.0)), "Gain at the middle (dB):");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const auto db = (float) juce::jlimit(-60.0, 24.0, window->getTextEditorContents("gain").getDoubleValue());
            self->settings_.setValue("spectralEq.db", db);
            self->applySpectralEdit("Spectral EQ", [db](std::vector<float>& channel, double rate, double low, double high)
            {
                return engine::spectral::bellBand(channel, 0, (int) channel.size(), rate, low, high, db);
            });
        }));
}

void MainComponent::showSpectralShelfDialog()
{
    if (! audioEditor_.frequencyBand())
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    auto* window = new juce::AlertWindow("Spectral Shelf",
                                         "Ramps across the selected band and holds beyond it, over the selected time.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addComboBox("side", { "High shelf (above the band)", "Low shelf (below the band)" }, "Shelf:");
    window->getComboBoxComponent("side")->setSelectedItemIndex(settings_.getIntValue("spectralShelf.side", 0));
    window->addTextEditor("gain", juce::String(settings_.getDoubleValue("spectralShelf.db", -6.0)), "Gain (dB):");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int  side = juce::jlimit(0, 1, window->getComboBoxComponent("side")->getSelectedItemIndex());
            const auto db   = (float) juce::jlimit(-60.0, 24.0, window->getTextEditorContents("gain").getDoubleValue());
            self->settings_.setValue("spectralShelf.side", side);
            self->settings_.setValue("spectralShelf.db", db);
            self->applySpectralEdit("Spectral shelf", [db, side](std::vector<float>& channel, double rate, double low, double high)
            {
                return engine::spectral::shelfBand(channel, 0, (int) channel.size(), rate, low, high, db, side == 0);
            });
        }));
}

/** The healing brush: what's painted on the spectrogram is rebuilt from what
    its frequencies do either side of the painting, and nothing else is. */
void MainComponent::repairPaintedSpectrum(const spectrogramimage::Brush& brush)
{
    const double selectionStart = audioEditor_.selection().startSeconds;
    const int    context = (int) std::lround((engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0) * 0.5);
    bool         refused = false;

    showBusy("Healing...");

    const bool edited = editSelectionInContext("Spectral repair", context,
        [&](std::vector<std::vector<float>>& channels, int from, int to, double rate)
        {
            // A window's middle, as a sample of what was read, back to seconds
            // into the clip, where the brush was painted.
            const auto maskAt = [&](double sample, double hz)
            {
                return brush.amountAt(selectionStart + (sample - from) / rate, hz);
            };

            for (auto& channel : channels)
                if (! engine::spectral::healMask(channel, from, to, rate, context, maskAt))
                    refused = true;
            return ! refused;
        });

    if (refused)
        showError("The healing brush needs a stroke at least a few milliseconds long, with audio either side of it");
    else if (edited)
        showStatus("Healed what was painted");
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

/** Spectral edits kept on the clip (REAPER's): the box is stored, and what
    plays is the file with it applied, so it can be removed at any time. */
void MainComponent::showSpectralClipEditDialog()
{
    if (! audioEditor_.frequencyBand())
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    auto* window = new juce::AlertWindow("Add Clip Spectral Edit",
                                         "Turns the box up or down as the clip plays, leaving its file as it is. "
                                         "-96 dB or lower removes it.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("gain", juce::String(settings_.getDoubleValue("spectralClipEdit.db", -12.0)), "Gain (dB):");
    window->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double db = juce::jlimit(-120.0, 24.0, window->getTextEditorContents("gain").getDoubleValue());
            self->settings_.setValue("spectralClipEdit.db", db);
            self->addSpectralClipEdit((float) db);
        }));
}

void MainComponent::addSpectralClipEdit(float gainDb)
{
    const auto* clip = selectedAudioClip();
    const auto  band = audioEditor_.frequencyBand();
    if (clip == nullptr || ! band)
        return;

    engine::SpectralRegion region;
    region.startSeconds = clip->sourceOffsetSeconds + audioEditor_.selection().startSeconds;
    region.endSeconds   = clip->sourceOffsetSeconds + audioEditor_.selection().endSeconds;
    region.lowHz        = band->first;
    region.highHz       = band->second;
    region.gainDb       = gainDb;

    const int trackIndex = selectedTrackIndex_, clipIndex = selectedClipIndex_;
    history_.edit("Add clip spectral edit", [trackIndex, clipIndex, region](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < (int) clips.size())
            clips[(size_t) clipIndex].spectralEdits.push_back(region);
    });

    showBusy("Applying spectral edit...");
    syncEngineTracks(); // renders the clip's file with its edits, once
    arrangementView_.setSong(history_.current());
    refreshAudioEditorForSelected();
    showStatus("Spectral edit kept on the clip - Edit > Spectral > Remove Clip Spectral Edits takes it off");
}

/** Takes the clip's stored spectral edits off where the selection is, or all
    of them with none. */
void MainComponent::removeSpectralClipEdits()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr || clip->spectralEdits.empty())
        return;

    const auto   selection = audioEditor_.selection();
    const bool   all       = selection.isEmpty();
    const double from      = clip->sourceOffsetSeconds + selection.startSeconds;
    const double to        = clip->sourceOffsetSeconds + selection.endSeconds;

    int removed = 0;
    for (const auto& region : clip->spectralEdits)
        if (all || region.overlaps(from, to))
            ++removed;
    if (removed == 0)
    {
        showStatus("No clip spectral edits in the selection");
        return;
    }

    const int trackIndex = selectedTrackIndex_, clipIndex = selectedClipIndex_;
    history_.edit("Remove clip spectral edits", [trackIndex, clipIndex, all, from, to](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;
        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= (int) clips.size())
            return;
        auto& edits = clips[(size_t) clipIndex].spectralEdits;
        edits.erase(std::remove_if(edits.begin(), edits.end(),
                                   [&](const engine::SpectralRegion& region) { return all || region.overlaps(from, to); }),
                    edits.end());
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
    refreshAudioEditorForSelected();
    showStatus("Removed " + juce::String(removed) + (removed == 1 ? " clip spectral edit" : " clip spectral edits"));
}

/** Audacity's Vocal Reduction and Isolation, Audition's Center Channel
    Extractor: what's panned to the centre of a stereo clip taken out or kept
    alone, over the audio editor's selection or the whole clip. */
void MainComponent::showVocalReductionDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    auto* window = new juce::AlertWindow("Vocal Reduction and Isolation",
                                         "Works on what's panned to the centre of a stereo recording: usually the lead vocal.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addComboBox("mode", { "Remove the centre (vocals out)", "Isolate the centre (vocals only)" }, "Action:");
    window->getComboBoxComponent("mode")->setSelectedItemIndex(settings_.getIntValue("vocalReduction.mode", 0));
    window->addTextEditor("strength", juce::String(settings_.getDoubleValue("vocalReduction.strength", 100.0)), "Strength (%):");
    window->addTextEditor("low", juce::String(settings_.getDoubleValue("vocalReduction.lowHz", 120.0)), "From (Hz):");
    window->addTextEditor("high", juce::String(settings_.getDoubleValue("vocalReduction.highHz", 9000.0)), "To (Hz):");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            engine::centre::Settings settings;
            settings.mode     = window->getComboBoxComponent("mode")->getSelectedItemIndex() == 1
                                    ? engine::centre::Mode::Isolate : engine::centre::Mode::Remove;
            settings.strength = juce::jlimit(0.0, 100.0, window->getTextEditorContents("strength").getDoubleValue()) / 100.0;
            settings.lowHz    = juce::jlimit(0.0, 24000.0, window->getTextEditorContents("low").getDoubleValue());
            settings.highHz   = juce::jlimit(settings.lowHz + 1.0, 96000.0, window->getTextEditorContents("high").getDoubleValue());

            auto& stored = self->settings_;
            stored.setValue("vocalReduction.mode", (int) settings.mode);
            stored.setValue("vocalReduction.strength", settings.strength * 100.0);
            stored.setValue("vocalReduction.lowHz", settings.lowHz);
            stored.setValue("vocalReduction.highHz", settings.highHz);
            self->reduceVocals(settings);
        }));
}

void MainComponent::reduceVocals(const engine::centre::Settings& settings)
{
    const juce::String label = settings.mode == engine::centre::Mode::Isolate ? "Isolate vocals" : "Remove vocals";
    bool               mono  = false;
    const auto transform = [&settings, &mono](std::vector<std::vector<float>>& channels, double rate)
    {
        if (channels.size() < 2)
        {
            mono = true;
            return;
        }
        engine::centre::process(channels[0], channels[1], rate, settings);
    };

    showBusy(label + "...");
    const bool edited = audioEditor_.selection().isEmpty() ? editWholeClip(label, transform)
                                                           : editSelection(label, false, transform);
    if (mono)
        showError("That clip is mono: there's no centre to find without two channels");
    else if (edited)
        showStatus(label + (audioEditor_.selection().isEmpty() ? " across the clip" : " in the selection"));
}

/** Adaptive noise reduction: no noise print needed, the noise followed as it
    changes. Over the selection, or the whole clip. */
void MainComponent::showAdaptiveNoiseReductionDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    auto* window = new juce::AlertWindow("Adaptive Noise Reduction",
                                         "Finds the steady noise under the sound (hiss, hum, air) by itself and takes "
                                         "it down, following it if it changes. No noise print needed.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("reduction", juce::String(settings_.getDoubleValue("adaptiveNoise.reductionDb", 12.0)),
                          "Reduction (dB):");
    window->addTextEditor("floor", juce::String(settings_.getDoubleValue("adaptiveNoise.floorDb", -18.0)),
                          "Never lower than (dB):");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const float reduction = (float) juce::jlimit(0.0, 40.0, window->getTextEditorContents("reduction").getDoubleValue());
            const float floor     = (float) juce::jlimit(-60.0, 0.0, window->getTextEditorContents("floor").getDoubleValue());
            self->settings_.setValue("adaptiveNoise.reductionDb", reduction);
            self->settings_.setValue("adaptiveNoise.floorDb", floor);

            const auto transform = [reduction, floor](std::vector<std::vector<float>>& channels, double rate)
            {
                for (auto& channel : channels)
                    channel = engine::noisereduction::reduceNoiseAdaptive(channel, rate, reduction, floor);
            };
            self->showBusy("Reducing noise...");
            const bool whole  = self->audioEditor_.selection().isEmpty();
            const bool edited = whole ? self->editWholeClip("Adaptive noise reduction", transform)
                                      : self->editSelection("Adaptive noise reduction", false, transform);
            if (edited)
                self->showStatus(juce::String("Noise reduced ") + (whole ? "across the clip" : "in the selection"));
        }));
}

/** De-crackle: the dense crackle of vinyl or a bad cable, mended. Over the
    selection, or the whole clip. */
void MainComponent::showDecrackleDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    auto* window = new juce::AlertWindow("DeCrackle",
                                         "Mends crackle: many tiny clicks, each far shorter than a millisecond.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("amount", juce::String(settings_.getDoubleValue("decrackle.amount", 50.0)),
                          "Amount (0 gentle to 100 thorough):");
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double amount = juce::jlimit(0.0, 100.0, window->getTextEditorContents("amount").getDoubleValue());
            self->settings_.setValue("decrackle.amount", amount);

            int        mended    = 0;
            const auto transform = [amount, &mended](std::vector<std::vector<float>>& channels, double rate)
            {
                for (auto& channel : channels)
                    mended += engine::repair::decrackle(channel, 0, (int) channel.size(), rate, amount / 100.0);
            };
            self->showBusy("Mending crackle...");
            const bool whole  = self->audioEditor_.selection().isEmpty();
            const bool edited = whole ? self->editWholeClip("DeCrackle", transform)
                                      : self->editSelection("DeCrackle", false, transform);
            if (edited)
                self->showStatus(mended == 0 ? juce::String("No crackle found")
                                             : "Mended " + juce::String(mended) + (mended == 1 ? " crackle" : " crackles"));
        }));
}

/** Pitch correction, as REAPER's ReaTune does it: each moment's pitch pulled
    to the nearest note of a key's scale, as strongly and as quickly as
    asked, over the selection or the whole clip. The length is kept. */
void MainComponent::showPitchCorrectionDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    auto* window = new juce::AlertWindow("Pitch Correction",
                                         "Pulls a voice or instrument onto the notes of a key. A fast speed and full "
                                         "strength give the robotic effect; slower and gentler sounds natural.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addComboBox("key", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, "Key:");
    window->getComboBoxComponent("key")->setSelectedItemIndex(settings_.getIntValue("pitchCorrection.key", 0));
    window->addComboBox("scale", { "Chromatic (every note)", "Major", "Minor" }, "Scale:");
    window->getComboBoxComponent("scale")->setSelectedItemIndex(settings_.getIntValue("pitchCorrection.scale", 0));
    window->addTextEditor("strength", juce::String(settings_.getDoubleValue("pitchCorrection.strength", 100.0)),
                          "Strength (%):");
    window->addTextEditor("speed", juce::String(settings_.getDoubleValue("pitchCorrection.speedMs", 30.0)),
                          "Speed (ms, 0 snaps at once):");
    window->addComboBox("formants", { "Moves with the pitch", "Stays put (for voices)" }, "Voice character:");
    window->getComboBoxComponent("formants")->setSelectedItemIndex(settings_.getIntValue("speedPitch.keepFormants", 1));
    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const int    key          = juce::jlimit(0, 11, window->getComboBoxComponent("key")->getSelectedItemIndex());
            const auto   scale        = (engine::pitch::Scale) juce::jlimit(0, 2, window->getComboBoxComponent("scale")->getSelectedItemIndex());
            const double strength     = juce::jlimit(0.0, 100.0, window->getTextEditorContents("strength").getDoubleValue()) / 100.0;
            const double speedMs      = juce::jlimit(0.0, 1000.0, window->getTextEditorContents("speed").getDoubleValue());
            const bool   keepFormants = window->getComboBoxComponent("formants")->getSelectedItemIndex() == 1;

            auto& stored = self->settings_;
            stored.setValue("pitchCorrection.key", key);
            stored.setValue("pitchCorrection.scale", (int) scale);
            stored.setValue("pitchCorrection.strength", strength * 100.0);
            stored.setValue("pitchCorrection.speedMs", speedMs);
            stored.setValue("speedPitch.keepFormants", keepFormants ? 1 : 0);

            constexpr int kHop     = 256;
            bool          tooShort = false;
            const auto    transform = [&](std::vector<std::vector<float>>& channels, double rate)
            {
                // The pitch is heard in the channels together.
                std::vector<float> mono(channels[0].size(), 0.0f);
                for (const auto& channel : channels)
                    for (size_t i = 0; i < mono.size() && i < channel.size(); ++i)
                        mono[i] += channel[i] / (float) channels.size();

                const auto shift = engine::pitch::correction(engine::pitch::track(mono, rate, kHop), rate, kHop, key,
                                                             scale, strength, speedMs);
                auto corrected = engine::hqstretch::transposeCurve(channels, rate, [&shift](int sample)
                {
                    return shift.empty() ? 0.0 : shift[std::min(shift.size() - 1, (size_t) sample / kHop)];
                }, keepFormants);
                if (corrected.empty())
                    tooShort = true;
                else
                    channels = std::move(corrected);
            };

            self->showBusy("Correcting pitch...");
            const bool whole  = self->audioEditor_.selection().isEmpty();
            const bool edited = whole ? self->editWholeClip("Pitch correction", transform)
                                      : self->editSelection("Pitch correction", false, transform);
            if (tooShort)
                self->showError("That's too short to correct - select at least a quarter of a second");
            else if (edited)
                self->showStatus(juce::String("Pitch corrected ") + (whole ? "across the clip" : "in the selection"));
        }));
}

} // namespace soundsplice
