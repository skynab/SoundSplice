#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The audio editor: clip gain, selection edits, noise reduction, speed and pitch,
// analysis, and applying or previewing effects on a selection.

namespace soundsplice
{
/** The selected clip if it's an Audio clip that actually references a file,
    or nullptr. Everything the audio editor does needs all three of those to
    hold, so they're checked once here rather than at each call site. */
const model::Clip* MainComponent::selectedAudioClip() const
{
    const auto& song = history_.current();
    if (selectedTrackIndex_ < 0 || selectedTrackIndex_ >= (int) song.tracks.size())
        return nullptr;

    const auto& clips = song.tracks[(size_t) selectedTrackIndex_].clips;
    if (selectedClipIndex_ < 0 || selectedClipIndex_ >= (int) clips.size())
        return nullptr;

    const auto& clip = clips[(size_t) selectedClipIndex_];
    if (clip.type != model::ClipType::Audio || clip.audioFile.empty())
        return nullptr;

    return &clip;
}

void MainComponent::refreshAudioEditorForSelected()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
    {
        audioEditor_.setNoAudioClipSelected();
        return;
    }

    const juce::File file(clip->audioFile);
    const auto&      track = history_.current().tracks[(size_t) selectedTrackIndex_];

    // The clip's audible window rather than the whole file: a split or
    // trimmed clip shares its file with audio it doesn't play, and editing
    // "this clip" has to mean the part that's heard. Positions in the editor
    // are seconds from the clip's start — readClipWindow maps them back.
    const double clipSeconds = clipAudibleSeconds(*clip, engine_.probeDurationSeconds(file),
                                                  history_.current().bpm);
    audioEditor_.setClip(file, clipSeconds, clip->gainDb, track.name, track.colour,
                         clip->sourceOffsetSeconds);
    audioEditor_.setNoisePrintCaptured(! noiseProfiles_.empty() && noiseProfileFile_ == file);

    // Read once per window, not once per refresh. A destructive edit writes a
    // new file and a trim moves the window, so a changed key is exactly the
    // signal that the peaks are stale.
    const auto peaksKey = file.getFullPathName() + "|" + juce::String(clip->sourceOffsetSeconds, 9)
                        + "|" + juce::String(clipSeconds, 9);
    if (peaksKey != waveformPeaksKey_)
    {
        double                          sampleRate = 0.0;
        std::vector<std::vector<float>> fileChannels;
        SampleWindow                    window;

        waveformPeaks_.clear();
        if (readClipWindow(*clip, fileChannels, sampleRate, window))
        {
            std::vector<std::vector<float>> channels;
            for (const auto& channel : fileChannels)
                channels.push_back(windowSamples(channel, window));
            waveformPeaks_.build(channels);
        }

        waveformPeaksKey_        = peaksKey;
        waveformPeaksSampleRate_ = sampleRate;
    }

    audioEditor_.setWaveform(waveformPeaks_, waveformPeaksSampleRate_);
}

/** Writes the selected clip's gain. Live during a slider drag — the
    surrounding beginStructDrag/commitStructDrag pair is what makes the whole
    drag one undo step, same as every other continuous control here. */
void MainComponent::setSelectedClipGainDb(float gainDb)
{
    if (selectedAudioClip() == nullptr)
        return;

    const int trackIndex = selectedTrackIndex_;
    const int clipIndex  = selectedClipIndex_;

    auto& song = history_.mutableCurrent();
    song.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].gainDb = gainDb;

    syncEngineTracks();
}

/** Sets the clip's gain so its loudest sample just reaches kNormaliseTargetPeak.

    Reads the file rather than using the thumbnail's summary: a thumbnail is a
    downsampled peak envelope, so it can under-report the true peak by enough
    to leave a "normalised" clip clipping. Reading is exact and happens once,
    on a button press, which is the one place it's affordable. */
void MainComponent::normaliseSelectedClip()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const juce::File file(clip->audioFile);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        showError("Could not read " + file.getFileName());
        return;
    }

    // Both extremes per channel, then the largest magnitude across them: a
    // waveform is rarely symmetric, so taking only the maximum would
    // under-read a signal whose biggest excursion is negative.
    //
    // Only over the samples the clip plays: a trimmed clip's hidden audio may
    // be louder than anything heard, and normalising to it would leave the
    // clip quieter than it should be.
    const auto window = clipSampleWindow(*clip,
                                         (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                                       std::numeric_limits<int>::max()),
                                         reader->sampleRate, history_.current().bpm);

    const int numChannels = juce::jmax(1, (int) reader->numChannels);
    std::vector<juce::Range<float>> levels((size_t) numChannels);
    if (! window.isEmpty())
        reader->readMaxLevels(window.start, window.length(), levels.data(), numChannels);

    float peak = 0.0f;
    for (const auto& range : levels)
        peak = juce::jmax(peak, std::abs(range.getStart()), std::abs(range.getEnd()));

    if (peak <= 0.0f)
    {
        // Silence has no peak to normalise to, and the alternative is
        // dividing by zero and handing the clip an infinite gain.
        showError("That clip is silent");
        return;
    }

    const float gainDb = juce::Decibels::gainToDecibels(kNormaliseTargetPeak / peak);

    const int trackIndex = selectedTrackIndex_;
    const int clipIndex  = selectedClipIndex_;
    history_.edit("Normalize clip", [trackIndex, clipIndex, gainDb](model::Song& s)
    {
        s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].gainDb = gainDb;
    });

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    showStatus("Normalized: " + juce::String(gainDb, 1) + " dB");
}

/** Reads @p file fully into per-channel float vectors.

    Deliberately its own read rather than reaching into AudioEngine's decode
    cache: that cache is keyed by *path* and shared by every clip pointing at
    the same file, so processing a buffer borrowed from it would silently
    alter every other clip using that recording. Offline editing here always
    reads fresh and writes somewhere new. */
std::vector<std::vector<float>> MainComponent::readAudioFileChannels(const juce::File& file,
                                                                     double& sampleRateOut) const
{
    std::vector<std::vector<float>> channels;
    sampleRateOut = 0.0;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return channels;

    const int numChannels = juce::jmax(1, (int) reader->numChannels);
    const int length      = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                          (juce::int64) std::numeric_limits<int>::max());

    juce::AudioBuffer<float> buffer(numChannels, length);
    reader->read(&buffer, 0, length, 0, true, true);
    sampleRateOut = reader->sampleRate;

    channels.resize((size_t) numChannels);
    for (int ch = 0; ch < numChannels; ++ch)
        channels[(size_t) ch].assign(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + length);

    return channels;
}

/** Reads @p clip's file whole and works out which of its samples the clip
    plays. False if the file can't be read or the clip's window holds no
    audio. */
bool MainComponent::readClipWindow(const model::Clip& clip, std::vector<std::vector<float>>& channelsOut,
                                   double& sampleRateOut, SampleWindow& windowOut) const
{
    channelsOut = readAudioFileChannels(juce::File(clip.audioFile), sampleRateOut);
    windowOut   = {};
    if (channelsOut.empty() || sampleRateOut <= 0.0)
        return false;

    windowOut = clipSampleWindow(clip, (int) channelsOut[0].size(), sampleRateOut, history_.current().bpm);
    return ! windowOut.isEmpty();
}

/** Moves each of @p clipSeconds (seconds from the clip's start) to the
    nearest zero crossing in the clip's audio, so a clip edge placed there
    doesn't click. Left as given if the clip can't be read. */
std::vector<double> MainComponent::zeroCrossingsNear(const model::Clip& clip,
                                                     std::vector<double> clipSeconds) const
{
    double                          sampleRate = 0.0;
    std::vector<std::vector<float>> channels;
    SampleWindow                    window;
    if (! readClipWindow(clip, channels, sampleRate, window))
        return clipSeconds;

    // Decided from the first channel and applied to all: snapping each
    // channel to its own crossing would shear a stereo file apart.
    const auto first = windowSamples(channels[0], window);
    for (auto& seconds : clipSeconds)
    {
        const int at = juce::jlimit(0, window.length(), (int) std::llround(seconds * sampleRate));
        seconds = (double) engine::audioedits::nearestZeroCrossing(first, at) / sampleRate;
    }

    return clipSeconds;
}

/** Copies the selection into the audio clipboard. Non-destructive, so it
    doesn't go through applyDestructiveEdit. */
void MainComponent::copyAudioSelection()
{
    int    from = 0, to = 0, length = 0;
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;

    if (! selectedSampleRange(from, to, length, sampleRate, channels, false))
    {
        showError("Select part of the clip first");
        return;
    }

    audioClipboard_.clear();
    for (const auto& channel : channels)
        audioClipboard_.push_back(engine::audioedits::extractRange(channel, from, to));

    audioClipboardSampleRate_ = sampleRate;
    showStatus("Copied " + juce::String((double) (to - from) / sampleRate, 2) + "s");
}

/** Runs @p transform over the selected range of the clip's audio.

    The range is resolved *inside* the transform, where the samples and the
    file's sample rate are already in hand. That's what makes this read the
    file once: resolving it beforehand meant decoding to find the boundaries
    and then decoding again to edit, which on a long take is two full passes
    over the whole recording for every button press. */
bool MainComponent::editSelection(
    const juce::String& label, bool snapToZeroCrossings,
    const std::function<void(std::vector<std::vector<float>>&, int from, int to, double sampleRate)>& transform)
{
    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        // For a destructive edit "no selection" must refuse rather than mean
        // "the whole clip" — a stray click before Cut would otherwise destroy
        // the take.
        showError("Select part of the clip first");
        return false;
    }

    return applyDestructiveEditToAllChannels(label,
        [range, snapToZeroCrossings, &transform](std::vector<std::vector<float>>& channels, double sampleRate)
    {
        const int length = (int) channels[0].size();
        int       from   = juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
        int       to     = juce::jlimit(0, length, (int) std::llround(range.endSeconds * sampleRate));

        if (snapToZeroCrossings)
        {
            // Decided once from the first channel and applied to all: snapping
            // each channel to its own crossing would shear a stereo file apart
            // at the edit point.
            from = engine::audioedits::nearestZeroCrossing(channels[0], from);
            to   = engine::audioedits::nearestZeroCrossing(channels[0], to);
            if (to < from)
                std::swap(from, to);
        }

        transform(channels, from, to, sampleRate);
    });
}

void MainComponent::cutAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Cut audio", true, [this](std::vector<std::vector<float>>& channels,
                                                int from, int to, double sampleRate)
        {
            // Lifted before the removal, from the same samples that are about
            // to be cut.
            audioClipboard_.clear();
            for (const auto& channel : channels)
                audioClipboard_.push_back(engine::audioedits::extractRange(channel, from, to));
            audioClipboardSampleRate_ = sampleRate;

            for (auto& channel : channels)
                channel = engine::audioedits::removeRange(channel, from, to);
        }))
        showStatus("Cut " + juce::String(range.lengthSeconds(), 2) + "s");
}

void MainComponent::deleteAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Delete audio", true, [](std::vector<std::vector<float>>& channels,
                                               int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::removeRange(channel, from, to);
        }))
        showStatus("Deleted " + juce::String(range.lengthSeconds(), 2) + "s");
}

/** Pastes the clipboard at the selection's start, replacing the selection if
    there is one. Resamples when the clipboard came from a file at another
    rate — otherwise pasting 44.1k into 48k would play back too fast. */
void MainComponent::pasteAudioAtSelection()
{
    if (audioClipboard_.empty())
    {
        showError("Nothing to paste");
        return;
    }

    const auto range = audioEditor_.selection();

    // With no selection, paste lands at the cursor — not at the start. It
    // used to read startSeconds off an *empty* range, which is always zero,
    // so every paste without a selection went to the beginning of the clip
    // however far along the cursor had been placed.
    const double atSeconds = range.isEmpty() ? audioEditor_.cursorSeconds() : range.startSeconds;

    const bool applied = applyDestructiveEditToAllChannels("Paste audio",
        [this, range, atSeconds](std::vector<std::vector<float>>& channels, double sampleRate)
    {
        const int wanted = juce::jmax(0, (int) std::llround(atSeconds * sampleRate));

        // A cursor past the end of the file is a request to paste *after* the
        // recording, so the gap is filled with silence rather than the paste
        // being dragged back to the last sample.
        for (auto& channel : channels)
            if (wanted > (int) channel.size())
                channel.resize((size_t) wanted, 0.0f);

        const int length = (int) channels[0].size();
        const int at     = juce::jlimit(0, length, wanted);
        const int until  = range.isEmpty()
                             ? at
                             : juce::jlimit(at, length, (int) std::llround(range.endSeconds * sampleRate));

        const double ratio = audioClipboardSampleRate_ > 0.0 ? audioClipboardSampleRate_ / sampleRate : 1.0;

        for (int ch = 0; ch < (int) channels.size(); ++ch)
        {
            // A mono clipboard into a stereo clip (or the reverse) reuses the
            // last available channel rather than refusing — the same rule the
            // players follow for channel-count mismatches.
            const auto& source = audioClipboard_[(size_t) juce::jmin(ch, (int) audioClipboard_.size() - 1)];
            const auto  fitted = std::abs(ratio - 1.0) < 1.0e-9
                                     ? source
                                     : engine::audioedits::resample(source, ratio);

            const auto cleared = until > at
                                     ? engine::audioedits::removeRange(channels[(size_t) ch], at, until)
                                     : channels[(size_t) ch];
            channels[(size_t) ch] = engine::audioedits::insertAt(cleared, fitted, at);
        }
    });

    if (applied)
        showStatus("Pasted");
}

/** Trims the clip to the selection without touching its file: the clip's
    window moves onto the selection, so the audio either side is still there
    to drag back out from the timeline. The kept audio stays where it was on
    the timeline rather than jumping back to the clip's old start. */
void MainComponent::trimToAudioSelection()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        showError("Select part of the clip first");
        return;
    }

    // Snapped, as the destructive trim was: a clip edge that lands mid-cycle
    // clicks.
    auto edges = zeroCrossingsNear(*clip, { range.startSeconds, range.endSeconds });
    if (edges[1] < edges[0])
        std::swap(edges[0], edges[1]);

    const double from = edges[0];
    const double to   = edges[1];
    if (to - from <= 0.0)
    {
        showError("That would leave the clip empty");
        return;
    }

    const int    trackIndex = selectedTrackIndex_;
    const int    clipIndex  = selectedClipIndex_;
    const double bpm        = history_.current().bpm;

    history_.edit("Trim audio", [trackIndex, clipIndex, from, to, bpm](model::Song& s)
    {
        auto& target = s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
        target = trimClipToRange(target, from, to, bpm);
    });

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    arrangementView_.setSong(history_.current());
    showStatus("Trimmed to " + juce::String(to - from, 2) + "s");
}

void MainComponent::silenceAudioSelection()
{
    // No zero-crossing snap: nothing moves, so there is no join to click.
    if (editSelection("Silence audio", false, [](std::vector<std::vector<float>>& channels,
                                                 int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::silenceRange(channel, from, to);
        }))
        showStatus("Silenced");
}

void MainComponent::fadeInAudioSelection()
{
    if (editSelection("Fade in", false, [](std::vector<std::vector<float>>& channels,
                                           int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::fadeIn(channel, from, to);
        }))
        showStatus("Faded in");
}

void MainComponent::fadeOutAudioSelection()
{
    if (editSelection("Fade out", false, [](std::vector<std::vector<float>>& channels,
                                            int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::fadeOut(channel, from, to);
        }))
        showStatus("Faded out");
}

void MainComponent::reverseAudioSelection()
{
    if (editSelection("Reverse audio", true, [](std::vector<std::vector<float>>& channels,
                                                int from, int to, double)
        {
            for (auto& channel : channels)
                channel = engine::audioedits::reverseRange(channel, from, to);
        }))
        showStatus("Reversed");
}

/** Splits the clip in two at the selection's start without touching its
    file: both halves play the same recording, the second from further in.
    It used to write each half out as a file of its own, which doubled the
    disk use of every split and made rejoining them impossible. */
void MainComponent::splitClipAtSelection()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        showError("Select where to split first");
        return;
    }

    const double bpm         = history_.current().bpm;
    const double clipSeconds = clipAudibleSeconds(*clip, engine_.probeDurationSeconds(juce::File(clip->audioFile)),
                                                  bpm);
    const double at          = zeroCrossingsNear(*clip, { range.startSeconds })[0];

    if (at <= 0.0 || at >= clipSeconds)
    {
        showError("That split point is at the very edge of the clip");
        return;
    }

    const int trackIndex = selectedTrackIndex_;
    const int clipIndex  = selectedClipIndex_;

    history_.edit("Split audio", [trackIndex, clipIndex, at, bpm](model::Song& s)
    {
        auto&      track  = s.tracks[(size_t) trackIndex];
        const auto halves = splitClipAt(track.clips[(size_t) clipIndex], at, bpm);

        track.clips[(size_t) clipIndex] = halves.first;
        model::addClip(s, track.id, halves.second); // reissues the id
    });

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    arrangementView_.setSong(history_.current());
    showStatus("Split at " + juce::String(at, 2) + "s");
}

/** Offers a scratch effect chain to render into the selection. */
void MainComponent::showApplyEffectsDialog()
{
    if (selectedAudioClip() == nullptr)
        return;

    if (audioEditor_.selection().isEmpty())
    {
        showError("Select part of the clip first");
        return;
    }

    auto dialog = std::make_unique<ApplyEffectsDialog>();
    dialog->setSize(520, 460);

    dialog->setUserPresets(userEffectPresets_);
    dialog->onPresetSaveRequested = [this](const model::EffectSlot& slot) { promptToSaveEffectPreset(slot); };
    dialog->onUserPresetDeleted   = [this](const std::string& effectId, const std::string& name)
    {
        deleteUserEffectPreset(effectId, name);
    };
    applyEffectsDialog_ = dialog.get();

    dialog->onPreview   = [this](const std::vector<model::EffectSlot>& chain) { previewEffectsOnSelection(chain); };
    dialog->onDismissed = [safe = juce::Component::SafePointer<MainComponent>(this)]
    {
        if (safe != nullptr)
            safe->engine_.stopAudition();
    };

    auto* raw = dialog.get();
    raw->onApply = [this, raw](const std::vector<model::EffectSlot>& chain)
    {
        engine_.stopAudition();
        applyEffectsToSelection(chain);
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };
    raw->onCancel = [raw]
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle                  = "Apply Effects to Selection";
    options.dialogBackgroundColour       = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar            = true;
    options.resizable                    = true;
    options.launchAsync();
}

/** Plays the start of the selection through @p chain without changing
    anything, or stops a preview that is already playing, which is what
    pressing Preview a second time means. */
void MainComponent::previewEffectsOnSelection(const std::vector<model::EffectSlot>& chain)
{
    if (engine_.isAuditioning())
    {
        engine_.stopAudition();
        showStatus("Preview stopped");
        return;
    }

    const auto [builtIns, plugins] = renderableEffectCounts(chain);
    if (builtIns == 0)
    {
        showError(plugins > 0 ? "Plugins can't be previewed on a selection yet" : "Add an effect first");
        return;
    }

    int    from = 0, to = 0, length = 0;
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;
    if (! selectedSampleRange(from, to, length, sampleRate, channels, false) || to <= from)
    {
        showError("Select part of the clip first");
        return;
    }

    const int count       = juce::jmin(to - from, (int) std::llround(kEffectPreviewSeconds * sampleRate));
    const int numChannels = (int) channels.size();

    juce::AudioBuffer<float> block(numChannels, count);
    for (int ch = 0; ch < numChannels; ++ch)
        std::copy(channels[(size_t) ch].begin() + from, channels[(size_t) ch].begin() + from + count,
                  block.getWritePointer(ch));

    renderEffectChain(chain, block, sampleRate, history_.current().bpm);

    // A few milliseconds of fade at each end, so a preview that starts or
    // ends mid-waveform doesn't click.
    const int fade = juce::jmin(count / 2, (int) std::llround(sampleRate * kEffectEdgeFadeSeconds));
    if (fade > 0)
    {
        block.applyGainRamp(0, fade, 0.0f, 1.0f);
        block.applyGainRamp(count - fade, fade, 1.0f, 0.0f);
    }

    // The song stops: a preview heard over the mix is hard to judge.
    post(Cmd::SetPlaying, 0.0);
    engine_.startAudition(block, sampleRate);
    showStatus("Previewing " + juce::String((double) count / sampleRate, 1) + "s - press Preview again to stop");
}

/** Renders @p chain into the selected range.

    The range is processed as its own buffer and written back over the
    original, with a short crossfade at each boundary. Without the crossfade
    an effect that changes level — any compressor, or a reverb's wet mix —
    produces a step at the edges of the selection, heard as a click exactly
    where the edit begins and ends. A few milliseconds of blend removes it
    and is far too short to be heard as a fade.

    Plugin slots are skipped: instantiating one needs the plugin host, which
    lives in the engine, and a half-rendered chain would be worse than an
    honest refusal. */
void MainComponent::applyEffectsToSelection(const std::vector<model::EffectSlot>& chain)
{
    const auto range = audioEditor_.selection();
    if (range.isEmpty())
        return;

    const auto [builtIns, plugins] = renderableEffectCounts(chain);

    if (builtIns == 0)
    {
        showError(plugins > 0 ? "Plugins can't be rendered into a selection yet"
                              : "Add an effect first");
        return;
    }

    const double bpm = history_.current().bpm;

    const bool applied = applyDestructiveEditToAllChannels("Apply effects",
        [&chain, range, bpm](std::vector<std::vector<float>>& channels, double sampleRate)
    {
        const int total = (int) channels[0].size();
        const int from  = juce::jlimit(0, total, (int) std::llround(range.startSeconds * sampleRate));
        const int to    = juce::jlimit(from, total, (int) std::llround(range.endSeconds * sampleRate));
        const int count = to - from;
        if (count <= 0)
            return;

        const int numChannels = (int) channels.size();

        juce::AudioBuffer<float> block(numChannels, count);
        for (int ch = 0; ch < numChannels; ++ch)
            std::copy(channels[(size_t) ch].begin() + from,
                      channels[(size_t) ch].begin() + to,
                      block.getWritePointer(ch));

        renderEffectChain(chain, block, sampleRate, bpm);

        // Blend back over the original at both edges.
        const int fade = juce::jmin(count / 2, (int) std::llround(sampleRate * kEffectEdgeFadeSeconds));
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto*       destination = channels[(size_t) ch].data() + from;
            const auto* processed   = block.getReadPointer(ch);

            for (int i = 0; i < count; ++i)
            {
                float wet = 1.0f;
                if (fade > 0)
                {
                    if (i < fade)                 wet = (float) i / (float) fade;
                    else if (i >= count - fade)   wet = (float) (count - 1 - i) / (float) fade;
                }
                destination[i] = destination[i] * (1.0f - wet) + processed[i] * wet;
            }
        }
    });

    if (applied)
        showStatus(plugins > 0 ? "Applied effects (plugins skipped)" : "Applied effects");
}

/** Measures the frequency content of the audio editor's selection.

    Falls back to the whole clip when nothing is selected: unlike the
    destructive actions, analysing everything is harmless — the same rule the
    audition transport follows. */
void MainComponent::analyseSelection()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    double                          sampleRate = 0.0;
    std::vector<std::vector<float>> file;
    SampleWindow                    window;
    if (! readClipWindow(*clip, file, sampleRate, window))
    {
        showError("Could not read that clip");
        return;
    }

    const auto channel = windowSamples(file[0], window);
    const auto range   = audioEditor_.selection();
    const int  length  = (int) channel.size();
    const int  from   = range.isEmpty() ? 0
                          : juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
    const int  to     = range.isEmpty() ? length
                          : juce::jlimit(from, length, (int) std::llround(range.endSeconds * sampleRate));

    // Channel 0 rather than a sum: summing a stereo pair cancels whatever is
    // out of phase between them, which would hide exactly the kind of problem
    // someone opens an analyser to find.
    const std::vector<float> passage(channel.begin() + from, channel.begin() + to);

    const auto measured = engine::spectrum::analyse(passage, sampleRate);
    if (measured.isEmpty())
    {
        showError("That selection is too short to analyse - select at least ~50ms");
        return;
    }

    analyserPane_.setSpectrum(measured);
    showStatus("Analysed " + juce::String((double) (to - from) / sampleRate, 2) + "s");
}

/** Speed and pitch, on the whole clip.

    Whole clip rather than a selection on purpose: both change the audio's
    duration, and splicing a re-timed section back into the middle of a clip
    would either leave a gap or overlap what follows. Audacity's own
    Change Speed works this way for the same reason. */
void MainComponent::showSpeedPitchDialog()
{
    if (selectedAudioClip() == nullptr)
        return;

    auto* window = new juce::AlertWindow("Speed and Pitch", {}, juce::MessageBoxIconType::NoIcon, this);

    window->addComboBox("speed", { "0.5x (half)", "0.75x", "1x (unchanged)", "1.5x", "2x (double)" },
                        "Speed (moves pitch with it):");
    window->getComboBoxComponent("speed")->setSelectedItemIndex(2);

    juce::StringArray semitones;
    for (int i = -12; i <= 12; ++i)
        semitones.add(i == 0 ? juce::String("0 (unchanged)") : juce::String(i > 0 ? "+" : "") + juce::String(i));
    window->addComboBox("pitch", semitones, "Pitch, keeping the length:");
    window->getComboBoxComponent("pitch")->setSelectedItemIndex(12); // 0

    window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            static const double kSpeeds[] = { 0.5, 0.75, 1.0, 1.5, 2.0 };
            const int speedIndex = window->getComboBoxComponent("speed")->getSelectedItemIndex();
            const int pitchIndex = window->getComboBoxComponent("pitch")->getSelectedItemIndex();

            self->applySpeedAndPitch(kSpeeds[(size_t) juce::jlimit(0, 4, speedIndex)],
                                     (double) (juce::jlimit(0, 24, pitchIndex) - 12));
        }));
}

/** Applies a speed change and a pitch shift to the whole clip.

    Speed first, then pitch: the pitch shift preserves length, so doing it
    second means it operates on the already-retimed audio and the two
    settings compose the way the dialog implies. */
void MainComponent::applySpeedAndPitch(double speedFactor, double semitones)
{
    const bool changesSpeed = std::abs(speedFactor - 1.0) > 1.0e-9;
    const bool changesPitch = std::abs(semitones) > 1.0e-9;

    if (! changesSpeed && ! changesPitch)
        return;

    showBusy("Processing...");

    const bool applied = applyDestructiveEditToAllChannels("Speed and pitch",
        [speedFactor, semitones, changesSpeed, changesPitch](std::vector<std::vector<float>>& channels, double)
    {
        for (auto& channel : channels)
        {
            if (changesSpeed)
                channel = engine::timestretch::changeSpeed(channel, speedFactor);
            if (changesPitch)
                channel = engine::timestretch::pitchShift(channel, semitones);
        }
    });

    if (applied)
    {
        juce::String what;
        if (changesSpeed) what += juce::String(speedFactor, 2) + "x speed";
        if (changesSpeed && changesPitch) what += ", ";
        if (changesPitch) what += juce::String(semitones > 0 ? "+" : "") + juce::String((int) semitones) + " semitones";
        showStatus("Applied " + what);
    }
}

/** The song beat a point in the selected clip's file corresponds to.

    The audio editor works in seconds into a file; the transport works in
    song beats. This is the one place that conversion lives, so the click
    gesture and the playhead drawing can't disagree about it. */
double MainComponent::songBeatForClipSeconds(double secondsIntoFile) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return 0.0;

    return clip->startBeats + engine::beatsForSeconds(secondsIntoFile, history_.current().bpm);
}

/** The inverse: where the song's playhead falls inside the selected clip's
    file. Negative before the clip starts, past its end after — the editor
    simply draws the playhead off the edge of the view in both cases. */
double MainComponent::clipSecondsForSongBeat(double beat) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return 0.0;

    const double bpm = history_.current().bpm;
    if (bpm <= 0.0)
        return 0.0;

    return (beat - clip->startBeats) * 60.0 / bpm;
}

/** Measures the noise in the selected range, per channel.

    Per channel rather than from a mono sum: a stereo recording's two sides
    routinely have different noise floors (different preamps, or one side
    nearer a fan), and subtracting an average from both would under-clean one
    and over-clean the other. */
void MainComponent::captureNoisePrint()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
    {
        showError("Select a passage of noise first");
        return;
    }

    const juce::File                file(clip->audioFile);
    double                          sampleRate = 0.0;
    std::vector<std::vector<float>> fileChannels;
    SampleWindow                    window;

    if (! readClipWindow(*clip, fileChannels, sampleRate, window))
    {
        showError("Could not read " + file.getFileName());
        return;
    }

    std::vector<std::vector<float>> channels;
    for (const auto& channel : fileChannels)
        channels.push_back(windowSamples(channel, window));

    const int total = (int) channels[0].size();
    const int from  = juce::jlimit(0, total, (int) std::llround(range.startSeconds * sampleRate));
    const int to    = juce::jlimit(from, total, (int) std::llround(range.endSeconds * sampleRate));

    std::vector<engine::NoiseProfile> profiles;
    for (const auto& channel : channels)
    {
        const std::vector<float> passage(channel.begin() + from, channel.begin() + to);
        profiles.push_back(engine::noisereduction::captureNoiseProfile(passage));
    }

    // captureNoiseProfile refuses a passage shorter than one analysis frame,
    // which is the honest answer rather than a profile built from padding —
    // so that refusal has to be reported, not silently stored.
    if (profiles.empty() || profiles[0].isEmpty())
    {
        showError("That selection is too short to measure - select at least ~50ms");
        return;
    }

    noiseProfiles_    = std::move(profiles);
    noiseProfileFile_ = file;
    audioEditor_.setNoisePrintCaptured(true);
    showStatus("Noise print captured from "
               + juce::String(range.lengthSeconds(), 2) + "s");
}

/** Subtracts the captured print from the whole clip, writing the result to a
    new file and repointing the clip at it in one undo step.

    Writing a new file rather than editing in place is what keeps this
    undoable and keeps it from leaking: undo just points the clip back at the
    original, which is still on disk and untouched. It also sidesteps the
    decode cache's path-keyed sharing entirely — a new path is a new entry. */
void MainComponent::reduceNoiseOnSelectedClip(float amountDb, float floorDb)
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    if (noiseProfiles_.empty() || noiseProfileFile_ != juce::File(clip->audioFile))
    {
        showError("Capture a noise print from this clip first");
        return;
    }

    showBusy("Reducing noise...");

    // Through the shared destructive path rather than its own copy of it.
    // This predated that helper and had drifted: it wrote into the
    // Recordings folder alongside real takes instead of the Edits folder
    // every other edit uses, and it repeated the repoint-and-invalidate
    // sequence that only has to be right once.
    const bool applied = applyDestructiveEditToAllChannels("Reduce noise",
        [this, amountDb, floorDb](std::vector<std::vector<float>>& channels, double)
    {
        for (int ch = 0; ch < (int) channels.size(); ++ch)
        {
            // A mono print on a stereo file (or the reverse) is possible if
            // the file changed underneath; reusing the last profile is better
            // than refusing, and clamping is how.
            const auto& profile = noiseProfiles_[(size_t) juce::jmin(ch, (int) noiseProfiles_.size() - 1)];
            channels[(size_t) ch] = engine::noisereduction::reduceNoise(channels[(size_t) ch], profile,
                                                                        amountDb, floorDb);
        }
    });

    if (applied)
        showStatus("Noise reduced");
}

juce::File MainComponent::editsDirectory() const
{
    // Separate from Recordings: these are derived files, and mixing them in
    // with takes makes it impossible to tell which is which when clearing
    // out space later.
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("SoundSplice Edits");
    dir.createDirectory();
    return dir;
}

/** Reads the selected clip and resolves the editor's selection to sample
    indices. Returns false — having reported why — when there's no selection,
    which for a destructive edit must refuse rather than quietly mean "the
    whole clip": a stray click before Cut would otherwise destroy the take. */
bool MainComponent::selectedSampleRange(int& fromOut, int& toOut, int& lengthOut,
                                        double& sampleRateOut,
                                        std::vector<std::vector<float>>& channelsOut,
                                        bool snapToZeroCrossings) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return false;

    const auto range = audioEditor_.selection();
    if (range.isEmpty())
        return false;

    std::vector<std::vector<float>> file;
    SampleWindow                    window;
    if (! readClipWindow(*clip, file, sampleRateOut, window))
        return false;

    channelsOut.clear();
    for (const auto& channel : file)
        channelsOut.push_back(windowSamples(channel, window));

    lengthOut = (int) channelsOut[0].size();
    fromOut   = juce::jlimit(0, lengthOut, (int) std::llround(range.startSeconds * sampleRateOut));
    toOut     = juce::jlimit(0, lengthOut, (int) std::llround(range.endSeconds * sampleRateOut));

    if (snapToZeroCrossings)
    {
        // Decided once, from the first channel, and applied to all of them:
        // snapping each channel to its own crossing would shear a stereo
        // file apart at the edit point.
        fromOut = engine::audioedits::nearestZeroCrossing(channelsOut[0], fromOut);
        toOut   = engine::audioedits::nearestZeroCrossing(channelsOut[0], toOut);
        if (toOut < fromOut)
            std::swap(fromOut, toOut);
    }

    return true;
}

/** The one path every destructive edit takes.

    Centralised because each step is easy to forget individually and each
    failure is quiet: a clip whose lengthBeats isn't updated plays the old
    duration, and a stale waveform-peaks cache draws the old audio over the
    new. */
bool MainComponent::applyDestructiveEdit(
    const juce::String& label,
    const std::function<std::vector<float>(const std::vector<float>&, int channel)>& transform)
{
    return applyDestructiveEditToAllChannels(label,
        [&transform](std::vector<std::vector<float>>& channels, double)
        {
            for (int ch = 0; ch < (int) channels.size(); ++ch)
                channels[(size_t) ch] = transform(channels[(size_t) ch], ch);
        });
}

bool MainComponent::applyDestructiveEditToAllChannels(
    const juce::String& label,
    const std::function<void(std::vector<std::vector<float>>&, double sampleRate)>& transform)
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return false;

    const juce::File                source(clip->audioFile);
    double                          sampleRate = 0.0;
    std::vector<std::vector<float>> file;
    SampleWindow                    window;
    if (! readClipWindow(*clip, file, sampleRate, window))
    {
        showError("Could not read " + source.getFileName());
        return false;
    }

    // The transform sees only the samples the clip plays, indexed from the
    // clip's own start — the same coordinates the editor's selection is in.
    std::vector<std::vector<float>> channels;
    for (const auto& channel : file)
        channels.push_back(windowSamples(channel, window));

    transform(channels, sampleRate);

    const int newLength = channels.empty() ? 0 : (int) channels[0].size();
    if (newLength <= 0)
    {
        // Editing a clip down to nothing would leave a clip referencing an
        // unreadable file, which plays as silence with no explanation.
        showError("That would leave the clip empty");
        return false;
    }

    // Spliced back between the audio either side of the window, so a trimmed
    // or split clip keeps the audio it hides and its offset still points at
    // the same place: only the window's length changes.
    std::vector<std::vector<float>> spliced;
    for (int ch = 0; ch < (int) channels.size(); ++ch)
    {
        // Channels can differ in length only if a transform is inconsistent,
        // which is a bug — but writing past the buffer would be a crash, so
        // each is padded or cut to the first channel's length.
        auto part = channels[(size_t) ch];
        part.resize((size_t) newLength, 0.0f);
        spliced.push_back(spliceWindow(file[(size_t) juce::jmin(ch, (int) file.size() - 1)], window, part));
    }

    const int totalLength = (int) spliced[0].size();
    juce::AudioBuffer<float> buffer((int) spliced.size(), totalLength);
    for (int ch = 0; ch < (int) spliced.size(); ++ch)
        std::copy(spliced[(size_t) ch].begin(), spliced[(size_t) ch].end(), buffer.getWritePointer(ch));

    const auto destination = editsDirectory()
                                 .getNonexistentChildFile(source.getFileNameWithoutExtension(), ".wav");
    if (! engine::OfflineRenderer::writeWav(destination, buffer, sampleRate))
    {
        showError("Could not write " + destination.getFileName());
        return false;
    }

    // The clip's window takes the edited audio's new duration. Its offset
    // doesn't move, because nothing before the window changed.
    const double newSeconds     = (double) newLength / sampleRate;
    const int    trackIndex     = selectedTrackIndex_;
    const int    clipIndex      = selectedClipIndex_;
    const auto   newPath        = destination.getFullPathName().toStdString();
    const double newLengthBeats = engine::beatsForSeconds(newSeconds, history_.current().bpm);

    history_.edit(label.toStdString(), [trackIndex, clipIndex, newPath, newLengthBeats](model::Song& s)
    {
        auto& target       = s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
        target.audioFile   = newPath;
        target.lengthBeats = juce::jmax(0.25, newLengthBeats);
    });

    // A noise print described the old file, and the peaks cache is keyed by
    // path — without clearing it the editor keeps drawing the old audio.
    noiseProfiles_.clear();
    noiseProfileFile_  = juce::File{};
    waveformPeaksKey_  = {};

    syncEngineTracks();
    refreshAudioEditorForSelected();
    refreshAutomationPaneForSelected();
    arrangementView_.setSong(history_.current());
    return true;
}

/** Remembers the selected clip's gain before a drag on the audio editor's
    gain slider started, so the whole drag lands as one undo step rather than
    one per mouse-move — the same pair, for the same reason, as the fader
    drags. */
void MainComponent::beginClipGainDrag()
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    clipGainDragging_   = true;
    clipGainDragTrack_  = selectedTrackIndex_;
    clipGainDragClip_   = selectedClipIndex_;
    clipGainDragFrom_   = clip->gainDb;
}

void MainComponent::endClipGainDrag()
{
    if (! clipGainDragging_
        || clipGainDragTrack_ != selectedTrackIndex_
        || clipGainDragClip_ != selectedClipIndex_)
        return;

    clipGainDragging_ = false;

    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const int   trackIndex = clipGainDragTrack_;
    const int   clipIndex  = clipGainDragClip_;
    const float landedOn   = clip->gainDb;

    commitStructDrag(history_, "Set clip gain", clipGainDragFrom_, landedOn,
                     [trackIndex, clipIndex](model::Song& s, const float& value)
    {
        s.tracks[(size_t) trackIndex].clips[(size_t) clipIndex].gainDb = value;
    });
}

} // namespace soundsplice
