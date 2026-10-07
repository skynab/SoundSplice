#include "engine/SpeechEnhance.h"
#include "MainComponentInternal.h"

#include "engine/AdaptiveNoiseReduction.h"
#include "engine/Dereverb.h"
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
    if (previewing_)
        to = juce::jmin(to, from + (int) (kPreviewSeconds * audio.sequence.sampleRate));

    const int readFrom = juce::jmax(0, from - contextFrames);
    const int readTo   = juce::jmin(audio.window.length(), to + contextFrames);

    auto channels = readClipAudio(audio, readFrom, readTo);
    if (channels.empty())
    {
        showError("Could not read " + audio.file.getFileName());
        return false;
    }

    // The selection as it was, for a preview's Original and Difference.
    std::vector<std::vector<float>> original;
    if (previewing_)
        for (const auto& channel : channels)
            original.emplace_back(channel.begin() + (from - readFrom), channel.begin() + (to - readFrom));

    if (! transform(channels, from - readFrom, to - readFrom, audio.sequence.sampleRate))
        return false;

    for (auto& channel : channels)
    {
        channel.erase(channel.begin() + (to - readFrom), channel.end());
        channel.erase(channel.begin(), channel.begin() + (from - readFrom));
    }

    if (previewing_)
        return capturePreview(original, channels, audio.sequence.sampleRate);

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
    previewedDialog("Click Removal", "Finds clicks and pops in the selection and fills each from the audio around it.")
        .choice("clickRemoval.sensitivity", "Sensitivity:",
                { "Gentle (only obvious clicks)", "Normal", "Strong (quieter clicks too)" }, 1)
        .number("clickRemoval.widthMs", "Longest click (ms):", 2.0, 0.1, 20.0)
        .show("Remove Clicks", [this](const FormDialog::Values& v)
        {
            static constexpr double kThresholds[] { 12.0, 8.0, 5.0 };
            removeClicksInSelection(kThresholds[v.choice("clickRemoval.sensitivity")], v.number("clickRemoval.widthMs"));
        });
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
    previewedDialog("Clip Fix",
                    "Redraws clipped peaks in the selection, carrying the waveform on past the level it was cut to. "
                    "The rebuilt peaks can go over full scale, so the selection can be turned down as well.")
        .number("clipFix.thresholdPercent", "Clipped at or above (% of the peak):", 95.0, 50.0, 100.0)
        .number("clipFix.reduceDb", "Then turn down by (dB):", 0.0, 0.0, 24.0)
        .show("Fix", [this](const FormDialog::Values& v)
        {
            fixClippingInSelection(v.number("clipFix.thresholdPercent"), v.number("clipFix.reduceDb"));
        });
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
    previewedDialog("Hum Removal", "Notches out mains hum and its harmonics from the selection.")
        .choice("humRemoval.mains", "Mains:", { "50 Hz (Europe, Asia, Africa, Australia)", "60 Hz (the Americas)" }, 0)
        .integer("humRemoval.harmonics", "Harmonics to remove (including the fundamental):", 8, 1, 40)
        .choice("humRemoval.width", "Notch width:", { "Narrow (least of the music)", "Medium", "Wide (hum that wanders)" }, 1)
        .show("Remove Hum", [this](const FormDialog::Values& v)
        {
            static constexpr double kQs[] { 60.0, 30.0, 10.0 };
            removeHumInSelection(v.choice("humRemoval.mains") == 0 ? 50.0 : 60.0, v.integer("humRemoval.harmonics"),
                                 kQs[v.choice("humRemoval.width")]);
        });
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

    dialog("Spectral EQ", "A bell across the selected band, strongest at its middle, over the selected time.")
        .number("spectralEq.db", "Gain at the middle (dB):", -9.0, -60.0, 24.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            const auto db = (float) v.number("spectralEq.db");
            applySpectralEdit("Spectral EQ", [db](std::vector<float>& channel, double rate, double low, double high)
            {
                return engine::spectral::bellBand(channel, 0, (int) channel.size(), rate, low, high, db);
            });
        });
}

void MainComponent::showSpectralShelfDialog()
{
    if (! audioEditor_.frequencyBand())
    {
        showError("Drag a box on the spectrogram first (View > Spectrogram)");
        return;
    }

    dialog("Spectral Shelf", "Ramps across the selected band and holds beyond it, over the selected time.")
        .choice("spectralShelf.side", "Shelf:", { "High shelf (above the band)", "Low shelf (below the band)" }, 0)
        .number("spectralShelf.db", "Gain (dB):", -6.0, -60.0, 24.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            const bool high = v.choice("spectralShelf.side") == 0;
            const auto db   = (float) v.number("spectralShelf.db");
            applySpectralEdit("Spectral shelf", [db, high](std::vector<float>& channel, double rate, double low, double hi)
            {
                return engine::spectral::shelfBand(channel, 0, (int) channel.size(), rate, low, hi, db, high);
            });
        });
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

    dialog("Spectral Gain", "Turns the selected band up or down over the selected time.")
        .number("spectralGain.db", "Gain (dB):", -12.0, -96.0, 24.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            scaleSpectralSelection("Spectral gain", juce::Decibels::decibelsToGain((float) v.number("spectralGain.db"), -96.0f));
        });
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

    dialog("Add Clip Spectral Edit",
           "Turns the box up or down as the clip plays, leaving its file as it is. -96 dB or lower removes it.")
        .number("spectralClipEdit.db", "Gain (dB):", -12.0, -120.0, 24.0)
        .show("Add", [this](const FormDialog::Values& v) { addSpectralClipEdit((float) v.number("spectralClipEdit.db")); });
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

    previewedDialog("Vocal Reduction and Isolation",
                    "Works on what's panned to the centre of a stereo recording: usually the lead vocal.")
        .choice("vocalReduction.mode", "Action:", { "Remove the centre (vocals out)", "Isolate the centre (vocals only)" }, 0)
        .number("vocalReduction.strength", "Strength (%):", 100.0, 0.0, 100.0)
        .number("vocalReduction.lowHz", "From (Hz):", 120.0, 0.0, 24000.0)
        .number("vocalReduction.highHz", "To (Hz):", 9000.0, 1.0, 96000.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            engine::centre::Settings settings;
            settings.mode     = (engine::centre::Mode) v.choice("vocalReduction.mode");
            settings.strength = v.number("vocalReduction.strength") / 100.0;
            settings.lowHz    = v.number("vocalReduction.lowHz");
            settings.highHz   = juce::jmax(settings.lowHz + 1.0, v.number("vocalReduction.highHz"));
            reduceVocals(settings);
        });
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

    previewedDialog("Adaptive Noise Reduction",
                    "Finds the steady noise under the sound (hiss, hum, air) by itself and takes "
                    "it down, following it if it changes. No noise print needed.")
        .number("adaptiveNoise.reductionDb", "Reduction (dB):", 12.0, 0.0, 40.0)
        .number("adaptiveNoise.floorDb", "Never lower than (dB):", -18.0, -60.0, 0.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            const auto reduction = (float) v.number("adaptiveNoise.reductionDb");
            const auto floor     = (float) v.number("adaptiveNoise.floorDb");
            const auto transform = [reduction, floor](std::vector<std::vector<float>>& channels, double rate)
            {
                for (auto& channel : channels)
                    channel = engine::noisereduction::reduceNoiseAdaptive(channel, rate, reduction, floor);
            };
            showBusy("Reducing noise...");
            const bool whole  = audioEditor_.selection().isEmpty();
            const bool edited = whole ? editWholeClip("Adaptive noise reduction", transform)
                                      : editSelection("Adaptive noise reduction", false, transform);
            if (edited)
                showStatus(juce::String("Noise reduced ") + (whole ? "across the clip" : "in the selection"));
        });
}

/** Speech Enhancement (AI): RNNoise over the selection or the whole clip,
    blended back with the original by an amount. */
void MainComponent::showSpeechEnhancementDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    previewedDialog("Speech Enhancement (AI)",
                    "A trained network (RNNoise) keeps the voice and takes away everything else: fans, "
                    "traffic, keyboards, noise that changes. For speech, not music.")
        .number("speechEnhance.amount", "Amount (%; less leaves some of the original in):", 100.0, 0.0, 100.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            const double amount    = v.number("speechEnhance.amount");
            const auto   transform = [amount](std::vector<std::vector<float>>& channels, double rate)
            {
                for (auto& channel : channels)
                    channel = engine::speechenhance::enhance(channel, rate, (float) (amount / 100.0));
            };
            showBusy("Enhancing speech...");
            const bool whole  = audioEditor_.selection().isEmpty();
            const bool edited = whole ? editWholeClip("Speech enhancement", transform)
                                      : editSelection("Speech enhancement", false, transform);
            if (edited)
                showStatus(juce::String("Speech enhanced ") + (whole ? "across the clip" : "in the selection"));
        });
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

    previewedDialog("DeCrackle", "Mends crackle: many tiny clicks, each far shorter than a millisecond.")
        .number("decrackle.amount", "Amount (0 gentle to 100 thorough):", 50.0, 0.0, 100.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            const double amount    = v.number("decrackle.amount");
            int          mended    = 0;
            const auto   transform = [amount, &mended](std::vector<std::vector<float>>& channels, double rate)
            {
                for (auto& channel : channels)
                    mended += engine::repair::decrackle(channel, 0, (int) channel.size(), rate, amount / 100.0);
            };
            showBusy("Mending crackle...");
            const bool whole  = audioEditor_.selection().isEmpty();
            const bool edited = whole ? editWholeClip("DeCrackle", transform) : editSelection("DeCrackle", false, transform);
            if (edited)
                showStatus(mended == 0 ? juce::String("No crackle found")
                                       : "Mended " + juce::String(mended) + (mended == 1 ? " crackle" : " crackles"));
        });
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

    previewedDialog("Pitch Correction",
                    "Pulls a voice or instrument onto the notes of a key. A fast speed and full "
                    "strength give the robotic effect; slower and gentler sounds natural.")
        .choice("pitchCorrection.key", "Key:", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0)
        .choice("pitchCorrection.scale", "Scale:", { "Chromatic (every note)", "Major", "Minor" }, 0)
        .number("pitchCorrection.strength", "Strength (%):", 100.0, 0.0, 100.0)
        .number("pitchCorrection.speedMs", "Speed (ms, 0 snaps at once):", 30.0, 0.0, 1000.0)
        .choice("speedPitch.keepFormants", "Voice character:", { "Moves with the pitch", "Stays put (for voices)" }, 1)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            const int    key          = v.choice("pitchCorrection.key");
            const auto   scale        = (engine::pitch::Scale) v.choice("pitchCorrection.scale");
            const double strength     = v.number("pitchCorrection.strength") / 100.0;
            const double speedMs      = v.number("pitchCorrection.speedMs");
            const bool   keepFormants = v.choice("speedPitch.keepFormants") == 1;

            // Static, so the inner lambda can name it without capturing it:
            // GCC 12 wants a plain local constexpr captured there.
            static constexpr int kHop = 256;
            bool          tooShort  = false;
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

            showBusy("Correcting pitch...");
            const bool whole  = audioEditor_.selection().isEmpty();
            const bool edited = whole ? editWholeClip("Pitch correction", transform)
                                      : editSelection("Pitch correction", false, transform);
            if (tooShort)
                showError("That's too short to correct - select at least a quarter of a second");
            else if (edited)
                showStatus(juce::String("Pitch corrected ") + (whole ? "across the clip" : "in the selection"));
        });
}

/** De-reverb: the room's tail taken out, over the selection or the whole
    clip. The reverb time is the room's (how long a clap takes to die away);
    too short leaves tail, too long takes some of the voice. */
void MainComponent::showDereverbDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    previewedDialog("DeReverb",
                    "Takes a room's echo out of a recording made in it: set the reverb time to "
                    "about how long a clap takes to die away there.")
        .number("dereverb.seconds", "Reverb time (s):", 0.8, 0.1, 10.0)
        .number("dereverb.amount", "Amount (%):", 67.0, 0.0, 100.0)
        .number("dereverb.floorDb", "Never lower than (dB):", -18.0, -60.0, 0.0)
        .show("Apply", [this](const FormDialog::Values& v)
        {
            engine::dereverb::Settings settings;
            settings.reverbSeconds = v.number("dereverb.seconds");
            settings.amount        = v.number("dereverb.amount") / 100.0 * 3.0; // 67% is the engine's 2
            settings.floorDb       = v.number("dereverb.floorDb");

            const auto transform = [settings](std::vector<std::vector<float>>& channels, double rate)
            {
                for (auto& channel : channels)
                    channel = engine::dereverb::process(channel, rate, settings);
            };
            showBusy("Removing reverb...");
            const bool whole  = audioEditor_.selection().isEmpty();
            const bool edited = whole ? editWholeClip("DeReverb", transform) : editSelection("DeReverb", false, transform);
            if (edited)
                showStatus(juce::String("Reverb reduced ") + (whole ? "across the clip" : "in the selection"));
        });
}

// ---- Preview before apply ------------------------------------------------------

FormDialog MainComponent::dialog(const juce::String& title, const juce::String& message)
{
    return FormDialog(*this, settings_, title, message);
}

FormDialog MainComponent::previewedDialog(const juce::String& title, const juce::String& message)
{
    auto form = dialog(title, message);
    form.withPreview([this](juce::AlertWindow& window, std::function<void()> run) { addPreviewStrip(&window, std::move(run)); },
                     [this] { endPreview(); });
    return form;
}

/** A Preview strip in @p window, running @p run - what the dialog's Apply
    does - in preview mode. */
void MainComponent::addPreviewStrip(juce::AlertWindow* window, std::function<void()> run)
{
    previewStrip_            = std::make_unique<PreviewStrip>();
    previewStrip_->onPreview = [this, run](preview::Mode mode) { previewEffect(run, mode); };
    previewStrip_->onSwitch  = [this](preview::Mode mode) { playPreview(mode); };
    previewStrip_->onStop    = [this] { engine_.stopAudition(); };
    window->addCustomComponent(previewStrip_.get());
}

/** Runs @p run with the edit functions capturing rather than committing,
    then plays @p mode of what it made. */
void MainComponent::previewEffect(const std::function<void()>& run, preview::Mode mode)
{
    engine_.stopAudition();
    previewOriginal_.clear();
    previewProcessed_.clear();
    previewing_ = true;
    run();
    previewing_ = false;

    if (previewProcessed_.empty())
    {
        showStatus("Nothing to preview: select part of an audio clip, or this effect doesn't change the audio itself");
        return;
    }
    if (previewStrip_ != nullptr)
        previewStrip_->setDifferenceAvailable(! preview::difference(previewOriginal_, previewProcessed_).empty());
    playPreview(mode);
}

bool MainComponent::capturePreview(const std::vector<std::vector<float>>& original,
                                   const std::vector<std::vector<float>>& processed, double sampleRate)
{
    previewOriginal_  = original;
    previewProcessed_ = processed;
    previewRate_      = sampleRate;
    return false; // nothing committed
}

void MainComponent::playPreview(preview::Mode mode)
{
    const auto difference = mode == preview::Difference ? preview::difference(previewOriginal_, previewProcessed_)
                                                        : std::vector<std::vector<float>> {};
    const auto& chosen = mode == preview::Original ? previewOriginal_ : mode == preview::Difference ? difference : previewProcessed_;
    if (chosen.empty() || chosen[0].empty() || previewRate_ <= 0.0)
        return;

    juce::AudioBuffer<float> buffer((int) chosen.size(), (int) chosen[0].size());
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        std::copy_n(chosen[(size_t) ch].data(), juce::jmin(buffer.getNumSamples(), (int) chosen[(size_t) ch].size()),
                    buffer.getWritePointer(ch));
    engine_.startAudition(buffer, previewRate_);

    static const char* names[] { "processed", "original", "difference - only what the effect changes" };
    showStatus(juce::String("Previewing: ") + names[(int) mode]);
}

/** The dialog closed: the preview stops and its strip goes. */
void MainComponent::endPreview()
{
    engine_.stopAudition();
    previewStrip_.reset();
    previewOriginal_.clear();
    previewProcessed_.clear();
}

} // namespace soundsplice
