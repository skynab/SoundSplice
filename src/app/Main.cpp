#include <juce_audio_utils/juce_audio_utils.h>

#include "DarkTitleBar.h"
#include "MainComponent.h"
#include "PluginProbe.h"

namespace soundsplice
{
/** `SoundSplice --render <project> <out> <rate> <bits> <contents> <report> [lufs]`:
    renders a project with no window and quits. soundsplice-cli runs this
    rather than having a mixer of its own, so a render from the command line
    is the app's own export and can't drift from it. What happened goes to
    the report file (a GUI app has no console to print to); the exit code is
    0 when every file was written. */
inline constexpr const char* kRenderFlag = "--render";

/** `SoundSplice --render-job <job.xml> <report>`: a render queue job (or
    soundsplice-cli's render with a preset) - a project snapshot, where the
    file goes, and every choice Export Audio makes (app/ExportChoices.h). */
inline constexpr const char* kRenderJobFlag = "--render-job";

inline int runRenderJob(const juce::StringArray& args)
{
    const int at = args.indexOf(kRenderJobFlag);
    if (at < 0 || args.size() < at + 3)
        return 2;
    const juce::File reportFile(args[at + 2].unquoted());
    const auto job = app::exportchoices::deserializeJob(juce::File(args[at + 1].unquoted()).loadFileAsString());
    if (! job)
    {
        reportFile.replaceWithText("That isn't a render job");
        return 2;
    }

    juce::String report;
    bool         ok = false;
    {
        MainComponent main(true);
        ok = main.renderHeadless(job->project, job->output, job->choice, report);
    }
    reportFile.replaceWithText(report);
    return ok ? 0 : 1;
}

inline int runHeadlessRender(const juce::StringArray& args)
{
    const int at = args.indexOf(kRenderFlag);
    if (at < 0 || args.size() < at + 7)
        return 2;

    const juce::File project(args[at + 1].unquoted());
    const juce::File out(args[at + 2].unquoted());
    const juce::File reportFile(args[at + 6].unquoted());

    engine::ExportOptions options;
    options.format = engine::ExportFormat::Wav;
    for (int i = 0; i < engine::kNumExportFormats; ++i)
        if (out.hasFileExtension(engine::extensionFor((engine::ExportFormat) i)))
            options.format = (engine::ExportFormat) i;
    options.sampleRate    = juce::jlimit(8000.0, 384000.0, args[at + 3].getDoubleValue());
    options.bitsPerSample = args[at + 4].getIntValue() == 16 ? 16 : args[at + 4].getIntValue() == 32 ? 32 : 24;
    options.contents      = (engine::ExportContents) juce::jlimit(0, engine::kNumExportContents - 1, args[at + 5].getIntValue());
    if (args.size() > at + 7)
        options.loudnessLufs = juce::jmin(0.0, args[at + 7].getDoubleValue());

    juce::String report;
    bool         ok = false;
    {
        MainComponent main(true);
        ok = main.renderHeadless(project, out, options, report);
    }
    reportFile.replaceWithText(report);
    return ok ? 0 : 1;
}
} // namespace soundsplice

namespace soundsplice
{
/** Application entry point and top-level window for Phase 0. */
class SoundSpliceApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "SoundSplice"; }
    const juce::String getApplicationVersion() override { return "0.0.1"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        juce::Logger::writeToLog("Another instance started: " + commandLine);
    }

    void initialise(const juce::String&) override
    {
        // Run as a plugin prober by a scan (app/PluginProbe.h): probe, report
        // and go, with no window and nothing else started.
        if (const auto args = getCommandLineParameterArray(); args.contains(kProbePluginFlag))
        {
            setApplicationReturnValue(runPluginProbe(args));
            quit();
            return;
        }

        // Run by the render queue: see runRenderJob.
        if (const auto args = getCommandLineParameterArray(); args.contains(kRenderJobFlag))
        {
            setApplicationReturnValue(runRenderJob(args));
            quit();
            return;
        }

        // Run by soundsplice-cli to render a project: see runHeadlessRender.
        if (const auto args = getCommandLineParameterArray(); args.contains(kRenderFlag))
        {
            setApplicationReturnValue(runHeadlessRender(args));
            quit();
            return;
        }

        // Before any window: everything takes its colours from it.
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);

        logger.reset(juce::FileLogger::createDefaultAppLogger(
            "SoundSplice", "SoundSplice.log",
            "SoundSplice " + getApplicationVersion() + " starting up"));
        juce::Logger::setCurrentLogger(logger.get());
        juce::Logger::writeToLog("System: " + juce::SystemStats::getOperatingSystemName()
            + " (" + juce::SystemStats::getDeviceDescription() + ")");

        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        // The dialogs first: theirs are callbacks into the main window (a
        // render queue's thread among them), and the look and feel they draw
        // with goes below.
        auto& desktop = juce::Desktop::getInstance();
        for (int i = desktop.getNumComponents(); --i >= 0;)
            if (auto* dialog = dynamic_cast<juce::DialogWindow*>(desktop.getComponent(i)))
                delete dialog;

        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        juce::Logger::writeToLog("SoundSplice shutting down");
        juce::Logger::setCurrentLogger(nullptr);
        logger = nullptr;
    }

    /** Quitting discards the document like any other destructive action, so
        it asks the same question New and Open do. The answer arrives
        asynchronously, so this returns without quitting and the callback
        finishes the job — a Cancel simply never calls back. */
    void systemRequestedQuit() override
    {
        // With how it got here: a quit nobody asked for otherwise shows up in
        // the log only as "shutting down", with nothing to say what caused it.
        juce::Logger::writeToLog("Quit requested\n" + juce::SystemStats::getStackBacktrace());

        auto* main = mainWindow != nullptr
                         ? dynamic_cast<MainComponent*>(mainWindow->getContentComponent())
                         : nullptr;

        if (main == nullptr)
        {
            quit(); // nothing open to lose
            return;
        }

        main->confirmDiscardChanges([] { quit(); }); // JUCEApplicationBase::quit() is static
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name,
                             juce::Desktop::getInstance().getDefaultLookAndFeel()
                                 .findColour(juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);

            // The frame dark like the interface, where the platform asks.
            if (auto* peer = getPeer())
                useDarkTitleBar(peer->getNativeHandle());
        }

        void closeButtonPressed() override
        {
            // The close button, Alt+F4, or anything else sending the window
            // WM_CLOSE — logged so it can be told apart from other quits.
            juce::Logger::writeToLog("Window close requested");
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::FileLogger> logger;
    AppLookAndFeel                    lookAndFeel; // see app/Theme.h
};

} // namespace soundsplice

START_JUCE_APPLICATION(soundsplice::SoundSpliceApplication)
