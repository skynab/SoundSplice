#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ExportAudioDialog.h>
#include <app/RenderQueueDialog.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    app::ExportChoice someChoice()
    {
        app::ExportChoice c;
        c.options.format        = engine::ExportFormat::Flac;
        c.options.sampleRate    = 44100.0;
        c.options.bitsPerSample = 16;
        c.options.contents      = engine::ExportContents::MasterMixAndStems;
        c.options.noiseShaping  = true;
        c.options.loudnessLufs  = -16.0;
        c.range                 = app::ExportRange::MarkerRanges;
        c.namePattern           = "$index - $region";
        c.tagging               = app::ExportTagging::InfoOnly;
        c.report                = true;
        return c;
    }

    void requireSame(const app::ExportChoice& a, const app::ExportChoice& b)
    {
        REQUIRE(a.options.format == b.options.format);
        REQUIRE(a.options.sampleRate == b.options.sampleRate);
        REQUIRE(a.options.bitsPerSample == b.options.bitsPerSample);
        REQUIRE(a.options.contents == b.options.contents);
        REQUIRE(a.options.dither == b.options.dither);
        REQUIRE(a.options.noiseShaping == b.options.noiseShaping);
        REQUIRE(a.options.loudnessLufs == b.options.loudnessLufs);
        REQUIRE(a.range == b.range);
        REQUIRE(a.namePattern == b.namePattern);
        REQUIRE(a.tagging == b.tagging);
        REQUIRE(a.report == b.report);
    }
}

TEST_CASE("Render presets and queue jobs keep every choice", "[gui][renderqueue]")
{
    JuceFixture fixture;
    namespace choices = app::exportchoices;

    auto presets = choices::withPreset({}, { "Podcast", someChoice() });
    presets      = choices::withPreset(presets, { "Master", app::ExportChoice {} });
    presets      = choices::withPreset(presets, { "Podcast", someChoice() }); // replaced, not added
    REQUIRE(presets.size() == 2);

    const auto read = choices::deserializePresets(choices::serializePresets(presets));
    REQUIRE(read.size() == 2);
    REQUIRE(read[0].name == "Podcast");
    requireSame(read[0].choice, someChoice());

    auto choice                 = someChoice();
    choice.range                = app::ExportRange::TimeSelection;
    choice.selectionStartBeats  = 8.0;
    choice.selectionLengthBeats = 16.0;
    const choices::Job job { juce::File::getCurrentWorkingDirectory().getChildFile("snap.soundsplice"),
                             juce::File::getCurrentWorkingDirectory().getChildFile("out.flac"), choice, "out.flac (Show)" };
    const auto queue = choices::deserializeQueue(choices::serializeQueue({ job, job }));
    REQUIRE(queue.size() == 2);
    REQUIRE(queue[1].output == job.output);
    REQUIRE(queue[1].label == job.label);
    REQUIRE(queue[1].choice.selectionLengthBeats == 16.0);
    requireSame(queue[1].choice, choice);

    REQUIRE(choices::deserializeQueue("").empty());
    REQUIRE_FALSE(choices::deserializeJob("<RENDER_JOB/>").has_value()); // no files: not a job
}

TEST_CASE("Export Audio's boxes show a preset as it was saved", "[gui][renderqueue]")
{
    JuceFixture fixture;
    juce::AlertWindow window("Export", {}, juce::MessageBoxIconType::NoIcon);
    app::ExportAudioDialog::buildControls(window, 48000.0, true, 3);

    app::ExportAudioDialog::applyChoice(window, someChoice(), 48000.0);
    requireSame(app::ExportAudioDialog::readChoice(window), someChoice());

    // A range this project hasn't got leaves the range as it was.
    juce::AlertWindow plain("Export", {}, juce::MessageBoxIconType::NoIcon);
    app::ExportAudioDialog::buildControls(plain, 48000.0, false, 0);
    app::ExportAudioDialog::applyChoice(plain, someChoice(), 48000.0);
    REQUIRE(app::ExportAudioDialog::readChoice(plain).range == app::ExportRange::Project);
}

TEST_CASE("The render queue renders in order and keeps what failed", "[gui][renderqueue]")
{
    JuceFixture fixture;

    std::vector<juce::String> ran;
    juce::CriticalSection     lock;
    RenderQueueDialog dialog([&](const app::exportchoices::Job& job, juce::String& report, std::function<bool()>)
    {
        const juce::ScopedLock sl(lock);
        ran.push_back(job.label);
        if (job.label == "bad")
        {
            report = "disk full";
            return false;
        }
        return true;
    });

    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("x");
    dialog.setJobs({ { file, file, {}, "one" }, { file, file, {}, "bad" }, { file, file, {}, "three" } });

    std::vector<int> rendered;
    bool             finished = false;
    dialog.onRunFinished = [&](const std::vector<int>& r)
    {
        rendered = r;
        finished = true;
    };

    // Render All, as clicked.
    for (auto* child : dialog.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(child); b != nullptr && b->getButtonText() == "Render All")
            b->triggerClick();

    for (int i = 0; i < 200 && ! finished; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    REQUIRE(finished);
    REQUIRE(ran == std::vector<juce::String> { "one", "bad", "three" });
    REQUIRE(rendered == std::vector<int> { 0, 2 });
    REQUIRE_FALSE(dialog.running());
}
