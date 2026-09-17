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

    // Whatever the editor shows is open, however it got there.
    if (clip != nullptr)
        openFiles_.open(clip->id);
    updateOpenFilesPane();

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
    // are seconds from the clip's start — readClipAudio maps them back.
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
        waveformPeaks_.clear();
        waveformPeaksSampleRate_ = 0.0;

        ClipAudio audio;
        if (openSelectedClipAudio(audio))
        {
            // A chunk at a time, so a long recording is never all in memory
            // at once just to be drawn: the peaks are a tiny fraction of it.
            constexpr int chunk  = WaveformPeaks::kDefaultSamplesPerBin * 16384;
            const int     length = audio.window.length();
            for (int from = 0; from < length; from += chunk)
            {
                const auto channels = readClipAudio(audio, from, from + chunk);
                if (channels.empty())
                {
                    waveformPeaks_.clear();
                    break;
                }
                waveformPeaks_.append(channels);
            }
            waveformPeaksSampleRate_ = audio.sequence.sampleRate;
        }

        waveformPeaksKey_ = peaksKey;
    }

    audioEditor_.setWaveform(waveformPeaks_, waveformPeaksSampleRate_);
}

void MainComponent::updateOpenFilesPane()
{
    const auto& song = history_.current();
    openFiles_.prune(song);

    std::vector<OpenFilesPane::Entry> entries;
    for (int id : openFiles_.clipIds())
    {
        const auto  where = app::OpenFiles::locate(song, id);
        const auto& track = song.tracks[(size_t) where.track];
        const auto& clip  = track.clips[(size_t) where.clip];

        // The clip's length in the arrangement: probing the file here would
        // open every one of them on each refresh.
        const double seconds = song.bpm > 0.0 ? clip.lengthBeats * 60.0 / song.bpm : 0.0;
        const int    minutes = (int) (seconds / 60.0);

        OpenFilesPane::Entry entry;
        entry.clipId = id;
        entry.name   = juce::File(clip.audioFile).getFileNameWithoutExtension();
        entry.detail = juce::String(track.name.empty() ? "Track" : track.name) + "  |  " + juce::String(minutes)
                     + ":" + juce::String(seconds - minutes * 60.0, 1).paddedLeft('0', 4);
        entry.colour = track.colour != 0 ? juce::Colour(track.colour) : juce::Colour(0xff5a6a80);
        entries.push_back(std::move(entry));
    }

    const auto* showing = selectedAudioClip();
    openFilesPane_.setEntries(std::move(entries), showing != nullptr ? showing->id : 0);
}

void MainComponent::showOpenFile(int clipId)
{
    const auto where = app::OpenFiles::locate(history_.current(), clipId);
    if (! where.isValid())
    {
        updateOpenFilesPane();
        return;
    }

    selectTrackAndClip(where.track, where.clip);
    if (workspace_.isPanelOpen("Audio"))
        workspace_.revealPanel("Audio");
}

void MainComponent::closeOpenFile(int clipId)
{
    const auto* showing = selectedAudioClip();
    const bool  wasShowing = showing != nullptr && showing->id == clipId;
    const int   next       = openFiles_.close(clipId);

    if (! wasShowing)
    {
        updateOpenFilesPane();
        return;
    }

    if (next != 0)
    {
        showOpenFile(next);
        return;
    }

    // Nothing left open: the editor shows nothing rather than reopening the
    // clip that was just closed.
    selectTrackAndClip(selectedTrackIndex_, -1);
}

void MainComponent::closeAllOpenFiles()
{
    openFiles_.closeAll();
    if (selectedAudioClip() != nullptr)
        selectTrackAndClip(selectedTrackIndex_, -1);
    else
        updateOpenFilesPane();
}

void MainComponent::stepOpenFile(int direction)
{
    const auto* showing = selectedAudioClip();
    const int   next    = openFiles_.neighbour(showing != nullptr ? showing->id : 0, direction);
    if (next != 0)
        showOpenFile(next);
}

/** A clip's volume curve as drawn in the arrangement: one undo step per
    gesture, since the view only reports a curve when a point is released. */
void MainComponent::setClipEnvelope(int trackIndex, int clipIndex, const engine::ClipEnvelope& envelope)
{
    history_.edit("Edit volume curve", [trackIndex, clipIndex, envelope](model::Song& s)
    {
        if (trackIndex < 0 || trackIndex >= (int) s.tracks.size())
            return;

        auto& clips = s.tracks[(size_t) trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < (int) clips.size())
            clips[(size_t) clipIndex].envelope = envelope;
    });

    syncEngineTracks();
    arrangementView_.setSong(history_.current());
}

/** The selected clip's actual samples over [fromSeconds, toSeconds), seconds
    from its start, for the audio editor zoomed in past its peaks. Only what's
    asked for is read, which at a zoom that close is a few tens of thousands
    of samples at most; a request for more than that isn't one a close zoom
    makes, and is ignored. */
void MainComponent::sendSampleDetailToEditor(double fromSeconds, double toSeconds)
{
    ClipAudio audio;
    if (! openSelectedClipAudio(audio))
        return;

    constexpr int kMostFrames = 1 << 20;

    const double rate   = audio.sequence.sampleRate;
    const int    length = audio.window.length();
    const int    from   = juce::jlimit(0, length, (int) std::floor(fromSeconds * rate));
    const int    to     = juce::jlimit(from, length, (int) std::ceil(toSeconds * rate));
    if (to <= from || to - from > kMostFrames)
        return;

    SampleDetail detail;
    detail.startSeconds = (double) from / rate;
    detail.sampleRate   = rate;
    detail.channels     = readClipAudio(audio, from, to);
    audioEditor_.setSampleDetail(std::move(detail));
}

/** A stroke of the draw tool: samples from @p firstSample of @p channel
    redrawn by hand, usually to take out a click. Written like any other edit,
    as a new block over just those samples, so it undoes as one step; the
    clip's other channels over the same span are written back unchanged. */
void MainComponent::drawSamplesOnSelectedClip(int channel, long firstSample, const std::vector<float>& values)
{
    ClipAudio audio;
    if (values.empty() || ! openSelectedClipAudio(audio))
        return;

    const int from = (int) juce::jlimit(0L, (long) audio.window.length(), firstSample);
    const int to   = (int) juce::jlimit((long) from, (long) audio.window.length(), firstSample + (long) values.size());
    if (to <= from)
        return;

    auto channels = readClipAudio(audio, from, to);
    if (channels.empty() || channel < 0 || channel >= (int) channels.size())
    {
        showError("Could not read " + audio.file.getFileName());
        return;
    }

    auto& target = channels[(size_t) channel];
    std::copy_n(values.begin() + (from - firstSample), juce::jmin((size_t) (to - from), target.size()), target.begin());

    if (replaceClipAudio("Draw samples", audio, from, to, channels))
        showStatus("Redrew " + juce::String(to - from) + (to - from == 1 ? " sample" : " samples"));
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
    engine::sequencefile::registerFormats(formats);

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

/** The selected clip's audio as a sequence, and which of its samples the clip
    plays. Reads the file's header (or the sequence file) but no samples.
    False if no audio clip is selected or its audio can't be opened. */
bool MainComponent::openSelectedClipAudio(ClipAudio& out) const
{
    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return false;

    out.file      = juce::File(clip->audioFile);
    auto sequence = engine::sequencefile::sequenceOf(out.file);
    if (! sequence || sequence->sampleRate <= 0.0)
        return false;

    out.sequence = std::move(*sequence);
    out.window   = clipSampleWindow(*clip,
                                    (int) juce::jmin<std::int64_t>(out.sequence.length(),
                                                                   std::numeric_limits<int>::max()),
                                    out.sequence.sampleRate, history_.current().bpm);
    return true;
}

/** Samples [from, to) of the clip, counted from its start and clamped to it.

    Only those samples are read, however long the recording, and always fresh
    rather than from AudioEngine's decode cache: that cache is shared by every
    clip playing the same file, so processing a buffer borrowed from it would
    silently alter all of them. */
std::vector<std::vector<float>> MainComponent::readClipAudio(const ClipAudio& audio, int from, int to) const
{
    const int length = audio.window.length();
    from = juce::jlimit(0, length, from);
    to   = juce::jlimit(from, length, to);

    juce::AudioBuffer<float> buffer;
    if (! engine::sequencefile::readRange(audio.file, (juce::int64) audio.window.start + from, to - from, buffer))
        return {};

    std::vector<std::vector<float>> channels((size_t) buffer.getNumChannels());
    if (buffer.getNumSamples() > 0)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            channels[(size_t) ch].assign(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + buffer.getNumSamples());

    return channels;
}

/** The nearest zero crossing to sample @p at of the clip (counted from its
    start), reading only the samples the search can reach.

    Decided from the first channel and applied to all: snapping each channel
    to its own crossing would shear a stereo file apart. */
int MainComponent::zeroCrossingNear(const ClipAudio& audio, int at) const
{
    constexpr int radius = 512;

    const int length = audio.window.length();
    at = juce::jlimit(0, length, at);

    // One sample more either side than the search reaches, so a crossing at
    // the edge of its reach has the sample before it to compare against.
    const int  from   = juce::jmax(0, at - radius - 1);
    const auto nearby = readClipAudio(audio, from, at + radius + 1);
    if (nearby.empty())
        return at;

    return from + engine::audioedits::nearestZeroCrossing(nearby[0], at - from, radius);
}

/** Moves each of @p clipSeconds (seconds from the selected clip's start) to
    the nearest zero crossing in its audio, so a clip edge placed there
    doesn't click. Left as given if the clip can't be read. */
std::vector<double> MainComponent::zeroCrossingsNear(std::vector<double> clipSeconds) const
{
    ClipAudio audio;
    if (! openSelectedClipAudio(audio) || audio.window.isEmpty())
        return clipSeconds;

    const double sampleRate = audio.sequence.sampleRate;
    for (auto& seconds : clipSeconds)
        seconds = (double) zeroCrossingNear(audio, (int) std::llround(seconds * sampleRate)) / sampleRate;

    return clipSeconds;
}

/** Copies the selection into the audio clipboard. Non-destructive, so it
    doesn't go through replaceClipAudio. */
void MainComponent::copyAudioSelection()
{
    ClipAudio audio;
    int       from = 0, to = 0;
    if (! openSelectedClipAudio(audio) || ! selectedClipRange(audio, from, to, false))
    {
        showError("Select part of the clip first");
        return;
    }

    auto channels = readClipAudio(audio, from, to);
    if (channels.empty())
    {
        showError("Could not read " + audio.file.getFileName());
        return;
    }

    audioClipboard_           = std::move(channels);
    audioClipboardSampleRate_ = audio.sequence.sampleRate;
    showStatus("Copied " + juce::String((double) (to - from) / audio.sequence.sampleRate, 2) + "s");
}

/** Runs @p transform over the selected samples and puts whatever it leaves
    (the same number, more, fewer or none) in their place.

    Only the selection is read, and only what the transform leaves is written:
    the rest of the clip stays in the blocks it's already in. That's what keeps
    an edit to a two-hour recording as quick as one to a three-second take. */
bool MainComponent::editSelection(
    const juce::String& label, bool snapToZeroCrossings,
    const std::function<void(std::vector<std::vector<float>>&, double sampleRate)>& transform)
{
    if (selectedAudioClip() == nullptr)
        return false;

    // For a destructive edit "no selection" must refuse rather than mean
    // "the whole clip" — a stray click before Cut would otherwise destroy
    // the take.
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
    if (! selectedClipRange(audio, from, to, snapToZeroCrossings) || to <= from)
    {
        showError("Select part of the clip first");
        return false;
    }

    auto selection = readClipAudio(audio, from, to);
    if (selection.empty())
    {
        showError("Could not read " + audio.file.getFileName());
        return false;
    }

    transform(selection, audio.sequence.sampleRate);
    return replaceClipAudio(label, audio, from, to, selection);
}

void MainComponent::cutAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Cut audio", true, [this](std::vector<std::vector<float>>& selection, double sampleRate)
        {
            // Lifted from the same samples that are about to be cut.
            audioClipboard_           = selection;
            audioClipboardSampleRate_ = sampleRate;

            for (auto& channel : selection)
                channel.clear();
        }))
        showStatus("Cut " + juce::String(range.lengthSeconds(), 2) + "s");
}

void MainComponent::deleteAudioSelection()
{
    const auto range = audioEditor_.selection();

    if (editSelection("Delete audio", true, [](std::vector<std::vector<float>>& selection, double)
        {
            for (auto& channel : selection)
                channel.clear();
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

    ClipAudio audio;
    if (! openSelectedClipAudio(audio))
    {
        if (selectedAudioClip() != nullptr)
            showError("Could not read that clip");
        return;
    }

    const double sampleRate = audio.sequence.sampleRate;
    const int    length     = audio.window.length();
    const int    wanted     = juce::jmax(0, (int) std::llround(atSeconds * sampleRate));
    const int    at         = juce::jmin(wanted, length);
    const int    until      = range.isEmpty()
                                  ? at
                                  : juce::jlimit(at, length, (int) std::llround(range.endSeconds * sampleRate));
    const double ratio      = audioClipboardSampleRate_ > 0.0 ? audioClipboardSampleRate_ / sampleRate : 1.0;

    std::vector<std::vector<float>> replacement((size_t) juce::jmax(1, audio.sequence.numChannels));
    for (int ch = 0; ch < (int) replacement.size(); ++ch)
    {
        // A cursor past the end of the clip is a request to paste *after* the
        // recording, so the gap is filled with silence rather than the paste
        // being dragged back to the last sample.
        auto& out = replacement[(size_t) ch];
        out.assign((size_t) (wanted - at), 0.0f);

        // A mono clipboard into a stereo clip (or the reverse) reuses the
        // last available channel rather than refusing — the same rule the
        // players follow for channel-count mismatches.
        const auto& source = audioClipboard_[(size_t) juce::jmin(ch, (int) audioClipboard_.size() - 1)];
        const auto  fitted = std::abs(ratio - 1.0) < 1.0e-9
                                 ? source
                                 : engine::audioedits::resample(source, ratio);
        out.insert(out.end(), fitted.begin(), fitted.end());
    }

    if (replaceClipAudio("Paste audio", audio, at, until, replacement))
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
    auto edges = zeroCrossingsNear({ range.startSeconds, range.endSeconds });
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
    if (editSelection("Silence audio", false, [](std::vector<std::vector<float>>& selection, double)
        {
            for (auto& channel : selection)
                channel = engine::audioedits::silenceRange(channel, 0, (int) channel.size());
        }))
        showStatus("Silenced");
}

void MainComponent::fadeInAudioSelection()
{
    if (editSelection("Fade in", false, [](std::vector<std::vector<float>>& selection, double)
        {
            for (auto& channel : selection)
                channel = engine::audioedits::fadeIn(channel, 0, (int) channel.size());
        }))
        showStatus("Faded in");
}

void MainComponent::fadeOutAudioSelection()
{
    if (editSelection("Fade out", false, [](std::vector<std::vector<float>>& selection, double)
        {
            for (auto& channel : selection)
                channel = engine::audioedits::fadeOut(channel, 0, (int) channel.size());
        }))
        showStatus("Faded out");
}

void MainComponent::reverseAudioSelection()
{
    if (editSelection("Reverse audio", true, [](std::vector<std::vector<float>>& selection, double)
        {
            for (auto& channel : selection)
                channel = engine::audioedits::reverseRange(channel, 0, (int) channel.size());
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
    const double at          = zeroCrossingsNear({ range.startSeconds })[0];

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
    // A time selection in the arrangement first, as the edit commands do;
    // otherwise the audio editor's selection.
    const auto isSelectedAudioTrack = [this](const model::Track& track)
    {
        return track.type == model::TrackType::Audio && timeSelection_.includes(track.id);
    };
    const auto& tracks          = history_.current().tracks;
    const bool  onTimeSelection = ! timeSelection_.isEmpty()
                               && std::any_of(tracks.begin(), tracks.end(), isSelectedAudioTrack);

    if (! onTimeSelection && (selectedAudioClip() == nullptr || audioEditor_.selection().isEmpty()))
    {
        showError("Select part of a clip in the audio editor, or time across audio tracks, first");
        return;
    }

    auto dialog = std::make_unique<ApplyEffectsDialog>();
    dialog->setSize(520, 460);

    dialog->setUserPresets(userEffectPresets_);
    dialog->setAvailablePlugins(engine_.pluginHost().knownPlugins());
    dialog->onPluginEditorRequested = [this](int slotIndex, const model::EffectSlot& slot)
    {
        openScratchPluginEditor(slotIndex, slot);
    };
    dialog->onChainAboutToChange = [this] { closeScratchPluginEditors(); };
    dialog->onPresetSaveRequested = [this](const model::EffectSlot& slot) { promptToSaveEffectPreset(slot); };
    dialog->onUserPresetDeleted   = [this](const std::string& effectId, const std::string& name)
    {
        deleteUserEffectPreset(effectId, name);
    };
    applyEffectsDialog_ = dialog.get();

    dialog->onPreview   = [this, onTimeSelection](const std::vector<model::EffectSlot>& chain)
    {
        if (onTimeSelection)
            previewEffectsOnTimeSelection(chain);
        else
            previewEffectsOnSelection(chain);
    };
    dialog->onDismissed = [safe = juce::Component::SafePointer<MainComponent>(this)]
    {
        if (safe != nullptr)
        {
            safe->engine_.stopAudition();
            safe->closeScratchPluginEditors();
        }
    };

    auto* raw = dialog.get();
    raw->onApply = [this, raw, onTimeSelection](const std::vector<model::EffectSlot>& chain)
    {
        engine_.stopAudition();
        if (onTimeSelection)
            applyEffectsToTimeSelection(chain);
        else
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

    if (enabledEffectCount(chain) == 0)
    {
        showError("Add an effect first");
        return;
    }

    ClipAudio audio;
    int       from = 0, to = 0;
    if (! openSelectedClipAudio(audio) || ! selectedClipRange(audio, from, to, false) || to <= from)
    {
        showError("Select part of the clip first");
        return;
    }

    const double sampleRate = audio.sequence.sampleRate;
    const int    count      = juce::jmin(to - from, (int) std::llround(kEffectPreviewSeconds * sampleRate));
    const auto   channels   = readClipAudio(audio, from, from + count);
    if (channels.empty())
    {
        showError("Could not read " + audio.file.getFileName());
        return;
    }

    const int numChannels = (int) channels.size();
    juce::AudioBuffer<float> block(numChannels, count);
    for (int ch = 0; ch < numChannels; ++ch)
        std::copy(channels[(size_t) ch].begin(), channels[(size_t) ch].end(), block.getWritePointer(ch));

    showBusy("Rendering preview...");
    if (const auto result = renderEffectChain(withScratchPluginStates(chain), block, sampleRate,
                                              history_.current().bpm, engine_.pluginHost());
        ! result.ok)
    {
        showError(result.error);
        return;
    }

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

    Rendered first, over a copy of the selection, and only then written: a
    plugin that won't load stops the whole apply before anything reaches
    disk, rather than leaving an undo step that changed nothing.

    The result is blended back over the original with a short crossfade at
    each boundary. Without it an effect that changes level — any compressor,
    or a reverb's wet mix — produces a step at the edges of the selection,
    heard as a click exactly where the edit begins and ends. A few
    milliseconds of blend removes it and is far too short to be heard as a
    fade. */
void MainComponent::applyEffectsToSelection(const std::vector<model::EffectSlot>& chain)
{
    if (audioEditor_.selection().isEmpty())
        return;

    if (enabledEffectCount(chain) == 0)
    {
        showError("Add an effect first");
        return;
    }

    ClipAudio audio;
    int       from = 0, to = 0;
    if (! openSelectedClipAudio(audio) || ! selectedClipRange(audio, from, to, false) || to <= from)
    {
        showError("Select part of the clip first");
        return;
    }

    const double sampleRate = audio.sequence.sampleRate;
    auto         channels   = readClipAudio(audio, from, to);
    if (channels.empty())
    {
        showError("Could not read " + audio.file.getFileName());
        return;
    }

    const int numChannels = (int) channels.size();
    juce::AudioBuffer<float> rendered(numChannels, to - from);
    for (int ch = 0; ch < numChannels; ++ch)
        std::copy(channels[(size_t) ch].begin(), channels[(size_t) ch].end(), rendered.getWritePointer(ch));

    showBusy("Applying effects...");
    if (const auto result = renderEffectChain(withScratchPluginStates(chain), rendered, sampleRate,
                                              history_.current().bpm, engine_.pluginHost());
        ! result.ok)
    {
        showError(result.error);
        return;
    }

    // Blended back over the original at both edges.
    const int count = juce::jmin(rendered.getNumSamples(), to - from);
    const int fade  = juce::jmin(count / 2, (int) std::llround(sampleRate * kEffectEdgeFadeSeconds));
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto*       destination = channels[(size_t) ch].data();
        const auto* processed   = rendered.getReadPointer(juce::jmin(ch, rendered.getNumChannels() - 1));

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

    if (replaceClipAudio("Apply effects", audio, from, to, channels))
        showStatus("Applied effects");
}

/** Opens an editor on a plugin slot of the Apply Effects dialog. That chain
    isn't playing on any track, so the editor gets an instance of its own,
    with the slot's state restored; its settings go back to the dialog when
    the window closes, and are read from it directly before a preview or an
    apply, so an editor left open still counts. */
void MainComponent::openScratchPluginEditor(int slotIndex, const model::EffectSlot& slot)
{
    for (auto& editor : scratchPluginEditors_)
    {
        if (editor->slotIndex == slotIndex)
        {
            editor->window->toFront(true);
            return;
        }
    }

    std::string  error;
    const double rate     = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    auto         instance = engine_.pluginHost().createInstance(pluginFormatName(slot.plugin.format),
                                                                slot.plugin.identifier, rate, 512, &error);
    if (instance == nullptr)
    {
        showError("That plugin isn't loaded on this machine");
        return;
    }

    juce::MemoryBlock state;
    if (! slot.plugin.state.empty() && state.fromBase64Encoding(juce::String(slot.plugin.state)) && state.getSize() > 0)
        instance->setStateInformation(state.getData(), (int) state.getSize());

    auto editor       = std::make_unique<ScratchPluginEditor>();
    editor->slotIndex = slotIndex;
    editor->instance  = std::move(instance);
    editor->window    = std::make_unique<PluginEditorWindow>(editor->instance->getName(), *editor->instance);
    editor->window->onCloseRequested = [this](PluginEditorWindow* window)
    {
        for (auto it = scratchPluginEditors_.begin(); it != scratchPluginEditors_.end(); ++it)
        {
            if ((*it)->window.get() != window)
                continue;

            if (applyEffectsDialog_ != nullptr)
                applyEffectsDialog_->setPluginState((*it)->slotIndex, pluginStateOf(*(*it)->instance));

            scratchPluginEditors_.erase(it);
            return;
        }
    };

    scratchPluginEditors_.push_back(std::move(editor));
}

/** @p chain with the current settings of any plugin being edited from the
    Apply Effects dialog. */
std::vector<model::EffectSlot> MainComponent::withScratchPluginStates(std::vector<model::EffectSlot> chain) const
{
    for (const auto& editor : scratchPluginEditors_)
        if (editor->slotIndex >= 0 && editor->slotIndex < (int) chain.size()
            && chain[(size_t) editor->slotIndex].kind == model::EffectKind::Plugin)
            chain[(size_t) editor->slotIndex].plugin.state = pluginStateOf(*editor->instance);

    return chain;
}

/** Closes the dialog's plugin editors, handing their settings back first.
    Called before its chain is reordered or shortened, while each editor's
    slot index still names its slot, and when the dialog goes away. */
void MainComponent::closeScratchPluginEditors()
{
    if (applyEffectsDialog_ != nullptr)
        for (const auto& editor : scratchPluginEditors_)
            applyEffectsDialog_->setPluginState(editor->slotIndex, pluginStateOf(*editor->instance));

    scratchPluginEditors_.clear();
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

    ClipAudio audio;
    if (! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError("Could not read that clip");
        return;
    }

    const double sampleRate = audio.sequence.sampleRate;
    const auto   range      = audioEditor_.selection();
    const int    length     = audio.window.length();
    const int    from       = range.isEmpty() ? 0
                                : juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
    const int    to         = range.isEmpty() ? length
                                : juce::jlimit(from, length, (int) std::llround(range.endSeconds * sampleRate));

    const auto channels = readClipAudio(audio, from, to);
    if (channels.empty())
    {
        showError("Could not read that clip");
        return;
    }

    // Channel 0 rather than a sum: summing a stereo pair cancels whatever is
    // out of phase between them, which would hide exactly the kind of problem
    // someone opens an analyser to find.
    const auto& passage = channels[0];

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

    const bool applied = editWholeClip("Speed and pitch",
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
    ClipAudio                       audio;
    int                             from = 0, to = 0;
    std::vector<std::vector<float>> channels;

    if (openSelectedClipAudio(audio) && selectedClipRange(audio, from, to, false))
        channels = readClipAudio(audio, from, to);

    if (channels.empty())
    {
        showError("Could not read " + file.getFileName());
        return;
    }

    std::vector<engine::NoiseProfile> profiles;
    for (const auto& passage : channels)
        profiles.push_back(engine::noisereduction::captureNoiseProfile(passage));

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
    const bool applied = editWholeClip("Reduce noise",
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

/** The editor's selection as samples from the clip's start. False when
    there's no selection, which for a destructive edit must refuse rather than
    quietly mean "the whole clip": a stray click before Cut would otherwise
    destroy the take. */
bool MainComponent::selectedClipRange(const ClipAudio& audio, int& fromOut, int& toOut,
                                      bool snapToZeroCrossings) const
{
    const auto range = audioEditor_.selection();
    if (range.isEmpty())
        return false;

    const double sampleRate = audio.sequence.sampleRate;
    const int    length     = audio.window.length();
    fromOut = juce::jlimit(0, length, (int) std::llround(range.startSeconds * sampleRate));
    toOut   = juce::jlimit(0, length, (int) std::llround(range.endSeconds * sampleRate));

    if (snapToZeroCrossings)
    {
        fromOut = zeroCrossingNear(audio, fromOut);
        toOut   = zeroCrossingNear(audio, toOut);
    }

    if (toOut < fromOut)
        std::swap(fromOut, toOut);

    return true;
}

bool MainComponent::editWholeClip(
    const juce::String& label,
    const std::function<void(std::vector<std::vector<float>>&, double sampleRate)>& transform)
{
    if (selectedAudioClip() == nullptr)
        return false;

    ClipAudio                       audio;
    std::vector<std::vector<float>> channels;
    if (openSelectedClipAudio(audio))
        channels = readClipAudio(audio, 0, audio.window.length());

    if (channels.empty() || channels[0].empty())
    {
        showError("Could not read that clip");
        return false;
    }

    transform(channels, audio.sequence.sampleRate);
    return replaceClipAudio(label, audio, 0, audio.window.length(), channels);
}

/** Writes a new sequence file that plays @p sequence (the audio of
    @p source) with frames [@p from, @p to) replaced by @p replacement, which
    may be any length: new blocks for the replacement, and the rest shared
    with what's already there. Returns the new file, or nothing, having said
    why, if it couldn't be written. Nothing already on disk is touched.

    Channels can differ in length only if a transform is inconsistent, which
    is a bug — but reading past one would be a crash, so each is padded or cut
    to the first channel's length. */
std::optional<juce::File> MainComponent::writeEditedSequence(const juce::File& source,
                                                             const engine::sequence::SampleSequence& sequence,
                                                             std::int64_t from, std::int64_t to,
                                                             const std::vector<std::vector<float>>& replacement)
{
    const int newFrames   = replacement.empty() ? 0 : (int) replacement[0].size();
    const int numChannels = juce::jmax(1, sequence.numChannels);

    juce::AudioBuffer<float> buffer(numChannels, newFrames);
    buffer.clear();
    for (int ch = 0; ch < numChannels && newFrames > 0; ++ch)
    {
        const auto& channel = replacement[(size_t) juce::jmin(ch, (int) replacement.size() - 1)];
        std::copy_n(channel.data(), juce::jmin(newFrames, (int) channel.size()), buffer.getWritePointer(ch));
    }

    const auto folder = audioDirectoryFor(editsDirectory());
    const auto stem   = source.getFileNameWithoutExtension();

    std::vector<engine::sequence::Span> blocks;
    if (newFrames > 0)
    {
        auto written = engine::sequencefile::writeBlocks(folder, stem, buffer, sequence.sampleRate);
        if (! written)
        {
            showError("Could not write the edited audio into " + folder.getFullPathName());
            return std::nullopt;
        }
        blocks = std::move(*written);
    }

    const auto edited      = engine::sequence::replaced(sequence, from, to, blocks);
    const auto destination = folder.getNonexistentChildFile(stem, engine::sequencefile::kExtension);
    if (! engine::sequencefile::save(destination, edited))
    {
        for (const auto& block : blocks)
            engine::sequencefile::fileFromPath(block.file).deleteFile();

        showError("Could not write " + destination.getFileName());
        return std::nullopt;
    }

    return destination;
}

/** The one path every destructive edit takes.

    Nothing already on disk is rewritten. The replacement goes into new block
    files, and a new sequence file plays the audio before the range from
    wherever it already was, then the new blocks, then the audio after it.
    Undo points the clip back at the previous sequence, whose files are all
    untouched. The audio outside the clip's window (hidden by a trim or split)
    is kept the same way, so its offset still points at the same place.

    Centralised because each step is easy to forget individually and each
    failure is quiet: a clip whose lengthBeats isn't updated plays the old
    duration, and a stale waveform-peaks cache draws the old audio over the
    new. */
bool MainComponent::replaceClipAudio(const juce::String& label, const ClipAudio& audio, int from, int to,
                                     const std::vector<std::vector<float>>& replacement)
{
    const int    length     = audio.window.length();
    const double sampleRate = audio.sequence.sampleRate;
    from = juce::jlimit(0, length, from);
    to   = juce::jlimit(from, length, to);

    const int newFrames = replacement.empty() ? 0 : (int) replacement[0].size();
    const int newLength = length - (to - from) + newFrames;
    if (newLength <= 0)
    {
        // Editing a clip down to nothing would leave a clip with no audio,
        // which plays as silence with no explanation.
        showError("That would leave the clip empty");
        return false;
    }

    const auto written = writeEditedSequence(audio.file, audio.sequence, (std::int64_t) audio.window.start + from,
                                             (std::int64_t) audio.window.start + to, replacement);
    if (! written)
        return false;

    const auto destination = *written;

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
