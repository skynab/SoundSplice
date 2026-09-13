#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>

namespace looper::app
{
/**
    Runs a long render on a background thread behind a progress window.

    Offline rendering used to happen on the message thread, which froze the app
    for the whole export - no progress, no cancel, and on a long project no way
    to tell it apart from a hang. Stem export made that up to eight renders back
    to back.

    Moving the loop off the message thread is safe because
    AudioEngine::renderOffline already suspends the audio device for its
    duration: the mixer has exactly one writer by design, and this moves which
    thread holds that writership rather than adding a second one. **The caller
    is still responsible for keeping the message thread out of the engine while
    a job runs** - see MainComponent's offlineRenderInProgress_, and note that
    its timer frees retired audio objects on every tick.

    Deliberately generic rather than export-shaped: it knows about progress and
    cancellation, not about files or formats.

    `launchThread` puts the window into a modal state *without* a modal loop, so
    this needs no JUCE_MODAL_LOOPS_PERMITTED (which the app target does not
    define). Input to the rest of the UI is blocked while the message loop keeps
    running normally.
*/
class OfflineRenderJob final : public juce::ThreadWithProgressWindow
{
public:
    /** Runs on the background thread. */
    using Work = std::function<void (OfflineRenderJob&)>;

    /** Starts the job and shows its window. Ownership is returned to the
        caller so it can be stopped on shutdown - quitting mid-export must not
        leave a thread running in an engine that is being torn down.

        @p onFinished runs on the message thread, with whether the user
        cancelled. */
    static std::unique_ptr<OfflineRenderJob> launch(const juce::String& title,
                                                    Work work,
                                                    std::function<void (bool cancelled)> onFinished)
    {
        auto job = std::unique_ptr<OfflineRenderJob>(
            new OfflineRenderJob(title, std::move(work), std::move(onFinished)));

        job->launchThread();
        return job;
    }

    /** Background thread: true once the user has pressed Cancel, or the job is
        being stopped. Work should return promptly when this goes true. */
    bool shouldAbort() const { return threadShouldExit(); }

    /** Background thread: reports progress in 0..1 and what is happening. */
    void report(double fraction, const juce::String& what)
    {
        setProgress(juce::jlimit(0.0, 1.0, fraction));
        setStatusMessage(what);
    }

    void run() override
    {
        if (work_)
            work_(*this);
    }

    void threadComplete(bool userPressedCancel) override
    {
        // Posted rather than run inline: the owner's completion handler is very
        // likely to release this job, and deleting an object from inside its
        // own callback is a good way to crash on the way out of it.
        if (auto finished = std::move(onFinished_))
            juce::MessageManager::callAsync([finished, userPressedCancel]
                                            { finished(userPressedCancel); });
    }

private:
    OfflineRenderJob(const juce::String& title, Work work,
                     std::function<void (bool)> onFinished)
        : juce::ThreadWithProgressWindow(title, true, true), // has progress bar, can cancel
          work_(std::move(work)),
          onFinished_(std::move(onFinished))
    {
    }

    Work                       work_;
    std::function<void (bool)> onFinished_;
};

} // namespace looper::app
