#include "MainComponentInternal.h"

// Part of MainComponent (shared pieces in MainComponentInternal.h).
// The workspace: panes, dock layouts, zoom controls and laying out each tab.

namespace soundsplice
{
void MainComponent::layoutLeftPane()
{
    auto area = leftPane_.getLocalBounds().reduced(12);

    auto row = area.removeFromTop(30);

    // Taken off the right first, so it stays pinned to the far edge whatever
    // width the pane has.
    collapseTransportButton_.setBounds(row.removeFromRight(28).reduced(2));
    row.removeFromRight(8);

    // The controls wrap onto another row when they don't fit — see
    // app::wrapRow for what went wrong when they didn't.
    //
    // First / previous / play-pause / next / last, in that order. The
    // frame-step glyphs are wider than tall, play/pause is taller than wide,
    // so they get different widths to keep the drawn glyphs a similar size.
    const std::vector<app::RowItem> items {
        { 32,  0, { 2, 2 } }, // first frame
        { 26,  0, { 2, 2 } }, // previous frame
        { 30,  0, { 3, 1 } }, // play/pause
        { 26,  0, { 2, 2 } }, // next frame
        { 32,  0, { 2, 2 } }, // last frame
        { 60, 12, { 0, 0 } }, // loop
        { 30, 12, { 1, 1 } }, // record — square: the icon is 25x25
        { 64, 12, { 0, 0 } }, // click
        { 78,  6, { 0, 0 } }, // monitor
        { 110, 6, { 0, 2 } }, // count-in
    };

    juce::Component* const controls[] {
        &firstFrameButton, &previousFrameButton, &playPauseButton,
        &nextFrameButton, &lastFrameButton, &loopButton, &recordButton,
        &metronomeButton, &monitorButton, &countInBox_
    };

    // The rows the buttons need, taken off the top before anything below is
    // placed — so a wrapped row pushes the tempo and position readouts down
    // rather than drawing over them.
    const int buttonsHeight = app::wrappedRowHeight(row.getWidth(), 30, 4, items);
    auto      buttonsArea   = row.withHeight(buttonsHeight);
    area.removeFromTop(buttonsHeight - row.getHeight());

    const auto bounds = app::wrapRow(buttonsArea, 30, 4, items);
    for (size_t i = 0; i < bounds.size() && i < std::size(controls); ++i)
        controls[i]->setBounds(bounds[i]);

    area.removeFromTop(8);

    if (transportCollapsed_)
        return; // nothing below the button row is showing

    positionLabel.setBounds(area.removeFromTop(28));
    clipLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(6);

    tempoSlider.setBounds(area.removeFromTop(26).withTrimmedLeft(64));
    area.removeFromTop(6);

    // Below tempo, sharing its label gutter: they are the two things that
    // decide what a bar is.
    timeSigBox_.setBounds(area.removeFromTop(24).withTrimmedLeft(64).removeFromLeft(90));
}

/** Shows or hides everything below the transport's button row. The arrow
    points the way the content will go, so it reads the same whichever state
    it's in. */
void MainComponent::applyTransportCollapse()
{
    juce::Component* belowFirstRow[] = { &positionLabel, &clipLabel, &tempoSlider,
                                        &timeSigBox_, &timeSigLabel_ };
    for (auto* c : belowFirstRow)
        c->setVisible(! transportCollapsed_);

    collapseTransportButton_.setButtonText(transportCollapsed_ ? "v" : "^");
    collapseTransportButton_.setTooltip(transportCollapsed_ ? "Show tempo and position"
                                                            : "Hide tempo and position");
    layoutLeftPane();
}

/** Index of @p name in the workspace's panel list — the offset that turns a
    View-menu id back into a panel. Both directions go through
    registeredPanels(), so the mapping can't drift as panes are added. */
int MainComponent::panelMenuIndex(const juce::String& name) const
{
    const auto names = workspace_.registeredPanels();
    for (int i = 0; i < (int) names.size(); ++i)
        if (names[(size_t) i] == name)
            return i;
    return 0;
}

/** Opens, reveals or closes the panel at @p index in the workspace's list. */
void MainComponent::togglePanel(int index)
{
    const auto names = workspace_.registeredPanels();
    if (index < 0 || index >= (int) names.size())
        return;

    const auto& name = names[(size_t) index];

    if (! workspace_.isPanelOpen(name))
        workspace_.openPanel(name);
    else if (workspace_.isPanelActive(name))
        workspace_.closePanel(name);   // already in front: the click means close
    else
        workspace_.revealPanel(name);  // open but buried: bring it forward first

    saveDockLayout();
}

/** The arrangement a workspace with no saved layout gets.

    Built from the same definition the Layout menu uses (see
    layouts::buildWorkspaceLayout) rather than by a second sequence of splits
    here — two descriptions of "the default" would drift, and the imperative
    one couldn't be checked by a headless test. */
void MainComponent::buildDefaultDockLayout()
{
    if (! workspace_.restoreLayout(layouts::workspaceLayoutText(activeWorkspace_)))
    {
        // Only reachable if a built-in layout stopped parsing, which the
        // layout tests exist to prevent. An empty single region is still a
        // usable workspace: every pane is reachable from the View menu.
        workspace_.resetToSingleRegion();
        workspace_.addPanel(workspace_.rootRegion(), "Tracks");
    }
}

juce::String MainComponent::settingsKeyForWorkspace(layouts::Workspace workspace) const
{
    return juce::String("dockLayout.") + layouts::workspaceName(workspace);
}

void MainComponent::loadDockLayout()
{
    // Which layout was in use last. Absent on an existing install, where
    // Music Creation is right: it is the arrangement the app already had.
    const auto savedName = settings_.getValue("activeLayout");
    for (int i = 0; i < layouts::kNumWorkspaces; ++i)
    {
        const auto workspace = (layouts::Workspace) i;
        if (savedName == layouts::workspaceName(workspace))
            activeWorkspace_ = workspace;
    }

    // That layout's own saved arrangement, then the flat "dockLayout" key an
    // older build wrote (so an existing install keeps the workspace it had
    // rather than being reset), then the built-in default.
    //
    // A saved layout that no longer parses — an older format, or one naming a
    // panel this build doesn't have — falls through rather than leaving a
    // half-built workspace.
    if (workspace_.restoreLayout(settings_.getValue(settingsKeyForWorkspace(activeWorkspace_))))
        return;
    if (workspace_.restoreLayout(settings_.getValue("dockLayout")))
        return;

    buildDefaultDockLayout();
}

void MainComponent::saveDockLayout()
{
    // Both the active layout's own slot and the flat key: the flat one is
    // what an older build reads, so writing it keeps a downgrade from
    // landing on an empty workspace.
    settings_.setValue("dockLayout", workspace_.saveLayout());
    saveActiveWorkspaceLayout();
}

void MainComponent::saveActiveWorkspaceLayout()
{
    settings_.setValue(settingsKeyForWorkspace(activeWorkspace_), workspace_.saveLayout());
    settings_.setValue("activeLayout", layouts::workspaceName(activeWorkspace_));
    settings_.saveIfNeeded();
}

/** Switches workspace. The outgoing arrangement is saved into its own slot
    first, so coming back finds it as it was left rather than reset to the
    built-in default. */
void MainComponent::applyWorkspaceLayout(layouts::Workspace workspace)
{
    saveActiveWorkspaceLayout();

    activeWorkspace_ = workspace;

    // The incoming layout's remembered arrangement, or its built-in default
    // the first time it's used. restoreLayout deliberately doesn't broadcast
    // a layout change, so neither of these can write back over the slot we
    // just saved.
    const auto remembered = settings_.getValue(settingsKeyForWorkspace(workspace));
    if (! workspace_.restoreLayout(remembered))
        workspace_.restoreLayout(layouts::workspaceLayoutText(workspace));

    resized();
    saveActiveWorkspaceLayout();
    showStatus("Layout: " + juce::String(layouts::workspaceName(workspace)));
}

/** Builds one of the zoom controls: icon, slider and editable multiplier.

    Shared by the tracks and keys panes. They zoom different axes — time in
    one, pitch in the other — but the control is the same thing and reads the
    same way, so it is built in one place. */
void MainComponent::setUpZoomControls(juce::Component& parent, juce::DrawableButton& icon,
                                      juce::Slider& slider, juce::Slider& box,
                                      double minZoom, double maxZoom,
                                      const juce::String& tooltip,
                                      std::function<void(float)> onZoom)
{
    // A DrawableButton in ImageFitted mode, as every other SVG in this app
    // uses. Clicks are switched off: this labels the slider, it isn't a
    // control.
    auto magnifier = icons::fromSvg(icons::kMagnifier);
    icon.setImages(magnifier.get());
    icon.setInterceptsMouseClicks(false, false);
    icon.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    parent.addAndMakeVisible(icon);

    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setRange(minZoom, maxZoom, 0.0);
    // Zoom is multiplicative, so a linear track would put x1 a fifth of the
    // way along and give most of the travel to zooming in. Skewing about the
    // midpoint puts x1 in the middle, where it belongs.
    slider.setSkewFactorFromMidPoint(1.0);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setTooltip(tooltip);
    slider.onValueChange = [&slider, onZoom] { onZoom((float) slider.getValue()); };
    parent.addAndMakeVisible(slider);

    box.setSliderStyle(juce::Slider::LinearBar); // a text field with a drag, not a track
    box.setRange(minZoom, maxZoom, 0.0);
    box.setSkewFactorFromMidPoint(1.0);
    box.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 20);
    // "x1.00" rather than "1.00 x": JUCE's suffix appends, and a multiplier
    // reads as a multiplier only with the x in front.
    box.textFromValueFunction = [](double value) { return "x" + juce::String(value, 2); };
    box.valueFromTextFunction = [](const juce::String& text)
    {
        return text.retainCharacters("0123456789.").getDoubleValue();
    };
    box.setTooltip(tooltip + " - type a multiplier, or drag");
    box.onValueChange = [&box, onZoom] { onZoom((float) box.getValue()); };
    parent.addAndMakeVisible(box);
}

/** Applies a new pitch zoom to the keys pane and keeps its controls
    describing it. The roll stores a row count, so the zoom lands on the
    nearest achievable window and the controls are set from where it landed
    rather than from what was asked for. */
void MainComponent::setKeysZoom(float zoom)
{
    pianoRoll_.setPitchZoom(zoom);
    updateKeysZoomControls();
}

void MainComponent::updateKeysZoomControls()
{
    const double zoom = pianoRoll_.pitchZoom();
    keysZoomSlider_.setValue(zoom, juce::dontSendNotification);
    keysZoomBox_.setValue(zoom, juce::dontSendNotification);
}

/** Keeps the playhead in view while the keys pane is scrolled.

    Only while playing: scrolling the view out from under someone who is
    editing a stopped pattern would be worse than the problem it solves. The
    paging rule itself is scrollToFollow, which is JUCE-free and tested — it
    pages rather than centring, so the grid stays still while the playhead
    crosses it instead of sliding continuously under a fixed line. */
void MainComponent::followKeysPlayhead()
{
    if (! keysFollowButton_.getToggleState() || ! engine_.isPlaying())
        return;

    const int viewportWidth = keysViewport_.getMaximumVisibleWidth();
    const int contentWidth  = pianoRoll_.getWidth();
    if (viewportWidth <= 0 || contentWidth <= viewportWidth)
        return; // nothing to scroll

    const int current = keysViewport_.getViewPositionX();
    const int wanted  = scrollToFollow((int) pianoRoll_.playheadX(), current,
                                       viewportWidth, contentWidth, kKeysFollowMargin);

    if (wanted != current)
        keysViewport_.setViewPosition(wanted, keysViewport_.getViewPositionY());
}

/** Widens the grid and lets the viewport scroll it. Unlike pitch zoom, which
    the roll stores as a row count and snaps, this is continuous — the roll
    simply draws to whatever width it's given. */
void MainComponent::setKeysTimeZoom(float zoom)
{
    pianoRoll_.setTimeZoom(zoom);
    updateKeysTimeZoomControls();
    layoutEditTab();
}

void MainComponent::updateKeysTimeZoomControls()
{
    const double zoom = pianoRoll_.timeZoom();
    keysTimeZoomSlider_.setValue(zoom, juce::dontSendNotification);
    keysTimeZoomBox_.setValue(zoom, juce::dontSendNotification);
}

/** Applies a new timeline zoom and keeps the controls describing it. */
void MainComponent::setTimelineZoom(float zoom)
{
    arrangementView_.setZoom(zoom);
    // Kept in step so a breakpoint sits under the bar it belongs to, rather
    // than under whichever bar this pane happened to be scaled for.
    automationPane_.setZoom(zoom);
    updateZoomControls();
}

/** Zooms the timeline so [@p startBeats, + @p lengthBeats) fills the view,
    with a little room either side, and scrolls it into place. The zoom is
    clamped to the view's range, so a span too long or too short to fit
    exactly is shown as near to it as that allows. */
void MainComponent::zoomTimelineToSpan(double startBeats, double lengthBeats)
{
    const auto& geometry = arrangementView_.geometry();
    const float visible  = (float) arrangementViewport_.getMaximumVisibleWidth() - geometry.gutterWidth;

    setTimelineZoom(app::zoomToFit(lengthBeats, visible, geometry.basePixelsPerBeat,
                                   ArrangementView::kMinZoom, ArrangementView::kMaxZoom));

    // After the zoom, so the scroll is measured at the new scale.
    arrangementViewport_.setViewPosition(app::scrollToShow(startBeats, lengthBeats, arrangementView_.geometry()),
                                         arrangementViewport_.getViewPositionY());
}

void MainComponent::zoomToTimeSelection()
{
    if (timeSelection_.isEmpty())
    {
        showError("Select time on the timeline first");
        return;
    }

    zoomTimelineToSpan(timeSelection_.startBeats, timeSelection_.lengthBeats());
}

void MainComponent::fitProjectInView()
{
    zoomTimelineToSpan(0.0, arrangementView_.arrangedEndBeats());
}

/** Sizes the lanes so every track fits the view's height, as far as the
    lanes' own limits allow, and scrolls back to the first track. */
void MainComponent::fitTracksVertically()
{
    const float height = app::laneHeightToFit(trackCount(), (float) arrangementViewport_.getMaximumVisibleHeight(),
                                              arrangementView_.geometry().rulerHeight,
                                              ArrangementView::kMinLaneHeight, ArrangementView::kMaxLaneHeight);
    arrangementView_.setLaneHeight(height);
    arrangementViewport_.setViewPosition(arrangementViewport_.getViewPositionX(), 0);
}

/** Mirrors the current zoom into both controls without either of them
    reporting it straight back as a user edit — they set each other, and the
    keyboard shortcuts set both. */
void MainComponent::updateZoomControls()
{
    const double zoom = arrangementView_.zoom();
    zoomSlider_.setValue(zoom, juce::dontSendNotification);
    zoomBox_.setValue(zoom, juce::dontSendNotification);
}

void MainComponent::layoutArrangeTab()
{
    auto area = arrangeTab_.getLocalBounds();

    auto toolbar = area.removeFromTop(28).reduced(4, 2);

    zoomIcon_.setBounds(toolbar.removeFromLeft(24));
    toolbar.removeFromLeft(2);
    zoomSlider_.setBounds(toolbar.removeFromLeft(120));
    toolbar.removeFromLeft(6);
    zoomBox_.setBounds(toolbar.removeFromLeft(56));
    toolbar.removeFromLeft(12);
    addClipButton_.setBounds(toolbar.removeFromLeft(90));

    arrangementViewport_.setBounds(area);
}

void MainComponent::layoutEditTab()
{
    auto area   = editTab_.getLocalBounds();
    auto header = area.removeFromTop(24);

    barsBox_.setBounds(header.removeFromRight(56).reduced(2, 0));
    barsLabel_.setBounds(header.removeFromRight(34));

    header.removeFromRight(10);
    keysZoomBox_.setBounds(header.removeFromRight(52).reduced(0, 2));
    keysZoomSlider_.setBounds(header.removeFromRight(80).reduced(2, 1));
    keysZoomIcon_.setBounds(header.removeFromRight(22).reduced(0, 1));

    header.removeFromRight(10);
    keysTimeZoomBox_.setBounds(header.removeFromRight(52).reduced(0, 2));
    keysTimeZoomSlider_.setBounds(header.removeFromRight(80).reduced(2, 1));
    keysTimeZoomIcon_.setBounds(header.removeFromRight(22).reduced(0, 1));

    header.removeFromRight(8);
    keysFollowButton_.setBounds(header.removeFromRight(72).reduced(0, 2));

    editingLabel_.setBounds(header.reduced(6, 0));

    keysViewport_.setBounds(area);

    // The roll is as tall as the pane — rows fill it, and pitch zoom decides
    // how many — and as wide as the time zoom asks for, which is what the
    // viewport then scrolls.
    const int visibleWidth = juce::jmax(1, keysViewport_.getMaximumVisibleWidth());
    pianoRoll_.setSize(juce::jmax(visibleWidth, pianoRoll_.preferredWidth(visibleWidth)),
                       juce::jmax(1, keysViewport_.getMaximumVisibleHeight()));
}

void MainComponent::layoutMixerView()
{
    auto area = mixerView_.getLocalBounds().reduced(10);
    if (area.isEmpty())
        return;

    auto toolbar = area.removeFromTop(28);
    addTrackButton.setBounds(toolbar.removeFromLeft(100));
    area.removeFromTop(8);

    // ---- per-track channel strips, filling the remaining width ----
    const int stripWidth = 96;
    const int gap        = 6;
    int       x          = area.getX();
    const int n           = trackCount();

    for (int i = 0; i < engine_.maxTracks(); ++i)
    {
        auto* strip = trackStrips_[i];
        if (i < n)
        {
            strip->setBounds(x, area.getY(), stripWidth, area.getHeight());
            x += stripWidth + gap;
        }
    }
}

void MainComponent::layoutMasterPanel()
{
    auto masterArea = masterPanel_.getLocalBounds();

    auto masterRow = masterArea.removeFromTop(26);
    autoRecButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(6);
    autoClearButton.setBounds(masterRow.removeFromRight(76));
    masterRow.removeFromRight(10);
    masterSlider.setBounds(masterRow.withTrimmedLeft(64));
    masterArea.removeFromTop(6);

    auto filterRow = masterArea.removeFromTop(26);
    filterButton.setBounds(filterRow.removeFromLeft(64));
    filterRow.removeFromLeft(6);
    filterModeBox_.setBounds(filterRow.removeFromLeft(104));
    filterRow.removeFromLeft(8);
    const int fw = juce::jmax(60, (filterRow.getWidth() - 8) / 2);
    filterCutoffSlider.setBounds(filterRow.removeFromLeft(fw));
    filterRow.removeFromLeft(8);
    filterResoSlider.setBounds(filterRow);
    masterArea.removeFromTop(6);

    auto delayRow = masterArea.removeFromTop(26);
    delayButton.setBounds(delayRow.removeFromLeft(70));
    delayRow.removeFromLeft(8);
    const int dw = juce::jmax(50, (delayRow.getWidth() - 16) / 3);
    delayTimeSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayFbSlider.setBounds(delayRow.removeFromLeft(dw));
    delayRow.removeFromLeft(8);
    delayMixSlider.setBounds(delayRow);
    masterArea.removeFromTop(6);

    auto reverbRow = masterArea.removeFromTop(26);
    reverbButton.setBounds(reverbRow.removeFromLeft(70));
    reverbRow.removeFromLeft(8);
    const int rw = juce::jmax(50, (reverbRow.getWidth() - 16) / 3);
    reverbRoomSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbDampSlider.setBounds(reverbRow.removeFromLeft(rw));
    reverbRow.removeFromLeft(8);
    reverbMixSlider.setBounds(reverbRow);
    masterArea.removeFromTop(6);

    auto eqRow = masterArea.removeFromTop(26);
    eqButton.setBounds(eqRow.removeFromLeft(70));
    eqRow.removeFromLeft(8);
    const int ew = juce::jmax(50, (eqRow.getWidth() - 16) / 3);
    eqBassSlider.setBounds(eqRow.removeFromLeft(ew));
    eqRow.removeFromLeft(8);
    eqMidSlider.setBounds(eqRow.removeFromLeft(ew));
    eqRow.removeFromLeft(8);
    eqTrebleSlider.setBounds(eqRow);
    masterArea.removeFromTop(4);
    eqCurveView_.setBounds(masterArea.removeFromTop(48));
    masterArea.removeFromTop(6);

    masterArea.removeFromTop(8);

    meter_.setBounds(masterArea.removeFromTop(44));
}

} // namespace soundsplice
