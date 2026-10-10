#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The window's chrome round the workspace, as the SoundSplice mockups lay it
// out: a title strip holding the menus, a toolbar to switch between the
// Waveform and Multitrack workspaces and reach their tools, and below the
// panes a transport bar and a status line.
//
// Nothing here is a new way of doing anything. Every button stands for a
// command the menus already offer, or clicks a control that already exists,
// so the toolbar, the menus and the keyboard can't disagree.

namespace soundsplice
{
namespace
{
    /** A tool on the toolbar: the command it runs and how it's drawn. */
    struct Tool
    {
        int           command;
        glyphs::Glyph glyph;
    };

    /** Each workspace's tools, in the order the mockups put them. Toggles
        light while their command is ticked. */
    std::vector<Tool> toolsFor(layouts::Workspace workspace)
    {
        using glyphs::Glyph;
        if (workspace == layouts::Workspace::AudioEditing)
            return { { commands::zoomToSelection, Glyph::selection },
                     { commands::findZeroCrossings, Glyph::zeroCrossing },
                     { commands::fitProject, Glyph::arrowLeftRight } };

        return { { commands::splitAtPlayhead, Glyph::scissors },
                 { commands::snapToGrid, Glyph::magnet },
                 { commands::showClipEnvelopes, Glyph::envelope },
                 { commands::fitProject, Glyph::arrowLeftRight } };
    }

    // The middle dot the mockups separate facts with.
    const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");

    juce::String seconds(double s)
    {
        // m:ss.mmm, as the mockups write every time on the transport.
        s = juce::jmax(0.0, s);
        const int minutes = (int) (s / 60.0);
        return juce::String(minutes) + ":" + juce::String(s - minutes * 60.0, 3).paddedLeft('0', 6);
    }
}

void MainComponent::setUpShell()
{
    addAndMakeVisible(titleStrip_);
    addAndMakeVisible(toolbar_);
    addAndMakeVisible(transportBar_);
    addAndMakeVisible(statusLine_);

    workspaceSwitch_.setOptions({ { "Waveform", true, glyphs::Glyph::waveform },
                                  { "Multitrack", true, glyphs::Glyph::rows } });
    workspaceSwitch_.onChange = [this](int index)
    {
        applyWorkspaceLayout(index == 0 ? layouts::Workspace::AudioEditing : layouts::Workspace::MusicCreation);
        layoutToolbar();
    };
    toolbar_.addAndMakeVisible(workspaceSwitch_);

    viewSwitch_.setOptions({ { "Waveform" }, { "Spectral" } }, 11.5f);
    viewSwitch_.onChange = [this](int index)
    {
        if ((index == 1) != commandIsTicked(commands::spectrogramView))
            invokeCommand(commands::spectrogramView);
    };
    toolbar_.addAndMakeVisible(viewSwitch_);

    for (auto& rule : toolbarRules_)
        toolbar_.addAndMakeVisible(rule);

    // The rack is wherever this workspace keeps its effects: the track's
    // chain in Multitrack, the mastering rack in Waveform.
    theme::setStyle(effectsRackButton_, theme::buttonStyle::ghost);
    effectsRackButton_.setTooltip("Show the effects rack");
    effectsRackButton_.onClick = [this]
    {
        const juce::String pane = activeWorkspace_ == layouts::Workspace::AudioEditing ? "Mastering" : "Track FX";
        if (workspace_.isPanelOpen(pane))
            workspace_.revealPanel(pane);
        else
            workspace_.openPanel(pane);
        saveDockLayout();
    };
    toolbar_.addAndMakeVisible(effectsRackButton_);

    theme::setStyle(primaryActionButton_, theme::buttonStyle::primary);
    primaryActionButton_.onClick = [this]
    {
        invokeCommand(activeWorkspace_ == layouts::Workspace::AudioEditing ? commands::saveProject
                                                                            : commands::exportAudio);
    };
    toolbar_.addAndMakeVisible(primaryActionButton_);

    // The transport bar's buttons click the transport's own controls, which
    // the commands and the keyboard already go through.
    transportBar_.toStart.onClick = [this] { firstFrameButton.triggerClick(); };
    transportBar_.back.onClick    = [this] { previousFrameButton.triggerClick(); };
    transportBar_.play.onClick    = [this] { playPauseButton.triggerClick(); };
    transportBar_.forward.onClick = [this] { nextFrameButton.triggerClick(); };
    transportBar_.toEnd.onClick   = [this] { lastFrameButton.triggerClick(); };
    transportBar_.record.onClick  = [this] { recordButton.triggerClick(); };
    transportBar_.loop.onClick    = [this] { loopButton.triggerClick(); };

    // Stop pauses where it is; pressed again, stopped, it goes back to the
    // start - the two halves of what a separate stop button is for.
    transportBar_.stop.onClick = [this]
    {
        if (engine_.isPlaying() || filePlayback_.active)
            playPauseButton.triggerClick();
        else
            firstFrameButton.triggerClick();
    };

    transportBar_.toStart.setTooltip(firstFrameButton.getTooltip());
    transportBar_.back.setTooltip(previousFrameButton.getTooltip());
    transportBar_.play.setTooltip(playPauseButton.getTooltip());
    transportBar_.forward.setTooltip(nextFrameButton.getTooltip());
    transportBar_.toEnd.setTooltip(lastFrameButton.getTooltip());
    transportBar_.record.setTooltip(recordButton.getTooltip());
    transportBar_.loop.setTooltip(loopButton.getTooltip());
    transportBar_.stop.setTooltip("Stop" + dot + "press again to go back to the start");
    transportBar_.record.setTint(theme::colour(*this, theme::dangerId));

    // The bar over a selection in the waveform: the edits the Waveform
    // mockup offers there, each the Edit menu's own command.
    {
        using glyphs::Glyph;
        const std::pair<int, std::pair<const char*, Glyph>> actions[] = {
            { commands::silenceAudio,           { "Silence", Glyph::speakerSlash } },
            { commands::fadeIn,                 { "Fade in", Glyph::trendUp } },
            { commands::fadeOut,                { "Fade out", Glyph::trendDown } },
            { commands::normalizePeak,          { "Normalize", Glyph::normalize } },
            { commands::adaptiveNoiseReduction, { "Denoise", Glyph::waves } },
            { commands::deleteAudio,            { "Delete", Glyph::trash } },
        };
        std::vector<SelectionActions::Action> list;
        for (const auto& [id, look] : actions)
        {
            juce::String tooltip;
            if (const auto* command = commandManager_.getCommandForID(id))
                tooltip = command->description;
            list.push_back({ look.first, look.second, tooltip, [this, id = id] { invokeCommand(id); } });
        }
        audioEditor_.setSelectionActions(std::move(list));
    }

    layoutToolbar();
}

void MainComponent::layoutShell(juce::Rectangle<int>& area)
{
    titleStrip_.setBounds(area.removeFromTop(shell::kTitleHeight));
    toolbar_.setBounds(area.removeFromTop(shell::kToolbarHeight));
    statusLine_.setBounds(area.removeFromBottom(shell::kStatusHeight));
    transportBar_.setBounds(area.removeFromBottom(shell::kTransportHeight));
    layoutToolbar();
}

/** Places the toolbar's controls, rebuilding the tool buttons when the
    workspace has changed since they were made. */
void MainComponent::layoutToolbar()
{
    const bool waveform = activeWorkspace_ == layouts::Workspace::AudioEditing;

    if (toolsBuiltFor_ != (int) activeWorkspace_)
    {
        toolsBuiltFor_ = (int) activeWorkspace_;
        toolButtons_.clear();
        toolCommands_.clear();
        for (const auto& tool : toolsFor(activeWorkspace_))
        {
            juce::ApplicationCommandInfo info(tool.command);
            juce::String                 name = "Tool";
            if (auto* target = commandManager_.getTargetForCommand(tool.command, info))
            {
                juce::ignoreUnused(target);
                name = info.shortName;
            }
            auto button = std::make_unique<GlyphButton>(name, tool.glyph);
            const auto key = commandManager_.getKeyMappings()->getKeyPressesAssignedToCommand(tool.command);
            button->setTooltip(key.isEmpty() ? name : name + " (" + key.getFirst().getTextDescriptionWithIcons() + ")");
            button->onClick = [this, id = tool.command] { invokeCommand(id); };
            toolbar_.addAndMakeVisible(*button);
            toolButtons_.push_back(std::move(button));
            toolCommands_.push_back(tool.command);
        }

        primaryActionButton_.setButtonText(waveform ? "Save" : "Export mixdown");
        primaryActionButton_.setTooltip(waveform ? "Save the project" : "Export the mix as an audio file");
        transportBar_.back.setVisible(! waveform);
        transportBar_.forward.setVisible(! waveform);
    }

    workspaceSwitch_.setSelected(waveform ? 0 : 1);
    viewSwitch_.setVisible(waveform);

    auto row = toolbar_.getLocalBounds().reduced(12, 0);
    row.removeFromBottom(1);
    const auto place = [&row](juce::Component& c, int width, int height, int gapAfter)
    {
        c.setBounds(row.removeFromLeft(width).withSizeKeepingCentre(width, height));
        row.removeFromLeft(gapAfter);
    };

    place(workspaceSwitch_, workspaceSwitch_.idealWidth(), 32, 6);
    place(toolbarRules_[0], 9, 28, 6);
    for (auto& button : toolButtons_)
        place(*button, 30, 28, 2);
    row.removeFromLeft(4);
    if (waveform)
    {
        place(toolbarRules_[1], 9, 28, 6);
        place(viewSwitch_, viewSwitch_.idealWidth(), 28, 0);
    }
    toolbarRules_[1].setVisible(waveform);

    auto right = row;
    const auto primaryWidth = juce::jmax(70, primaryActionButton_.getBestWidthForHeight(28) + 12);
    primaryActionButton_.setBounds(right.removeFromRight(primaryWidth).withSizeKeepingCentre(primaryWidth, 28));
    right.removeFromRight(8);
    effectsRackButton_.setBounds(right.removeFromRight(104).withSizeKeepingCentre(104, 28));
}

/** Brings the chrome up to date with the document and the transport. Called
    every timer tick, so every setter here compares before repainting. */
void MainComponent::updateShell()
{
    const bool waveform = activeWorkspace_ == layouts::Workspace::AudioEditing;
    if (toolsBuiltFor_ != (int) activeWorkspace_)
        layoutToolbar(); // switched from the menus
    workspaceSwitch_.setSelected(waveform ? 0 : 1);

    // ---- title ----
    const bool   saved   = projectFile_ != juce::File{};
    const auto   project = saved ? projectFile_.getParentDirectory().getFileName() : juce::String();
    juce::String document = saved ? projectFile_.getFileName() : juce::String("Untitled");
    if (waveform && audioEditor_.file() != juce::File{})
        document = audioEditor_.file().getFileName();
    titleStrip_.setDocument(project, document, hasUnsavedChanges());
    titleStrip_.setStatus(hasUnsavedChanges() ? juce::String("Unsaved changes")
                          : saved             ? juce::String("All changes saved")
                                              : juce::String());

    // ---- toolbar ----
    for (size_t i = 0; i < toolButtons_.size() && i < toolCommands_.size(); ++i)
        toolButtons_[i]->setLit(commandIsTicked(toolCommands_[i]));
    if (waveform)
        viewSwitch_.setSelected(commandIsTicked(commands::spectrogramView) ? 1 : 0);

    // ---- transport ----
    const double sampleRate = engine_.sampleRate() > 0.0 ? engine_.sampleRate() : 48000.0;
    // In the Waveform view the clock is the open file's: where its cursor
    // is, counted from its start, as the transport plays it.
    const bool   fileClock  = playsOpenFile() || filePlayback_.active;
    const double playhead   = fileClock ? filePlaybackSeconds() : (double) engine_.playheadSamples() / sampleRate;
    transportBar_.setTime(timeDisplay_.format == app::TimeFormat::BarsBeats
                              ? seconds(playhead)
                              : juce::String(app::formatPosition(timeDisplay_, playhead)));
    const bool rolling = fileClock ? filePlayback_.active : engine_.isPlaying();
    transportBar_.play.setGlyph(rolling ? glyphs::Glyph::pause : glyphs::Glyph::play);
    transportBar_.record.setGlyph(recordButton.getToggleState() ? glyphs::Glyph::stop : glyphs::Glyph::record);
    transportBar_.loop.setLit(loopButton.getToggleState());
    transportBar_.setLevels(engine_.masterPeak(0), engine_.masterPeak(1));

    // The selection and the view, in the editor that's in front: the clip's
    // own seconds in Waveform, the arrangement's in Multitrack.
    juce::StringArray selection, view;
    if (waveform)
    {
        const auto range = audioEditor_.selection();
        if (! range.isEmpty())
            selection = { "Selection", seconds(range.startSeconds), seconds(range.endSeconds),
                          seconds(range.lengthSeconds()) };
        else
            selection = { "Cursor", seconds(audioEditor_.cursorSeconds()), seconds(audioEditor_.cursorSeconds()),
                          seconds(0.0) };
        if (audioEditor_.file() != juce::File{})
        {
            const auto shown = audioEditor_.visibleSeconds();
            const auto to    = juce::jmin(shown.second, audioEditor_.lengthSeconds());
            view = { "View", seconds(shown.first), seconds(to), seconds(to - shown.first) };
        }
    }
    else if (! timeSelection_.isEmpty())
    {
        const auto at = [this, sampleRate](double beats)
        { return (double) uiTempoMap_.samplesFromPpq(beats) / sampleRate; };
        const double from = at(timeSelection_.startBeats), to = at(timeSelection_.endBeats);
        selection = { "Selection", seconds(from), seconds(to), seconds(to - from) };
    }
    transportBar_.setReadout(selection, view);

    // ---- status ----
    juce::String hint;
    if (waveform)
        hint = audioEditor_.selection().isEmpty()
                   ? "Drag across the waveform to select" + dot + "click to place the cursor"
                   : "Edits rewrite the selection" + dot + "undo from History";
    else
        hint = "Select a clip" + dot + "drag its edges to trim, its corners to fade";
    statusLine_.setText(hint, juce::String((int) sampleRate) + " Hz" + dot + "32-bit float");
}

bool MainComponent::commandIsTicked(int commandId)
{
    juce::ApplicationCommandInfo info(commandId);
    if (commandManager_.getTargetForCommand(commandId, info) == nullptr)
        return false;
    return (info.flags & juce::ApplicationCommandInfo::isTicked) != 0;
}

void MainComponent::invokeCommand(int commandId)
{
    commandManager_.invokeDirectly(commandId, true);
}

} // namespace soundsplice
