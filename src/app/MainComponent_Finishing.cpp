#include "MainComponentInternal.h"

#include "engine/Diagnostics.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// Audition's finishing workflows: the Diagnostics pane - scan a clip for what's
// wrong with it, then select or fix each problem.

namespace soundsplice
{
namespace
{
    /** A click's stretch widened a little, so the repair has its
        surroundings to work from. */
    constexpr double kClickMarginSeconds = 0.005;
}

/** Scans the selected audio clip (engine/Diagnostics.h) and fills the
    Diagnostics pane with what it found. */
void MainComponent::runDiagnostics()
{
    const auto* clip = selectedAudioClip();
    ClipAudio   audio;
    if (clip == nullptr || clip->warp || ! openSelectedClipAudio(audio) || audio.window.isEmpty())
    {
        showError(clip != nullptr && clip->warp ? "This clip is warped - turn Warp off to diagnose its audio"
                                                : "Select an audio clip first");
        return;
    }

    struct DiagnosticScan final : ClipScan
    {
        engine::diagnostics::Scanner scanner;
        std::vector<engine::diagnostics::Issue> issues;

        void prepare(double sampleRate) override { scanner.prepare(sampleRate, {}); }
        void process(const float* const* outputs, int frames) override { scanner.process(outputs, 2, frames); }
    };

    auto       scan   = std::make_shared<DiagnosticScan>();
    const int  clipId = clip->id;
    const auto name   = juce::File(clip->audioFile).getFileNameWithoutExtension();

    workspace_.revealPanel("Diagnostics");
    scanClipAudio("Diagnostics", "Looking for problems", audio, 0, audio.window.length(), scan,
                  [this, scan, clipId, name](double rate)
    {
        std::vector<DiagnosticsPane::Row> rows;
        for (const auto& issue : scan->scanner.finish())
            rows.push_back({ issue, (double) issue.from / rate, (double) issue.to / rate });

        diagnosedClipId_ = clipId;
        diagnosticsPane_.setRows(std::move(rows), name);
        showStatus("Diagnostics: " + juce::String((int) diagnosticsPane_.rows().size()) + " found in " + name);
    });
}

/** The row's stretch, selected in the audio editor - of the clip it was
    found in, which is selected again if need be. */
void MainComponent::selectDiagnostic(const DiagnosticsPane::Row& row)
{
    const auto where = app::OpenFiles::locate(history_.current(), diagnosedClipId_);
    if (! where.isValid())
    {
        showError("That clip has gone - Scan again");
        return;
    }
    if (where.track != selectedTrackIndex_ || where.clip != selectedClipIndex_)
        selectTrackAndClip(where.track, where.clip);

    workspace_.revealPanel("Audio");
    const double margin = row.issue.kind == engine::diagnostics::Kind::Click ? kClickMarginSeconds : 0.0;
    audioEditor_.selectRange({ juce::jmax(0.0, row.fromSeconds - margin), row.toSeconds + margin });
}

/** Repairs [range] of the selected clip as @p kind needs: clicks filled,
    clipped peaks redrawn, silence taken out, DC offset removed. */
bool MainComponent::fixDiagnosticRange(engine::diagnostics::Kind kind, AudioRange range)
{
    audioEditor_.selectRange(range);
    switch (kind)
    {
        case engine::diagnostics::Kind::Click:    removeClicksInSelection(8.0, 2.0); return true;
        case engine::diagnostics::Kind::Clipping: fixClippingInSelection(95.0, 0.0); return true;
        case engine::diagnostics::Kind::Silence:  deleteAudioSelection(); return true;
        case engine::diagnostics::Kind::DcOffset: removeDcOffsetInSelection(); return true;
    }
    return false;
}

/** Fixes one problem, then scans again: a fix moves what comes after it. */
void MainComponent::fixDiagnostic(const DiagnosticsPane::Row& row)
{
    selectDiagnostic(row);
    if (selectedAudioClip() == nullptr || selectedAudioClip()->id != diagnosedClipId_)
        return;

    const double margin = row.issue.kind == engine::diagnostics::Kind::Click ? kClickMarginSeconds : 0.0;
    const double clipEnd = row.issue.kind == engine::diagnostics::Kind::DcOffset ? row.toSeconds : row.toSeconds + margin;
    fixDiagnosticRange(row.issue.kind, { juce::jmax(0.0, row.fromSeconds - margin), clipEnd });
    runDiagnostics();
}

/** Fixes every problem of @p kind. Clicks, clipping and DC offset in one
    pass over the whole clip; silences one at a time from the last, so
    taking one out doesn't move the ones still to go. */
void MainComponent::fixAllDiagnostics(engine::diagnostics::Kind kind)
{
    std::vector<DiagnosticsPane::Row> rows;
    for (const auto& row : diagnosticsPane_.rows())
        if (row.issue.kind == kind)
            rows.push_back(row);
    if (rows.empty())
        return;

    selectDiagnostic(rows.front());
    if (selectedAudioClip() == nullptr || selectedAudioClip()->id != diagnosedClipId_)
        return;

    if (kind == engine::diagnostics::Kind::Silence)
    {
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            fixDiagnosticRange(kind, { it->fromSeconds, it->toSeconds });
    }
    else
    {
        // From the first to the last: the repairs only touch what they find.
        const double from = rows.front().fromSeconds;
        double       to   = rows.front().toSeconds;
        for (const auto& row : rows)
            to = juce::jmax(to, row.toSeconds);
        fixDiagnosticRange(kind, { juce::jmax(0.0, from - kClickMarginSeconds), to + kClickMarginSeconds });
    }
    runDiagnostics();
}

/** Takes each channel's mean out of the audio editor's selection. */
void MainComponent::removeDcOffsetInSelection()
{
    if (editSelection("Remove DC offset", false, [](std::vector<std::vector<float>>& channels, double)
        {
            for (auto& channel : channels)
            {
                if (channel.empty())
                    continue;
                double sum = 0.0;
                for (const float x : channel)
                    sum += x;
                const auto mean = (float) (sum / (double) channel.size());
                for (auto& x : channel)
                    x -= mean;
            }
        }))
        showStatus("Removed the DC offset");
}

} // namespace soundsplice
