#include "PaneAudit.h"

#include <app/AnalyserPane.h>
#include <app/ApplyEffectsDialog.h>
#include <app/AudioEditorPane.h>
#include <app/MasteringPane.h>
#include <app/DrumsPane.h>
#include <app/EffectChainPanel.h>
#include <app/FileBrowserPanel.h>
#include <app/MixerStrip.h>
#include <app/SessionView.h>

using namespace looper;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** The sizes a docked pane actually gets: generous, cramped, and absurd.
        A pane is a dock tab, so its size is whatever the user's layout gives
        it — including sizes nobody designed for. */
    const juce::Rectangle<int> kSizes[] = {
        { 0, 0, 1100, 700 },
        { 0, 0, 700, 400 },
        { 0, 0, 420, 260 },
        { 0, 0, 260, 160 },
        { 0, 0, 140, 90 },
    };

    model::Song songWithEverything()
    {
        model::Song song;
        const int synth  = model::addTrack(song, model::TrackType::Instrument, "Synth").id;
        const int drums  = model::addTrack(song, model::TrackType::Drum, "Drums").id;
        model::addTrack(song, model::TrackType::Guitar, "Guitar");

        model::Clip clip;
        clip.type                = model::ClipType::Instrument;
        clip.lengthBeats         = 4.0;
        clip.pattern.lengthBeats = 4.0;
        model::addClip(song, synth, clip);
        model::addClip(song, drums, clip);

        model::addScene(song, "Intro");
        model::addScene(song, "Chorus");
        model::setSessionClip(song, 0, 0, clip);
        return song;
    }

    std::vector<model::EffectSlot> chainOfEveryKind()
    {
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
        return chain;
    }
}

TEST_CASE("The drums pane lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        DrumsPane pane;
        pane.setVisible(true);
        pane.setBounds(size);
        pane.connectCallbacks();

        engine::Pattern pattern;
        pattern.lengthBeats = 4.0;
        pane.setKit(model::makeDefaultDrumKit().pads, pattern);
        pane.resized();

        paneaudit::requireUsable(pane, "DrumsPane at " + size.toString());
    }
}

TEST_CASE("The audio editor lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        AudioEditorPane pane;
        pane.setVisible(true);
        pane.setBounds(size);
        pane.setClip(juce::File("/nonexistent/take.wav"), 12.0, 0.0f, "Audio 1", 0xff3080ff);
        pane.resized();

        paneaudit::requireUsable(pane, "AudioEditorPane at " + size.toString());
    }
}

TEST_CASE("The analyser pane lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        AnalyserPane pane;
        pane.setVisible(true);
        pane.setBounds(size);
        pane.resized();

        paneaudit::requireUsable(pane, "AnalyserPane at " + size.toString());
    }
}

TEST_CASE("The apply-effects dialog lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        ApplyEffectsDialog dialog;
        dialog.setVisible(true);
        dialog.setBounds(size);
        dialog.resized();

        paneaudit::requireUsable(dialog, "ApplyEffectsDialog at " + size.toString());
    }
}

TEST_CASE("The mastering pane lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        MasteringPane pane;
        pane.setVisible(true);
        pane.setBounds(size);
        pane.setSettings(model::MasteringSettings {});
        pane.resized();

        paneaudit::requireUsable(pane, "MasteringPane at " + size.toString());
    }
}

TEST_CASE("The audio editor is usable with no clip selected", "[gui][panes]")
{
    JuceFixture fixture;

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 700, 400);
    pane.setNoAudioClipSelected();
    pane.resized();

    paneaudit::requireUsable(pane, "AudioEditorPane with no clip");
}

TEST_CASE("The drums pane is usable with no track selected too", "[gui][panes]")
{
    // The placeholder state. A control left visible but unpositioned here
    // would be invisible in the app and impossible to notice.
    JuceFixture fixture;

    DrumsPane pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 700, 400);
    pane.setNoDrumTrackSelected();
    pane.resized();

    paneaudit::requireUsable(pane, "DrumsPane with no track");
}

TEST_CASE("The session view lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        SessionView view;
        view.setVisible(true);
        view.setBounds(size);
        view.setSong(songWithEverything());
        view.setPlayingSlots({ 0, -1, -1 });
        view.resized();

        paneaudit::requireUsable(view, "SessionView at " + size.toString());
    }
}

TEST_CASE("A mixer strip lays out usably at every size", "[gui][panes]")
{
    // Strips are packed side by side, so a narrow one is the normal case once
    // there are several tracks.
    JuceFixture fixture;

    for (int width : { 120, 90, 70, 50, 34 })
    {
        MixerStrip strip;
        strip.setVisible(true);
        strip.setBounds(0, 0, width, 320);
        strip.setTrackName("Track 1");
        strip.setGainDb(-6.0f);
        strip.resized();

        paneaudit::requireUsable(strip, "MixerStrip " + juce::String(width) + "px wide");
    }
}

TEST_CASE("The effect chain panel lays out usably for every effect kind", "[gui][panes]")
{
    // Each kind shows a different set of controls, and only the ones it uses.
    // A kind whose controls were never given bounds would look like an effect
    // with no parameters.
    JuceFixture fixture;

    const auto chain = chainOfEveryKind();

    for (const auto& size : kSizes)
    {
        for (int selected = 0; selected < (int) chain.size(); ++selected)
        {
            EffectChainPanel panel;
            panel.setVisible(true);
            panel.setBounds(size);
            panel.setChain(chain);
            panel.selectSlotForTesting(selected);
            panel.resized();

            paneaudit::requireUsable(panel, "EffectChainPanel slot " + juce::String(selected)
                                            + " at " + size.toString());
        }
    }
}

TEST_CASE("The effect chain panel is usable with no track selected", "[gui][panes]")
{
    JuceFixture fixture;

    EffectChainPanel panel;
    panel.setVisible(true);
    panel.setBounds(0, 0, 700, 400);
    panel.setNoTrackSelected();
    panel.resized();

    paneaudit::requireUsable(panel, "EffectChainPanel with no track");
}

TEST_CASE("The file browser lays out usably at every size", "[gui][panes]")
{
    JuceFixture fixture;

    for (const auto& size : kSizes)
    {
        FileBrowserPanel panel;
        panel.setVisible(true);
        panel.setBounds(size);
        panel.resized();

        paneaudit::requireUsable(panel, "FileBrowserPanel at " + size.toString());
    }
}

TEST_CASE("Controls hidden for lack of room come back when there is room", "[gui][panes]")
{
    // Hiding is only honest if it's reversible. A control dropped because the
    // pane was too narrow must return when the pane is widened, or shrinking
    // a pane once would remove the control permanently.
    JuceFixture fixture;

    MixerStrip strip;
    strip.setVisible(true);

    // The output picker only applies once a bus exists, and this test is about
    // controls hidden for lack of *room* — so it is given one, or the picker
    // would be counted as missing for a reason this test isn't checking.
    strip.setOutputOptions({ { 7, "Drum Bus" } }, -1);

    strip.setBounds(0, 0, 34, 320);
    strip.resized();
    const auto crampedFindings = paneaudit::audit(strip);
    REQUIRE(crampedFindings.empty()); // nothing unusable, just fewer things

    strip.setBounds(0, 0, 120, 320);
    strip.resized();

    // Every control is back and usable at a comfortable width.
    std::vector<juce::Component*> controls;
    paneaudit::collectControls(strip, controls);

    int visibleAndUsable = 0;
    for (auto* control : controls)
        if (paneaudit::effectivelyVisible(strip, control)
            && control->getWidth() > 0 && control->getHeight() > 0)
            ++visibleAndUsable;

    REQUIRE(visibleAndUsable == (int) controls.size());
    REQUIRE(paneaudit::audit(strip).empty());
}

TEST_CASE("The audit notices a control that isn't parented", "[gui][panes]")
{
    // The drive-pedal shape: laid out, shown, never a child. Checked here
    // because the audit is what the other tests in this file rely on, and an
    // audit that reports nothing is indistinguishable from a clean pane.
    JuceFixture fixture;

    juce::Component pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 400, 300);

    juce::TextButton orphan("Orphan");
    orphan.setVisible(true);
    orphan.setBounds(10, 10, 80, 24);

    REQUIRE_FALSE(pane.isParentOf(&orphan));            // never added
    REQUIRE_FALSE(paneaudit::effectivelyVisible(pane, &orphan));

    // And once it is a child, it registers.
    pane.addAndMakeVisible(orphan);
    REQUIRE(paneaudit::effectivelyVisible(pane, &orphan));
}

TEST_CASE("The audit notices a zero-sized control", "[gui][panes]")
{
    JuceFixture fixture;

    juce::Component pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 400, 300);

    juce::Slider squashed;
    pane.addAndMakeVisible(squashed);
    squashed.setBounds(10, 10, 0, 20);

    const auto findings = paneaudit::audit(pane);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].what == "zero-sized control");
}

TEST_CASE("The audit notices a control pushed outside its parent", "[gui][panes]")
{
    JuceFixture fixture;

    juce::Component pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 400, 300);

    juce::TextButton offscreen("Gone");
    pane.addAndMakeVisible(offscreen);
    offscreen.setBounds(900, 900, 80, 24);

    const auto findings = paneaudit::audit(pane);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].what == "control outside its parent");
}
