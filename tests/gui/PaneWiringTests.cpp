#include "PaneAudit.h"

#include <app/AnalyserPane.h>
#include <app/ApplyEffectsDialog.h>
#include <app/AudioEditorPane.h>
#include <app/MasteringPane.h>
#include <app/DrumsPane.h>
#include <app/EffectChainPanel.h>
#include <app/FileBrowserPanel.h>
#include <app/FretboardPane.h>
#include <app/MixerStrip.h>
#include <app/SessionView.h>
#include <app/SynthEditor.h>

using namespace looper;

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

    DrumsPane drums;
    drums.connectCallbacks();
    paneaudit::requireWired(drums, "DrumsPane");

    SessionView session;
    paneaudit::requireWired(session, "SessionView");

    MixerStrip strip;
    paneaudit::requireWired(strip, "MixerStrip");

    EffectChainPanel fx;
    paneaudit::requireWired(fx, "EffectChainPanel");

    FileBrowserPanel files;
    paneaudit::requireWired(files, "FileBrowserPanel");

    FretboardPane fret;
    paneaudit::requireWired(fret, "FretboardPane");

    SynthEditor synth;
    paneaudit::requireWired(synth, "SynthEditor");

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
}

TEST_CASE("A mixer strip reports every move the user makes", "[gui][wiring]")
{
    // Five separate callbacks, and the strip is the only way to reach any of
    // them. One left unwired is a fader that moves and changes nothing.
    JuceFixture fixture;

    MixerStrip strip;
    strip.setVisible(true);
    strip.setBounds(0, 0, 120, 320);
    strip.resized();

    float gain = 0.0f,  send = -1.0f, pan = -99.0f;
    bool  muted = false, soloed = false;
    int   gains = 0, sends = 0, pans = 0, mutes = 0, solos = 0, routes = 0;
    int   routedTo = -99;

    strip.onGainChange = [&](float v) { gain = v;   ++gains; };
    strip.onSendChange = [&](float v) { send = v;   ++sends; };
    strip.onPanChange  = [&](float v) { pan  = v;   ++pans;  };
    strip.onMuteChange = [&](bool  v) { muted = v;  ++mutes; };
    strip.onSoloChange = [&](bool  v) { soloed = v; ++solos; };
    strip.onOutputBusChange = [&](int busId) { routedTo = busId; ++routes; };

    // The output picker only appears once there is a bus to pick, so it has to
    // be given one here — otherwise this test would silently stop covering it.
    strip.setOutputOptions({ { 7, "Drum Bus" } }, -1);
    strip.resized();

    std::vector<juce::Component*> controls;
    paneaudit::collectControls(strip, controls);

    // Each control on its own, and each must report something: five callbacks
    // checked together would let a dead fader hide behind a live one.
    int reportsBefore = 0;
    for (auto* control : controls)
    {
        reportsBefore = gains + sends + pans + mutes + solos + routes;

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
        REQUIRE(gains + sends + pans + mutes + solos + routes > reportsBefore);
    }

    INFO("gain " << gains << " send " << sends << " pan " << pans
         << " mute " << mutes << " solo " << solos << " route " << routes);
    REQUIRE(gains > 0);
    REQUIRE(sends > 0);
    REQUIRE(pans > 0);
    REQUIRE(mutes > 0);
    REQUIRE(solos > 0);
    REQUIRE(routes > 0);

    // The values reported are the ones the controls were set to, not defaults.
    REQUIRE(muted);
    REQUIRE(soloed);
    REQUIRE(pan != -99.0f);
    REQUIRE(send >= 0.0f);
    // The bus's id, not its position in the list — the encoding the picker
    // uses is exactly where a routing silently points at the wrong track.
    REQUIRE(routedTo == 7);
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

TEST_CASE("The synth editor reports its parameter changes", "[gui][wiring]")
{
    JuceFixture fixture;

    SynthEditor editor;
    editor.setVisible(true);
    editor.setBounds(0, 0, 700, 420);

    // setSettings is what reveals the controls — without it the editor shows
    // its placeholder and the sweep below finds nothing to check. The
    // checked > 0 assertion is there so that state fails loudly rather than
    // passing vacuously, which is what it did on the first run of this test.
    editor.setSettings(model::SynthSettings {});
    editor.resized();

    int reports = 0;
    editor.onSettingsChanged = [&reports](const model::SynthSettings&) { ++reports; };

    std::vector<juce::Component*> controls;
    paneaudit::collectControls(editor, controls);

    // Per control, for the same reason as the effect panel: any-of-them is
    // satisfied by a single wired slider among a dozen dead ones.
    int checked = 0;
    for (auto* control : controls)
    {
        auto* slider = dynamic_cast<juce::Slider*>(control);
        if (slider == nullptr || ! paneaudit::effectivelyVisible(editor, control))
            continue;

        const int before = reports;
        slider->setValue(slider->getMinimum()
                         + (slider->getMaximum() - slider->getMinimum()) * 0.6);
        pump();

        INFO("synth slider " << (slider->getName().isEmpty() ? juce::String("(unnamed)")
                                                             : slider->getName())
             << " reported " << (reports - before) << " change(s)");
        REQUIRE(reports > before);
        ++checked;
    }

    REQUIRE(checked > 0);
}

TEST_CASE("The synth editor brackets every slider drag", "[gui][wiring]")
{
    // Same gap as the effect panel's equivalent test: onSettingsChanged
    // firing (checked above) doesn't prove onSettingsDragStart/End also
    // fire, and those are what make a drag undoable rather than merely live.
    JuceFixture fixture;

    SynthEditor editor;
    editor.setVisible(true);
    editor.setBounds(0, 0, 700, 420);
    editor.setSettings(model::SynthSettings {});
    editor.resized();

    int starts = 0, ends = 0;
    editor.onSettingsDragStart = [&] { ++starts; };
    editor.onSettingsDragEnd   = [&] { ++ends; };

    std::vector<juce::Component*> controls;
    paneaudit::collectControls(editor, controls);

    int checked = 0;
    for (auto* control : controls)
    {
        auto* slider = dynamic_cast<juce::Slider*>(control);
        if (slider == nullptr || ! paneaudit::effectivelyVisible(editor, control))
            continue;

        REQUIRE(slider->onDragStart);
        REQUIRE(slider->onDragEnd);

        const int startsBefore = starts, endsBefore = ends;
        slider->onDragStart();
        slider->onDragEnd();

        INFO("synth slider " << (slider->getName().isEmpty() ? juce::String("(unnamed)")
                                                             : slider->getName()));
        REQUIRE(starts == startsBefore + 1);
        REQUIRE(ends == endsBefore + 1);
        ++checked;
    }

    REQUIRE(checked > 0);
}

TEST_CASE("The fretboard brackets every slider drag", "[gui][wiring]")
{
    // Same gap as the synth editor's and effect panel's equivalent tests:
    // onSettingsChanged firing doesn't prove onSettingsDragStart/End also
    // fire, and those are what make a drag undoable rather than merely live.
    JuceFixture fixture;

    FretboardPane pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 900, 600);
    pane.setSettings(model::GuitarSettings {});
    pane.resized();

    int starts = 0, ends = 0;
    pane.onSettingsDragStart = [&] { ++starts; };
    pane.onSettingsDragEnd   = [&] { ++ends; };

    std::vector<juce::Component*> controls;
    paneaudit::collectControls(pane, controls);

    int checked = 0;
    for (auto* control : controls)
    {
        auto* slider = dynamic_cast<juce::Slider*>(control);
        if (slider == nullptr || ! paneaudit::effectivelyVisible(pane, control))
            continue;

        REQUIRE(slider->onDragStart);
        REQUIRE(slider->onDragEnd);

        const int startsBefore = starts, endsBefore = ends;
        slider->onDragStart();
        slider->onDragEnd();

        INFO("fretboard slider " << (slider->getName().isEmpty() ? juce::String("(unnamed)")
                                                                  : slider->getName()));
        REQUIRE(starts == startsBefore + 1);
        REQUIRE(ends == endsBefore + 1);
        ++checked;
    }

    REQUIRE(checked > 0);
}

TEST_CASE("The drums pane reports adding and removing a pad", "[gui][wiring]")
{
    // connectCallbacks() wires the two halves through to the pane's own
    // outputs. If it missed one, the button would work internally and the app
    // would never hear about it.
    JuceFixture fixture;

    DrumsPane pane;
    pane.setVisible(true);
    pane.setBounds(0, 0, 900, 500);
    pane.connectCallbacks();

    engine::Pattern pattern;
    pattern.lengthBeats = 4.0;
    pane.setKit(model::makeDefaultDrumKit().pads, pattern);
    pane.resized();

    int added = 0, removed = 0;
    pane.onPadAdded   = [&added] { ++added; };
    pane.onPadRemoved = [&removed](int) { ++removed; };

    std::vector<juce::Component*> controls;
    paneaudit::collectControls(pane, controls);

    for (auto* control : controls)
    {
        const auto name = control->getName();
        if (name == "Add Pad" || name == "Remove Pad")
            if (auto* button = dynamic_cast<juce::Button*>(control))
                button->triggerClick();
    }

    pump();

    INFO("added " << added << " removed " << removed);
    REQUIRE(added > 0);
    REQUIRE(removed > 0);
}
