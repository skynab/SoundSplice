#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

#include "ExportChoices.h"

namespace soundsplice
{
/**
    File > Render Queue (REAPER's): the exports queued with Export Audio's
    Add to Render Queue, rendered one after another by Render All.

    Each job is a snapshot of the project as it was when it was queued, so
    the queue renders what was asked for even after the project has moved
    on. They're rendered by the app's own headless render in a child process
    (`SoundSplice --render-job`), so the window - and the app - stay usable
    while they run. Closing the window stops the run.
*/
class RenderQueueDialog final : public juce::Component,
                                private juce::ListBoxModel
{
public:
    /** A run finished: the jobs (by index, as they were) that rendered,
        which the dialog has already taken off its list - the owner takes
        them off the queue. Failed ones stay, with what went wrong. */
    std::function<void(const std::vector<int>& rendered)> onRunFinished;
    std::function<void(int index)>                        onRemove;

    /** Runs a job and says how it went: the owner supplies the process. */
    using Runner = std::function<bool(const app::exportchoices::Job&, juce::String& report, std::function<bool()> shouldStop)>;

    explicit RenderQueueDialog(Runner runner) : runner_(std::move(runner))
    {
        list_.setModel(this);
        list_.setRowHeight(40);
        list_.setTitle("Render queue");
        addAndMakeVisible(list_);

        renderButton_.onClick = [this] { renderAll(); };
        removeButton_.onClick = [this]
        {
            const int row = list_.getSelectedRow();
            if (row >= 0 && ! running() && onRemove)
                onRemove(row);
        };
        stopButton_.onClick = [this] { stop(); };
        for (auto* button : { &renderButton_, &removeButton_, &stopButton_ })
            addAndMakeVisible(*button);
        addAndMakeVisible(status_);
        setSize(560, 380);
        updateButtons();
    }

    ~RenderQueueDialog() override { stop(); }

    void setJobs(std::vector<app::exportchoices::Job> jobs)
    {
        jobs_ = std::move(jobs);
        states_.assign(jobs_.size(), {});
        list_.updateContent();
        list_.repaint();
        updateButtons();
    }

    /** The queue changed: shown at once. During a run only new jobs can be
        added (Remove waits for it), so they're added at the end, waiting
        for the next run, and the ones being rendered keep their states. */
    void updateJobs(const std::vector<app::exportchoices::Job>& jobs)
    {
        if (! running())
        {
            setJobs(jobs);
            return;
        }
        for (size_t i = jobs_.size(); i < jobs.size(); ++i)
        {
            jobs_.push_back(jobs[i]);
            states_.push_back({});
        }
        list_.updateContent();
        list_.repaint();
    }

    bool running() const { return worker_ != nullptr && worker_->isThreadRunning(); }

    /** What it lists, in order. */
    const std::vector<app::exportchoices::Job>& jobs() const noexcept { return jobs_; }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto bottom = area.removeFromBottom(28);
        renderButton_.setBounds(bottom.removeFromRight(100));
        bottom.removeFromRight(6);
        stopButton_.setBounds(bottom.removeFromRight(70));
        bottom.removeFromRight(6);
        removeButton_.setBounds(bottom.removeFromLeft(90));
        bottom.removeFromLeft(8);
        status_.setBounds(bottom);
        area.removeFromBottom(6);
        list_.setBounds(area);
    }

    void paint(juce::Graphics& g) override { g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId)); }

private:
    struct State
    {
        juce::String text = "Waiting";
        bool         done = false, failed = false;
    };

    /** Runs the jobs on its own thread. */
    struct Worker final : juce::Thread
    {
        RenderQueueDialog&                   owner;
        std::vector<app::exportchoices::Job> jobs;

        Worker(RenderQueueDialog& o, std::vector<app::exportchoices::Job> j)
            : juce::Thread("Render queue"), owner(o), jobs(std::move(j)) {}

        void run() override
        {
            juce::Component::SafePointer<RenderQueueDialog> safe(&owner);
            for (int i = 0; i < (int) jobs.size() && ! threadShouldExit(); ++i)
            {
                juce::MessageManager::callAsync([safe, i]
                {
                    if (safe != nullptr)
                        safe->setState(i, "Rendering...", false, false);
                });

                juce::String report;
                const bool   ok = owner.runner_(jobs[(size_t) i], report, [this] { return threadShouldExit(); });
                const bool   stopped = threadShouldExit();
                juce::MessageManager::callAsync([safe, i, ok, report, stopped]
                {
                    if (safe == nullptr)
                        return;
                    safe->setState(i, stopped ? juce::String("Stopped") : ok ? juce::String("Done") : "Failed: " + report.trim(),
                                   ! stopped, ! ok || stopped);
                });
            }
            juce::MessageManager::callAsync([safe]
            {
                if (safe != nullptr)
                    safe->finished();
            });
        }
    };

    void renderAll()
    {
        if (running() || jobs_.empty())
            return;
        states_.assign(jobs_.size(), {});
        worker_ = std::make_unique<Worker>(*this, jobs_);
        worker_->startThread();
        status_.setText("Rendering " + juce::String((int) jobs_.size()) + (jobs_.size() == 1 ? " job" : " jobs") + "...",
                        juce::dontSendNotification);
        updateButtons();
    }

    void stop()
    {
        if (worker_ != nullptr)
            worker_->stopThread(10000);
        updateButtons();
    }

    void finished()
    {
        int done = 0, failed = 0;
        for (const auto& state : states_)
        {
            done += state.done && ! state.failed ? 1 : 0;
            failed += state.failed ? 1 : 0;
        }
        // The rendered ones come off; the rest keep what happened to them.
        std::vector<int> rendered;
        for (int i = (int) states_.size() - 1; i >= 0; --i)
            if (states_[(size_t) i].done && ! states_[(size_t) i].failed)
            {
                rendered.insert(rendered.begin(), i);
                jobs_.erase(jobs_.begin() + i);
                states_.erase(states_.begin() + i);
            }
        list_.updateContent();
        list_.repaint();

        status_.setText(juce::String(done) + " rendered" + (failed > 0 ? ", " + juce::String(failed) + " not (still queued)" : juce::String()),
                        juce::dontSendNotification);
        juce::AccessibilityHandler::postAnnouncement(status_.getText(), juce::AccessibilityHandler::AnnouncementPriority::medium);
        updateButtons();
        if (onRunFinished)
            onRunFinished(rendered);
    }

    void setState(int index, const juce::String& text, bool done, bool failed)
    {
        if (index < 0 || index >= (int) states_.size())
            return;
        states_[(size_t) index] = { text, done, failed };
        list_.repaintRow(index);
    }

    void updateButtons()
    {
        renderButton_.setEnabled(! running() && ! jobs_.empty());
        removeButton_.setEnabled(! running() && ! jobs_.empty());
        stopButton_.setEnabled(running());
        if (jobs_.empty() && ! running() && status_.getText().isEmpty())
            status_.setText("Nothing queued: Export Audio > Add to Render Queue", juce::dontSendNotification);
    }

    int getNumRows() override { return (int) jobs_.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) jobs_.size())
            return;
        if (selected)
            g.fillAll(juce::Colours::steelblue.withAlpha(0.4f));
        const auto& job   = jobs_[(size_t) row];
        const auto& state = states_[(size_t) row];
        g.setColour(theme::colour(*this, theme::textId));
        g.setFont(juce::FontOptions(14.0f));
        g.drawText(job.label, 8, 2, width - 16, height / 2, juce::Justification::centredLeft, true);
        g.setFont(juce::FontOptions(12.0f));
        g.setColour(state.failed ? theme::colour(*this, theme::dangerTextId) : theme::colour(*this, theme::textMutedId));
        g.drawText(engine::displayNameFor(job.choice.options.format) + "  -  " + state.text, 8, height / 2, width - 16,
                   height / 2 - 2, juce::Justification::centredLeft, true);
    }

    juce::String getNameForRow(int row) override
    {
        return row >= 0 && row < (int) jobs_.size() ? jobs_[(size_t) row].label + ", " + states_[(size_t) row].text : juce::String();
    }

    Runner                                runner_;
    std::vector<app::exportchoices::Job>  jobs_;
    std::vector<State>                    states_;
    std::unique_ptr<Worker>               worker_;
    juce::ListBox                         list_;
    juce::TextButton                      renderButton_ { "Render All" }, removeButton_ { "Remove" }, stopButton_ { "Stop" };
    juce::Label                           status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderQueueDialog)
};

} // namespace soundsplice
