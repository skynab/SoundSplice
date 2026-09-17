#include "MainComponentInternal.h"

#include "engine/AmplitudeAnalysis.h"
#include "engine/ClipChannels.h"
#include "model/Markers.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The Analyze menu's scans of a clip: amplitude statistics, Find Clipping and
// Label Sounds, and the render-thread scan they (and Measure Loudness) share.
// The analysis itself is engine/AmplitudeAnalysis.h.

namespace soundsplice
{
namespace
{
    juce::String level(double db, const char* unit)
    {
        return std::isfinite(db) ? juce::String(db, 1) + " " + unit : juce::String("-inf ") + unit;
    }

    struct StatisticsScan final : MainComponent::ClipScan
    {
        engine::AmplitudeStatistics stats;

        void prepare(double sampleRate) override { stats.prepare(sampleRate, 2); }
        void process(const float* const* outputs, int frames) override { stats.process(outputs, 2, frames); }
    };

    struct ClippingScan final : MainComponent::ClipScan
    {
        engine::ClippingDetector detector;

        void prepare(double) override {}
        void process(const float* const* outputs, int frames) override { detector.process(outputs, 2, frames); }
    };

    struct SilenceScan final : MainComponent::ClipScan
    {
        std::unique_ptr<engine::silence::PeakEnvelope> envelope;

        void prepare(double sampleRate) override
        {
            envelope = std::make_unique<engine::silence::PeakEnvelope>(juce::jmax(1, (int) std::llround(sampleRate * 0.01)));
        }
        void process(const float* const* outputs, int frames) override { envelope->append(outputs, 2, frames); }
    };
}

/** Reads samples [from, to) of the selected clip on the render thread and
    hands them to @p scan as the clip plays them: two outputs, each carrying
    the channel the clip's channel setting gives it, so a mono file counts on
    both sides as it's heard. The clip's gain isn't applied. @p onScanned runs
    on the message thread, with the file's rate, unless the job was cancelled
    or the audio couldn't be read. */
void MainComponent::scanClipAudio(const juce::String& title, const juce::String& activity, const ClipAudio& audio,
                                  int from, int to, std::shared_ptr<ClipScan> scan,
                                  std::function<void(double sampleRate)> onScanned)
{
    if (renderJob_ != nullptr)
    {
        showError("A render is already running");
        return;
    }

    const auto* clip = selectedAudioClip();
    if (clip == nullptr)
        return;

    const auto file     = audio.file;
    const auto start    = (std::int64_t) audio.window.start + from;
    const auto count    = (std::int64_t) juce::jmax(0, to - from);
    const auto channels = clip->channels;
    auto       rate     = std::make_shared<double>(0.0);

    auto work = [file, start, count, channels, scan, rate, activity](app::OfflineRenderJob& job)
    {
        juce::AudioFormatManager formats;
        engine::sequencefile::registerFormats(formats);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0)
            return;

        const int fileChannels = juce::jmax(1, (int) reader->numChannels);
        scan->prepare(reader->sampleRate);

        constexpr int            kChunk = 1 << 16;
        juce::AudioBuffer<float> buffer(fileChannels, kChunk);

        for (std::int64_t done = 0; done < count; done += kChunk)
        {
            if (job.shouldAbort())
                return;

            const int n = (int) juce::jmin<std::int64_t>(kChunk, count - done);
            if (! reader->read(buffer.getArrayOfWritePointers(), fileChannels, start + done, n))
                return;

            const float* outputs[2] {
                buffer.getReadPointer(engine::sourceChannelFor(channels, 0, fileChannels)),
                buffer.getReadPointer(engine::sourceChannelFor(channels, 1, fileChannels)),
            };
            scan->process(outputs, n);

            job.report((double) (done + n) / (double) count, activity);
        }

        *rate = reader->sampleRate;
    };

    auto onFinished = [self = juce::Component::SafePointer<MainComponent>(this), rate, title,
                       onScanned = std::move(onScanned)](bool cancelled)
    {
        if (self == nullptr)
            return;

        self->renderJob_.reset();
        if (cancelled)
        {
            self->showStatus(title + " cancelled");
            return;
        }
        if (*rate <= 0.0)
        {
            self->showError("Could not read that clip");
            return;
        }
        onScanned(*rate);
    };

    renderJob_ = app::OfflineRenderJob::launch(title, std::move(work), std::move(onFinished));
}

/** The audio editor's selection in the selected clip, or the whole clip.
    False, having said why, if there's no audio clip to read. */
bool MainComponent::selectedScanRange(ClipAudio& audio, int& from, int& to, bool& whole)
{
    if (selectedAudioClip() == nullptr || ! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError("Select an audio clip first");
        return false;
    }

    from  = 0;
    to    = audio.window.length();
    whole = audioEditor_.selection().isEmpty();
    if (! whole)
        selectedClipRange(audio, from, to, false);
    return to > from;
}

void MainComponent::showAmplitudeStatistics()
{
    ClipAudio audio;
    int       from = 0, to = 0;
    bool      whole = true;
    if (! selectedScanRange(audio, from, to, whole))
        return;

    const double gainDb = selectedAudioClip()->gainDb;
    auto         scan   = std::make_shared<StatisticsScan>();

    scanClipAudio("Amplitude Statistics", "Measuring levels", audio, from, to, scan, [this, scan, gainDb, whole](double rate)
    {
        const auto r = scan->stats.report();

        // As heard: the clip's gain moves every level, not the offset's share
        // of full scale or the range.
        const auto heard = [gainDb](double db) { return db + gainDb; };

        juce::String text;
        text << (whole ? "The whole clip" : "The selection") << ", " << juce::String((double) r.frames / rate, 2) << " s"
             << (std::abs(gainDb) > 0.001f ? " (with its " + juce::String(gainDb, 1) + " dB gain)" : juce::String()) << "\n\n"
             << "Peak:  " << level(heard(r.peakDb), "dBFS")
             << "   (left " << level(heard(r.peakLeftDb), "dB") << ", right " << level(heard(r.peakRightDb), "dB") << ")\n"
             << "RMS:  " << level(heard(r.rmsDb), "dBFS") << "\n"
             << "Loudest 50 ms:  " << level(heard(r.loudestWindowDb), "dB RMS") << "\n"
             << "Quietest 50 ms:  " << level(heard(r.quietestWindowDb), "dB RMS") << "\n"
             << "Dynamic range:  " << juce::String(r.dynamicRangeDb, 1) << " dB\n"
             << "DC offset:  " << juce::String(r.dcOffsetPercent, 3) << " %";

        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Amplitude Statistics", text, "OK", this);
    });
}

/** Adds a marker range, named "Clipped", over each run of clipped samples in
    the selection or clip, as Audacity labels them. One undo step. */
void MainComponent::findClipping()
{
    ClipAudio audio;
    int       from = 0, to = 0;
    bool      whole = true;
    if (! selectedScanRange(audio, from, to, whole))
        return;

    const int clipId = selectedAudioClip()->id;
    auto      scan   = std::make_shared<ClippingScan>();

    scanClipAudio("Find Clipping", "Looking for clipping", audio, from, to, scan, [this, scan, clipId, from](double rate)
    {
        const auto& runs = scan->detector.finish();
        if (runs.empty())
        {
            showStatus("No clipping found");
            return;
        }
        addMarkerRangesInClip(clipId, runs, from, rate, "Clipped", false, "Find clipping");
        showStatus("Found " + juce::String((int) runs.size()) + (runs.size() == 1 ? " clipped run" : " clipped runs")
                   + " - marked on the timeline");
    });
}

void MainComponent::showLabelSoundsDialog()
{
    if (selectedAudioClip() == nullptr)
    {
        showError("Select an audio clip first");
        return;
    }

    auto* window = new juce::AlertWindow("Label Sounds",
                                         "Adds a marker range over each sound in the selection or clip: "
                                         "whatever lies between the silences.",
                                         juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("threshold", juce::String(settings_.getDoubleValue("labelSounds.threshold", -40.0)),
                          "Silent below (dB):");
    window->addTextEditor("silence", juce::String(settings_.getDoubleValue("labelSounds.silence", 0.5)),
                          "Silences last at least (seconds):");
    window->addTextEditor("sound", juce::String(settings_.getDoubleValue("labelSounds.sound", 0.1)),
                          "Sounds last at least (seconds):");
    window->addButton("Label", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [self = juce::Component::SafePointer<MainComponent>(this), window](int result)
        {
            std::unique_ptr<juce::AlertWindow> owned(window);
            if (self == nullptr || result != 1)
                return;

            const double threshold = juce::jlimit(-120.0, 0.0, window->getTextEditorContents("threshold").getDoubleValue());
            const double silence   = juce::jlimit(0.01, 60.0, window->getTextEditorContents("silence").getDoubleValue());
            const double sound     = juce::jlimit(0.0, 3600.0, window->getTextEditorContents("sound").getDoubleValue());

            self->settings_.setValue("labelSounds.threshold", threshold);
            self->settings_.setValue("labelSounds.silence", silence);
            self->settings_.setValue("labelSounds.sound", sound);
            self->labelSounds((float) threshold, silence, sound);
        }));
}

/** Marks each sound between silences, as Audacity's Label Sounds does: the
    silence scan Detach at Silences uses, turned inside out. */
void MainComponent::labelSounds(float thresholdDb, double minSilenceSeconds, double minSoundSeconds)
{
    ClipAudio audio;
    int       from = 0, to = 0;
    bool      whole = true;
    if (! selectedScanRange(audio, from, to, whole))
        return;

    const int clipId = selectedAudioClip()->id;
    auto      scan   = std::make_shared<SilenceScan>();

    scanClipAudio("Label Sounds", "Finding sounds", audio, from, to, scan,
                  [this, scan, clipId, from, to, thresholdDb, minSilenceSeconds, minSoundSeconds](double rate)
    {
        auto&       envelope = *scan->envelope;
        const auto  silences = engine::silence::silentRuns(envelope.finish(), envelope.windowFrames(), to - from,
                                                           engine::silence::gainForDecibels(thresholdDb),
                                                           std::llround(minSilenceSeconds * rate));
        const auto  sounds   = engine::silence::soundRuns(silences, to - from, std::llround(minSoundSeconds * rate));

        if (sounds.empty())
        {
            showStatus("No sounds found above " + juce::String(thresholdDb, 1) + " dB");
            return;
        }
        addMarkerRangesInClip(clipId, sounds, from, rate, "Sound", true, "Label sounds");
        showStatus("Labelled " + juce::String((int) sounds.size()) + (sounds.size() == 1 ? " sound" : " sounds"));
    });
}

/** Adds a marker range over each of @p runs, frames counted from @p offset
    into clip @p clipId, as one undo step named @p label. Each is named
    @p name, numbered from 1 when @p numbered. */
void MainComponent::addMarkerRangesInClip(int clipId, const std::vector<engine::silence::FrameRange>& runs, int offset,
                                          double sampleRate, const juce::String& name, bool numbered,
                                          const juce::String& label)
{
    const auto where = app::OpenFiles::locate(history_.current(), clipId);
    if (! where.isValid() || sampleRate <= 0.0)
    {
        showError("The clip was removed while it was being read");
        return;
    }

    const auto& clip = history_.current().tracks[(size_t) where.track].clips[(size_t) where.clip];
    const double start = clip.startBeats;
    const double bpm   = history_.current().bpm;

    history_.edit(label.toStdString(), [&](model::Song& s)
    {
        int number = 1;
        for (const auto& run : runs)
        {
            const double fromSeconds = (double) (offset + run.from) / sampleRate;
            const double toSeconds   = (double) (offset + run.to) / sampleRate;
            const auto   text        = numbered ? name + " " + juce::String(number++) : name;
            model::addMarker(s, start + engine::beatsForSeconds(fromSeconds, bpm),
                             engine::beatsForSeconds(toSeconds - fromSeconds, bpm), text.toStdString());
        }
    });

    arrangementView_.setSong(history_.current());
}

} // namespace soundsplice
