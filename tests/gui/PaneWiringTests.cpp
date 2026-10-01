#include "PaneAudit.h"

#include <app/LevelMeter.h>
#include <app/AnalyserPane.h>
#include <app/ApplyEffectsDialog.h>
#include <app/AudioEditorPane.h>
#include <app/MasteringPane.h>
#include <app/EffectChainPanel.h>
#include <app/FileBrowserPanel.h>
#include <app/MixerStrip.h>
#include <app/OpenFilesPane.h>
#include <app/SessionView.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** Button::triggerClick posts a message rather than calling back directly,
        so the queue has to be pumped before the callback has run. */
    void pump(int ms = 60)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
    }
}

TEST_CASE("Every pane's controls have something listening to them", "[gui][wiring]")
{
    // The other half of the layout audit. A control can be parented, sized and
    // hit-testable and still do nothing, because whoever added it never
    // assigned its callback — which is how the drive pedal's controls existed
    // for a commit while being unreachable.
    //
    // Read-on-demand controls are assigned an empty handler explicitly, so
    // "deliberately not reacted to" stays distinguishable from "nobody ever
    // wired this".
    JuceFixture fixture;

    SessionView session;
    paneaudit::requireWired(session, "SessionView");

    MixerStrip strip;
    paneaudit::requireWired(strip, "MixerStrip");

    EffectChainPanel fx;
    paneaudit::requireWired(fx, "EffectChainPanel");

    FileBrowserPanel files;
    paneaudit::requireWired(files, "FileBrowserPanel");

    AudioEditorPane audio;
    // Shown against a file that doesn't exist: the pane must still parent and
    // wire everything, since that's exactly the state it's in for the frames
    // before a thumbnail loads (and permanently, for a clip whose file has
    // been moved).
    audio.setClip(juce::File("/nonexistent/take.wav"), 12.0, 0.0f, "Audio 1", 0xff3080ff);
    paneaudit::requireWired(audio, "AudioEditorPane");

    MasteringPane mastering;
    paneaudit::requireWired(mastering, "MasteringPane");

    ApplyEffectsDialog effects;
    paneaudit::requireWired(effects, "ApplyEffectsDialog");

    AnalyserPane analyser;
    paneaudit::requireWired(analyser, "AnalyserPane");

    OpenFilesPane openFiles;
    openFiles.setEntries({ { 1, "Take", "Vox", juce::Colours::orange } }, 1);
    paneaudit::requireWired(openFiles, "OpenFilesPane");
}

TEST_CASE("The open files pane reports choosing and closing", "[gui][wiring]")
{
    JuceFixture fixture;

    OpenFilesPane pane;
    pane.setEntries({ { 4, "Take", "Vox", juce::Colours::orange }, { 9, "Room", "Vox", juce::Colours::orange } }, 9);
    pane.setVisible(true);
    pane.setBounds(0, 0, 300, 300);
    pane.resized();

    int  closed    = 0;
    bool closedAll = false;
    pane.onClosed   = [&closed](int id) { closed = id; };
    pane.onCloseAll = [&closedAll] { closedAll = true; };

    // Close means the one showing.
    for (auto* child : pane.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child))
            button->triggerClick();
    pump();

    REQUIRE(closed == 9);
    REQUIRE(closedAll);
}

TEST_CASE("A mixer strip reports every move the user makes", "[gui][wiring]")
{
    // Four separate callbacks, and the strip is the only way to reach any of
    // them. One left unwired is a fader that moves and changes nothing.
    JuceFixture fixture;

    MixerStrip strip;
    strip.setVisible(true);
    strip.setBounds(0, 0, 120, 320);
    strip.resized();

    float gain = 0.0f, pan = -99.0f;
    bool  muted = false, soloed = false;
    int   gains = 0, pans = 0, mutes = 0, solos = 0, arms = 0;

    strip.onGainChange = [&](float v) { gain = v;   ++gains; };
    strip.onPanChange  = [&](float v) { pan  = v;   ++pans;  };
    strip.onMuteChange = [&](bool  v) { muted = v;  ++mutes; };
    strip.onSoloChange = [&](bool  v) { soloed = v; ++solos; };
    strip.onArmChange  = [&](bool)    { ++arms; };
    strip.onAutomationModeMenuRequested = [&] { ++arms; }; // counted with the R button's
    strip.onOutputMenuRequested = [&] { ++arms; };          // and the routing buttons
    strip.onSendsMenuRequested  = [&] { ++arms; };

    std::vector<juce::Component*> controls;
    paneaudit::collectControls(strip, controls);

    // Each control on its own, and each must report something: four callbacks
    // checked together would let a dead fader hide behind a live one.
    int reportsBefore = 0;
    for (auto* control : controls)
    {
        reportsBefore = gains + pans + mutes + solos + arms;

        if (auto* slider = dynamic_cast<juce::Slider*>(control))
            slider->setValue(slider->getMinimum()
                             + (slider->getMaximum() - slider->getMinimum()) * 0.25);
        else if (auto* button = dynamic_cast<juce::Button*>(control))
            button->triggerClick();
        else if (auto* box = dynamic_cast<juce::ComboBox*>(control))
            box->setSelectedItemIndex(box->getSelectedItemIndex() == 0 ? 1 : 0);

        pump();

        INFO("control " << (control->getName().isEmpty() ? juce::String("(unnamed)")
                                                         : control->getName())
             << " reported nothing");
        REQUIRE(gains + pans + mutes + solos + arms > reportsBefore);
    }

    INFO("gain " << gains << " pan " << pans << " mute " << mutes << " solo " << solos);
    REQUIRE(gains > 0);
    REQUIRE(pans > 0);
    REQUIRE(mutes > 0);
    REQUIRE(solos > 0);

    // The values reported are the ones the controls were set to, not defaults.
    REQUIRE(muted);
    REQUIRE(soloed);
    REQUIRE(pan != -99.0f);
}

TEST_CASE("The effect panel reports a parameter change for every kind", "[gui][wiring]")
{
    // Each kind shows its own controls. A kind whose controls were never
    // hooked to pushParams would be an effect whose knobs move and do nothing
    // — and each kind has to be checked, since they are wired separately.
    JuceFixture fixture;

    std::vector<model::EffectSlot> chain;
    for (auto kind : { model::EffectKind::Filter, model::EffectKind::Delay,
                       model::EffectKind::Reverb, model::EffectKind::Drive,
                       model::EffectKind::Compressor, model::EffectKind::Tremolo,
                       model::EffectKind::Chorus, model::EffectKind::Wobble })
    {
        model::EffectSlot slot;
        slot.kind    = kind;
        slot.enabled = true;
        chain.push_back(slot);
    }

    for (int selected = 0; selected < (int) chain.size(); ++selected)
    {
        EffectChainPanel panel;
        panel.setVisible(true);
        panel.setBounds(0, 0, 700, 420);
        panel.setChain(chain);
        panel.selectSlotForTesting(selected);
        panel.resized();

        int reports = 0;
        panel.onSlotParamsChanged = [&reports](const model::EffectSlot&, int) { ++reports; };

        std::vector<juce::Component*> controls;
        paneaudit::collectControls(panel, controls);

        // One control at a time. Nudging them all together and asking for any
        // report at all is too weak: a kind with five controls would pass with
        // four of them wired, which is exactly the bug being looked for.
        int checked = 0;
        for (auto* control : controls)
        {
            if (! paneaudit::effectivelyVisible(panel, control))
                continue;

            auto* slider = dynamic_cast<juce::Slider*>(control);
            if (slider == nullptr)
                continue;

            const int before = reports;
            slider->setValue(slider->getMinimum()
                             + (slider->getMaximum() - slider->getMinimum()) * 0.4);
            pump();

            INFO("effect kind index " << selected << ", slider "
                 << (slider->getName().isEmpty() ? juce::String("(unnamed)") : slider->getName())
                 << " reported " << (reports - before) << " change(s)");
            REQUIRE(reports > before);
            ++checked;
        }

        INFO("effect kind index " << selected << " showed " << checked << " slider(s)");
        REQUIRE(checked > 0); // a kind showing no controls would pass vacuously
    }
}

TEST_CASE("The effect panel brackets every slider drag, for every kind", "[gui][wiring]")
{
    // A slider that fires onSlotParamsChanged (checked above) but never
    // reports onSlotParamsDragStart/End would still commit live values while
    // dragging and simply never become undoable — the same class of gap
    // "wired to move the value" and "wired to be undoable" leaves open.
    // juce::Slider's onDragStart/onDragEnd are ordinary std::function
    // members, so they're called directly here rather than needing a real
    // simulated mouse drag.
    JuceFixture fixture;

    std::vector<model::EffectSlot> chain;
    for (auto kind : { model::EffectKind::Filter, model::EffectKind::Delay,
                       model::EffectKind::Reverb, model::EffectKind::Drive,
                       model::EffectKind::Compressor, model::EffectKind::Tremolo,
                       model::EffectKind::Chorus, model::EffectKind::Wobble })
    {
        model::EffectSlot slot;
        slot.kind    = kind;
        slot.enabled = true;
        chain.push_back(slot);
    }

    for (int selected = 0; selected < (int) chain.size(); ++selected)
    {
        EffectChainPanel panel;
        panel.setVisible(true);
        panel.setBounds(0, 0, 700, 420);
        panel.setChain(chain);
        panel.selectSlotForTesting(selected);
        panel.resized();

        int starts = 0, ends = 0;
        int startedSlot = -1, endedSlot = -1;
        panel.onSlotParamsDragStart = [&](int i) { ++starts; startedSlot = i; };
        panel.onSlotParamsDragEnd   = [&](int i) { ++ends; endedSlot = i; };

        std::vector<juce::Component*> controls;
        paneaudit::collectControls(panel, controls);

        int checked = 0;
        for (auto* control : controls)
        {
            if (! paneaudit::effectivelyVisible(panel, control))
                continue;

            auto* slider = dynamic_cast<juce::Slider*>(control);
            if (slider == nullptr)
                continue;

            REQUIRE(slider->onDragStart);
            REQUIRE(slider->onDragEnd);

            const int startsBefore = starts, endsBefore = ends;
            slider->onDragStart();
            slider->onDragEnd();

            INFO("effect kind index " << selected << ", slider "
                 << (slider->getName().isEmpty() ? juce::String("(unnamed)") : slider->getName()));
            REQUIRE(starts == startsBefore + 1);
            REQUIRE(ends == endsBefore + 1);
            REQUIRE(startedSlot == selected);
            REQUIRE(endedSlot == selected);
            ++checked;
        }

        INFO("effect kind index " << selected << " showed " << checked << " slider(s)");
        REQUIRE(checked > 0);
    }
}


TEST_CASE("The analyser pane asks for loudness and shows what was measured", "[gui][wiring]")
{
    JuceFixture fixture;

    AnalyserPane pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 900, 300);
    pane.resized();

    bool asked = false;
    pane.onLoudnessRequested = [&asked] { asked = true; };
    for (auto* child : pane.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child); button != nullptr
            && button->getButtonText() == "Measure Loudness")
            button->triggerClick();
    pump();
    REQUIRE(asked);

    soundsplice::engine::LoudnessReport report;
    report.integratedLufs  = -16.04;
    report.loudnessRangeLu = 6.2;
    report.truePeakDb      = -1.26;
    pane.setLoudness(report.withGain(2.0));

    const auto text = pane.loudnessText();
    REQUIRE(text.contains("Integrated -14.0 LUFS"));
    REQUIRE(text.contains("Range 6.2 LU"));
    REQUIRE(text.contains("True peak 0.7 dBTP"));
    REQUIRE(text.contains("Max momentary -inf LUFS"));
}

TEST_CASE("The effect panel's Track/Clip switch is wired and survives an empty clip scope", "[gui][wiring]")
{
    JuceFixture fixture;

    EffectChainPanel panel;
    panel.setVisible(true);
    panel.setBounds(0, 0, 700, 420);
    panel.setChain({ model::makeEffectSlot(model::EffectKind::Filter) });

    juce::TextButton* scope = nullptr;
    for (auto* child : panel.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child); button != nullptr && button->getButtonText() == "Track FX")
            scope = button;
    REQUIRE(scope != nullptr);
    REQUIRE(paneaudit::effectivelyVisible(panel, scope));

    int  changes = 0;
    bool toClip  = false;
    panel.onScopeChanged = [&](bool clip) { ++changes; toClip = clip; };

    scope->triggerClick();
    pump();
    REQUIRE(changes == 1);
    REQUIRE(toClip);
    REQUIRE(panel.clipScope());
    REQUIRE(scope->getButtonText() == "Clip FX");

    // No audio clip to edit: the list goes, the way back to Track doesn't.
    panel.setNoClipSelected();
    REQUIRE(scope->isVisible());
    scope->triggerClick();
    pump();
    REQUIRE_FALSE(panel.clipScope());

    panel.setNoTrackSelected();
    REQUIRE_FALSE(scope->isVisible());
}

TEST_CASE("The input meter's clip light stays lit until it's clicked", "[gui][wiring]")
{
    JuceFixture fixture;

    LevelMeter meter;
    meter.setShowsClipping(true);
    meter.setBounds(0, 0, 90, 16);
    bool clicks = false, childClicks = false;
    meter.getInterceptsMouseClicks(clicks, childClicks);
    REQUIRE(clicks); // the click has to reach it

    meter.setClipped(1);
    for (int i = 0; i < 100; ++i) // long after the level itself has fallen
        meter.setLevel(1, 0.0f);
    REQUIRE(meter.isClipped(1));
    REQUIRE_FALSE(meter.isClipped(0));

    meter.mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 1.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, &meter, &meter, juce::Time::getCurrentTime(), {},
                                     juce::Time::getCurrentTime(), 1, false));
    REQUIRE_FALSE(meter.isClipped(1));

    // Both ways round draw without trouble, clip light and all.
    juce::Image image(juce::Image::ARGB, 90, 60, true);
    juce::Graphics g(image);
    meter.setClipped(0);
    meter.setLevel(0, 0.5f);
    meter.setHorizontal(true);
    meter.paint(g);
    meter.setHorizontal(false);
    meter.setBounds(0, 0, 20, 60);
    meter.paint(g);
}
