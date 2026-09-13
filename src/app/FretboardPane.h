#pragma once

#include <array>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

#include "engine/GuitarChords.h"
#include "engine/GuitarTone.h"
#include "engine/MidiNote.h"
#include "model/GuitarSettings.h"
#include "model/Track.h"

#include "TrackColours.h"

namespace looper
{
/**
    The guitar's own interface: six strings across, frets down the neck.

    A piano roll can express the notes a guitar plays but not *how* it plays
    them — which string, which fret, and the fact that only one note sounds per
    string at a time. The fretboard is the view where that is self-evident: the
    note currently ringing on each string is lit, so a second note on the same
    string visibly takes the first one's place rather than mysteriously
    silencing it.

    Laid out like tablature, high E at the top and low E at the bottom, because
    that is the orientation anyone reading guitar notation already has.

    Owns no document state: it draws from a settings snapshot plus the engine's
    ringing-note readout, and reports intent through the callbacks.
*/
class FretboardPane final : public juce::Component
{
public:
    std::function<void(int midiNote)>                    onFretPlayed;
    std::function<void(const model::GuitarSettings&)>    onSettingsChanged;

    /** Brackets a change to the settings, so the owner can commit the whole
        drag as one undo step — same pair as SynthEditor's, for the same
        reason: settings are one struct changed as a unit. */
    std::function<void()> onSettingsDragStart;
    std::function<void()> onSettingsDragEnd;

    /** Stamps a strummed chord into the open clip. The pane hands over the
        shape and how to strike it; the owner decides where in the pattern it
        lands (see MainComponent::stampChord). */
    std::function<void(const engine::ChordShape&, int fretOffset,
                       const engine::StrumSettings&)>    onChordStamped;

    /** Fired when a fret is clicked while a chord mode is selected: the shape
        rooted on that string at that fret. @p writeToClip says whether the
        user asked for it to be recorded as well as heard — see the Write
        toggle, which is deliberately explicit rather than a modifier key. */
    std::function<void(engine::MovableShape shape, int rootString, int fret,
                       const engine::StrumSettings&, bool writeToClip)> onChordAtFret;

    /** A one-click starting tone: settings plus an effect chain tuned for
        the picked engine::GuitarTone — see model::presetForGuitarTone,
        which is what actually knows the values, the same way this pane
        never has document mutation logic of its own for anything else
        either. */
    std::function<void(engine::GuitarTone)> onGuitarToneRequested;

    FretboardPane()
    {
        placeholder_.setText("Select a Guitar track to play it", juce::dontSendNotification);
        placeholder_.setJustificationType(juce::Justification::centred);
        placeholder_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholder_);

        // Tuning, one box per string. Explicit note names rather than a
        // custom click-to-nudge gesture: there's no hit-testing to get wrong,
        // and "drop D" is then visibly one box changed.
        for (int s = 0; s < model::kNumGuitarStrings; ++s)
        {
            auto& box = tuningBoxes_[(size_t) s];
            for (int note = kLowestTuning; note <= kHighestTuning; ++note)
                box.addItem(engine::midiNoteName(note), note); // ids are the note numbers
            box.onChange = [this] { reportInstantEdit(); };
            addChildComponent(box);
        }

        // Chord palette: one button per shape. Stamping writes real notes at
        // real times into the clip rather than a "strum" flag, so what you get
        // is editable afterwards — the same choice §18's swing made.
        for (int i = 0; i < engine::kNumChordShapes; ++i)
        {
            auto* button = chordButtons_.add(new juce::TextButton(engine::kChordShapes[i].name));
            button->onClick = [this, i] { stampChord(i); };
            addChildComponent(button);
        }

        // Chord mode: what a click on the neck means. "Single note" keeps the
        // original behaviour, so the board is still a board.
        chordMode_.addItem("Single note", 1);
        for (int i = 0; i < engine::kNumMovableShapes; ++i)
            chordMode_.addItem(engine::movableShapeName((engine::MovableShape) i), i + 2);
        chordMode_.setSelectedId(1, juce::dontSendNotification);
        chordMode_.setTooltip("What clicking a fret plays: one note, or a chord rooted there");
        // Read when a fret is clicked, not reacted to. Said explicitly rather
        // than left unassigned, so a control nobody reads at all is still
        // distinguishable from one deliberately read on demand.
        chordMode_.onChange = [] {};
        addChildComponent(chordMode_);

        writeToClip_.setButtonText("Write");
        writeToClip_.setTooltip("Also write the chord into the open clip, not just play it");
        writeToClip_.onClick = [] {}; // read on demand — see chordMode_ above
        addChildComponent(writeToClip_);

        strumDirection_.addItem("Down", 1);
        strumDirection_.addItem("Up", 2);
        strumDirection_.setSelectedId(1, juce::dontSendNotification);
        strumDirection_.onChange = [] {}; // read on demand — see chordMode_ above
        addChildComponent(strumDirection_);

        setupSlider(strumSpread_, 0.0, 60.0, 1.0, " ms", [] {});
        strumSpread_.setValue(18.0, juce::dontSendNotification);
        setupSlider(strumHumanise_, 0.0, 100.0, 1.0, " %", [] {});
        setupLabel(strumSpreadLabel_, "Spread");
        setupLabel(strumHumaniseLabel_, "Feel");

        setupSlider(decay_, 0.2, 12.0, 0.1, " s", [this] { pushSettings(); });
        setupSlider(brightness_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(pickPosition_, 2.0, 50.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(pickHardness_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(velocitySense_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(coupling_,      0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(stiffness_,     0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(width_,         0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(muteOnRelease_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(pickupHz_, 500.0, 8000.0, 10.0, " Hz", [this] { pushSettings(); });
        setupSlider(pickupQ_, 0.5, 4.0, 0.05, "", [this] { pushSettings(); });
        setupSlider(palmDecay_, 0.05, 1.0, 0.01, " s", [this] { pushSettings(); });
        palmDecay_.setTooltip("How long a palm-muted note rings - the length of a chug");
        setupSlider(palmBright_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        palmBright_.setTooltip("How dark a palm-muted note is - the picking hand rolls off the top");

        setupLabel(decayLabel_, "Decay");
        setupLabel(brightnessLabel_, "Bright");
        setupLabel(pickPositionLabel_, "Pick pos");
        setupLabel(pickHardnessLabel_, "Pick");
        setupLabel(velocitySenseLabel_, "Dynamics");
        setupLabel(couplingLabel_, "Coupling");
        setupLabel(stiffnessLabel_, "Stiffness");
        setupLabel(widthLabel_, "Width");
        setupLabel(muteOnReleaseLabel_, "Damp off");
        setupLabel(pickupHzLabel_, "Pickup");
        setupLabel(pickupQLabel_, "Pickup Q");
        setupLabel(palmDecayLabel_, "Mute len");
        setupLabel(palmBrightLabel_, "Mute tone");

        // Tone templates: one button per engine::GuitarTone, each applying
        // its whole GuitarSettings + effect chain in one click. Laid out as
        // one row, same "divide what's actually there" technique as
        // chordButtons_ above, rather than stacked - five buttons stacked
        // would nearly double kToneHeight for no benefit over a row.
        for (int i = 0; i < engine::kNumGuitarTones; ++i)
        {
            const auto tone   = (engine::GuitarTone) i;
            auto*      button = toneButtons_.add(new juce::TextButton(engine::guitarToneName(tone)));
            button->onClick   = [this, tone] { if (onGuitarToneRequested) onGuitarToneRequested(tone); };
            addChildComponent(button);
        }

        // Same guarantee as EffectChainPanel: a control that was never
        // parented lays out and hides perfectly while drawing nothing.
        for (auto* control : managedControls())
        {
            jassert(getIndexOfChildComponent(control) >= 0);
            addChildComponent(control);
        }

        setContentVisible(false);
    }

    void setSettings(const model::GuitarSettings& settings)
    {
        settings_ = settings;
        updating_ = true;

        for (int s = 0; s < model::kNumGuitarStrings; ++s)
            tuningBoxes_[(size_t) s].setSelectedId(settings.tuning[(size_t) s], juce::dontSendNotification);

        decay_.setValue(settings.decaySeconds, juce::dontSendNotification);
        brightness_.setValue(settings.brightness * 100.0, juce::dontSendNotification);
        pickPosition_.setValue(settings.pickPosition * 100.0, juce::dontSendNotification);
        pickHardness_.setValue(settings.pickHardness * 100.0, juce::dontSendNotification);
        velocitySense_.setValue(settings.velocitySensitivity * 100.0, juce::dontSendNotification);
        coupling_.setValue(settings.stringCoupling * 100.0, juce::dontSendNotification);
        stiffness_.setValue(settings.stiffness * 100.0, juce::dontSendNotification);
        width_.setValue(settings.stereoWidth * 100.0, juce::dontSendNotification);
        muteOnRelease_.setValue(settings.muteOnNoteOff * 100.0, juce::dontSendNotification);
        pickupHz_.setValue(settings.pickupResonanceHz, juce::dontSendNotification);
        pickupQ_.setValue(settings.pickupQ, juce::dontSendNotification);
        palmDecay_.setValue(settings.palmMuteDecaySeconds, juce::dontSendNotification);
        palmBright_.setValue(settings.palmMuteBrightness * 100.0, juce::dontSendNotification);

        updating_ = false;
        setContentVisible(true);
    }

    void setNoGuitarTrackSelected() { setContentVisible(false); }

    /** What each string is currently sounding, straight from the engine — the
        pane can't infer it, since a string keeps ringing after its note-off. */
    void setRingingNotes(const std::array<int, model::kNumGuitarStrings>& notes)
    {
        if (ringing_ != notes)
        {
            ringing_ = notes;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! contentVisible_)
            return;

        int stringIndex = 0, fret = 0;
        if (! fretAt(e.position, stringIndex, fret))
            return;

        if (const auto shape = selectedShape())
        {
            if (onChordAtFret)
                onChordAtFret(*shape, stringIndex, fret, currentStrum(), writeToClip_.getToggleState());
            return;
        }

        const int note = settings_.tuning[(size_t) stringIndex] + fret;
        if (note >= 0 && note <= 127 && onFretPlayed)
            onFretPlayed(note);
    }

    /** The chord mode, or nothing when the board is in single-note mode. */
    std::optional<engine::MovableShape> selectedShape() const
    {
        const int id = chordMode_.getSelectedId();
        if (id <= 1)
            return {};
        return (engine::MovableShape) (id - 2);
    }

    engine::StrumSettings currentStrum() const
    {
        engine::StrumSettings strum;
        strum.downstroke = strumDirection_.getSelectedId() != 2;
        strum.spreadMs   = strumSpread_.getValue();
        strum.humanise   = strumHumanise_.getValue() / 100.0;
        return strum;
    }

    /** Which track this is, so the header says so — this tab's title never
        changes per track, so without this there was no on-screen way to
        tell which track's fretboard was actually open after switching
        tracks while parked here. */
    void setTrackInfo(const juce::String& name, juce::uint32 colour)
    {
        trackName_   = name;
        trackColour_ = colour;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));
        if (! contentVisible_)
            return;

        paintTrackHeader(g, getLocalBounds().removeFromTop(kTrackHeaderHeight),
                         trackName_, trackColour_, model::TrackType::Guitar);

        const auto board = boardArea();
        if (board.getHeight() <= 0 || board.getWidth() <= 0)
            return;

        const float rowHeight = (float) board.getHeight() / (float) model::kNumGuitarStrings;
        const float fretWidth = (float) board.getWidth() / (float) (kNumFrets + 1);

        // Position dots, where they sit on a real neck — the only thing that
        // makes a grid of identical cells navigable at a glance.
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        for (int fret : { 3, 5, 7, 9, 12, 15, 17, 19, 21, 24 })
        {
            const auto x = (float) board.getX() + (float) fret * fretWidth;
            g.fillRect(juce::Rectangle<float>(x, (float) board.getY(), fretWidth, (float) board.getHeight()));
        }

        for (int row = 0; row < model::kNumGuitarStrings; ++row)
        {
            // Row 0 is the top of the display, which in tab is the *highest*
            // string — so the display order is the reverse of the model's.
            const int   stringIndex = model::kNumGuitarStrings - 1 - row;
            const float y           = (float) board.getY() + (float) row * rowHeight;
            const int   openNote    = settings_.tuning[(size_t) stringIndex];
            const int   sounding    = ringing_[(size_t) stringIndex];

            // Thicker line for the lower strings, as on the instrument.
            g.setColour(juce::Colours::white.withAlpha(0.22f));
            g.fillRect((float) board.getX(), y + rowHeight * 0.5f,
                       (float) board.getWidth(), 1.0f + 0.4f * (float) stringIndexToThickness(stringIndex));

            for (int fret = 0; fret <= kNumFrets; ++fret)
            {
                const auto cell = juce::Rectangle<float>((float) board.getX() + (float) fret * fretWidth,
                                                         y, fretWidth, rowHeight).reduced(1.0f);

                if (sounding == openNote + fret)
                {
                    // The note this string is actually ringing right now.
                    g.setColour(juce::Colours::limegreen);
                    g.fillRoundedRectangle(cell, 3.0f);
                    g.setColour(juce::Colours::black.withAlpha(0.7f));
                }
                else if (fret == hoverFret_ && stringIndex == hoverString_)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.14f));
                    g.fillRoundedRectangle(cell, 3.0f);
                    g.setColour(juce::Colours::white.withAlpha(0.8f));
                }
                else
                {
                    g.setColour(juce::Colours::white.withAlpha(fret == 0 ? 0.55f : 0.28f));
                }

                g.setFont(juce::FontOptions(9.5f));
                g.drawText(engine::midiNoteName(openNote + fret), cell, juce::Justification::centred);
            }
        }

        // Nut: the open-string column is the instrument, not a fret.
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.fillRect((float) board.getX() + fretWidth - 1.0f, (float) board.getY(),
                   2.0f, (float) board.getHeight());
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int stringIndex = -1, fret = -1;
        if (! fretAt(e.position, stringIndex, fret))
            stringIndex = fret = -1;

        if (stringIndex != hoverString_ || fret != hoverFret_)
        {
            hoverString_ = stringIndex;
            hoverFret_   = fret;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hoverString_ = hoverFret_ = -1;
        repaint();
    }

    void resized() override
    {
        placeholder_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight); // matches boardArea() and the strip paint() draws
        area = area.reduced(6);

        auto tuningRow = area.removeFromTop(kTuningHeight);
        const int boxWidth = juce::jmax(40, tuningRow.getWidth() / model::kNumGuitarStrings);
        for (int s = 0; s < model::kNumGuitarStrings; ++s)
        {
            // Left to right as low-to-high, matching how a player names them
            // ("E A D G B e"), even though the board draws high at the top.
            tuningBoxes_[(size_t) s].setBounds(tuningRow.removeFromLeft(boxWidth).reduced(2));
        }

        auto chordRow = area.removeFromBottom(kChordHeight);
        {
            auto modeRow = chordRow.removeFromBottom(22);
            chordMode_.setBounds(modeRow.removeFromLeft(110).reduced(1));
            writeToClip_.setBounds(modeRow.removeFromLeft(70).reduced(1));

            auto strumRow = chordRow.removeFromBottom(22);
            strumDirection_.setBounds(strumRow.removeFromLeft(70).reduced(1));
            strumSpreadLabel_.setBounds(strumRow.removeFromLeft(46));
            strumSpread_.setBounds(strumRow.removeFromLeft(juce::jmax(90, strumRow.getWidth() / 2)).reduced(2, 1));
            strumHumaniseLabel_.setBounds(strumRow.removeFromLeft(36));
            strumHumanise_.setBounds(strumRow.reduced(2, 1));

            // Divide what's actually there rather than imposing a minimum
            // width: a minimum overflows the row in a narrow pane, and the
            // buttons past the edge get zero width — present, hit-testable at
            // nothing, and indistinguishable from a button that doesn't work.
            // Recomputing per button spreads the remainder instead of leaving
            // it all on the last one.
            for (int i = 0; i < chordButtons_.size(); ++i)
            {
                const int remaining = chordButtons_.size() - i;
                const int width     = juce::jmax(1, chordRow.getWidth() / remaining);
                chordButtons_[i]->setBounds(chordRow.removeFromLeft(width).reduced(1));
            }
        }

        auto tone = area.removeFromBottom(kToneHeight);
        {
            auto toneRow = tone.removeFromTop(22);
            for (int i = 0; i < toneButtons_.size(); ++i)
            {
                const int remaining = toneButtons_.size() - i;
                const int width     = juce::jmax(1, toneRow.getWidth() / remaining);
                toneButtons_[i]->setBounds(toneRow.removeFromLeft(width).reduced(1));
            }
        }
        auto row  = [&tone](juce::Label& label, juce::Slider& slider)
        {
            auto r = tone.removeFromTop(22);
            label.setBounds(r.removeFromLeft(64));
            slider.setBounds(r.reduced(2, 1));
        };
        row(decayLabel_, decay_);
        row(brightnessLabel_, brightness_);
        row(pickPositionLabel_, pickPosition_);
        row(pickHardnessLabel_, pickHardness_);
        row(velocitySenseLabel_, velocitySense_);
        row(couplingLabel_, coupling_);
        row(stiffnessLabel_, stiffness_);
        row(widthLabel_, width_);
        row(muteOnReleaseLabel_, muteOnRelease_);
        row(pickupHzLabel_, pickupHz_);
        row(pickupQLabel_, pickupQ_);
        row(palmDecayLabel_, palmDecay_);
        row(palmBrightLabel_, palmBright_);
    }

private:
    static constexpr int kNumFrets      = 22; // 0 (open) through 22
    static constexpr int kTuningHeight  = 26;
    static constexpr int kToneHeight    = 10 * 22; // the tone-template button row + 9 GuitarSettings sliders
    // A row of open-shape buttons, the strum controls, and the chord-mode row.
    static constexpr int kChordHeight   = 26 + 22 + 22;
    static constexpr int kLowestTuning  = 28; // E1, low enough for any drop tuning
    static constexpr int kHighestTuning = 67;

    /** Lower strings are drawn thicker, as on the instrument. */
    static int stringIndexToThickness(int stringIndex) { return model::kNumGuitarStrings - 1 - stringIndex; }

    juce::Rectangle<int> boardArea() const
    {
        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight); // matches the untransformed strip paint() draws into
        area = area.reduced(6);
        area.removeFromTop(kTuningHeight);
        area.removeFromBottom(kToneHeight);
        area.removeFromBottom(kChordHeight);
        return area;
    }

    bool fretAt(juce::Point<float> point, int& stringOut, int& fretOut) const
    {
        const auto board = boardArea();
        if (! board.toFloat().contains(point))
            return false;

        const float rowHeight = (float) board.getHeight() / (float) model::kNumGuitarStrings;
        const float fretWidth = (float) board.getWidth() / (float) (kNumFrets + 1);
        if (rowHeight <= 0.0f || fretWidth <= 0.0f)
            return false;

        const int row  = juce::jlimit(0, model::kNumGuitarStrings - 1,
                                      (int) ((point.y - (float) board.getY()) / rowHeight));
        const int fret = juce::jlimit(0, kNumFrets,
                                      (int) ((point.x - (float) board.getX()) / fretWidth));

        stringOut = model::kNumGuitarStrings - 1 - row; // display is high-to-low
        fretOut   = fret;
        return true;
    }

    void stampChord(int shapeIndex)
    {
        if (! onChordStamped || shapeIndex < 0 || shapeIndex >= engine::kNumChordShapes)
            return;

        onChordStamped(engine::kChordShapes[shapeIndex], 0, currentStrum());
    }

    void setupSlider(juce::Slider& slider, double lo, double hi, double step,
                     const juce::String& suffix, std::function<void()> onChange)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = std::move(onChange);
        // Also wired on the strum-spread/humanise sliders, which don't touch
        // settings_ at all (they're read on demand — see their own comment)
        // — a harmless no-op pair each time, since commitStructDrag skips a
        // drag that landed back where it started.
        slider.onDragStart = [this] { if (onSettingsDragStart) onSettingsDragStart(); };
        slider.onDragEnd   = [this] { if (onSettingsDragEnd)   onSettingsDragEnd(); };
        addChildComponent(slider);
    }

    /** For a discrete control (the tuning dropdowns): a click has no
        "during" to bracket, so both ends fire back to back around the one
        edit it makes — same reasoning as SynthEditor::reportInstantEdit. */
    void reportInstantEdit()
    {
        if (onSettingsDragStart) onSettingsDragStart();
        pushSettings();
        if (onSettingsDragEnd) onSettingsDragEnd();
    }

    void setupLabel(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
        label.setInterceptsMouseClicks(false, false);
        addChildComponent(label);
    }

    /** Reads the controls back into the settings and reports them. Guarded
        against setSettings's own setValue calls, which would otherwise echo
        straight back as a user edit. */
    void pushSettings()
    {
        if (updating_ || ! onSettingsChanged)
            return;

        for (int s = 0; s < model::kNumGuitarStrings; ++s)
        {
            const int id = tuningBoxes_[(size_t) s].getSelectedId();
            if (id > 0)
                settings_.tuning[(size_t) s] = id;
        }

        settings_.decaySeconds  = (float) decay_.getValue();
        settings_.brightness    = (float) (brightness_.getValue() / 100.0);
        settings_.pickPosition  = (float) (pickPosition_.getValue() / 100.0);
        settings_.pickHardness  = (float) (pickHardness_.getValue() / 100.0);
        settings_.velocitySensitivity = (float) (velocitySense_.getValue() / 100.0);
        settings_.stringCoupling      = (float) (coupling_.getValue() / 100.0);
        settings_.stiffness           = (float) (stiffness_.getValue() / 100.0);
        settings_.stereoWidth         = (float) (width_.getValue() / 100.0);
        settings_.muteOnNoteOff = (float) (muteOnRelease_.getValue() / 100.0);
        settings_.pickupResonanceHz = (float) pickupHz_.getValue();
        settings_.pickupQ           = (float) pickupQ_.getValue();
        settings_.palmMuteDecaySeconds = (float) palmDecay_.getValue();
        settings_.palmMuteBrightness   = (float) (palmBright_.getValue() / 100.0);

        onSettingsChanged(settings_);
        repaint(); // a tuning change relabels the whole board
    }

    /** Every control this pane shows and hides, in one place — see the same
        list in EffectChainPanel, and the bug that made it worth having. */
    std::vector<juce::Component*> managedControls()
    {
        std::vector<juce::Component*> controls {
            &decayLabel_, &decay_, &brightnessLabel_, &brightness_,
            &pickPositionLabel_, &pickPosition_, &pickHardnessLabel_,
            &pickHardness_, &velocitySenseLabel_, &velocitySense_,
            &couplingLabel_, &coupling_, &stiffnessLabel_, &stiffness_,
            &widthLabel_, &width_,
            &muteOnReleaseLabel_, &muteOnRelease_,
            &pickupHzLabel_, &pickupHz_, &pickupQLabel_, &pickupQ_,
            &palmDecayLabel_, &palmDecay_, &palmBrightLabel_, &palmBright_,
            &strumDirection_, &strumSpread_, &strumHumanise_,
            &strumSpreadLabel_, &strumHumaniseLabel_,
            &chordMode_, &writeToClip_
        };

        for (auto& box : tuningBoxes_)
            controls.push_back(&box);
        for (auto* button : chordButtons_)
            controls.push_back(button);
        for (auto* button : toneButtons_)
            controls.push_back(button);

        return controls;
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholder_.setVisible(! visible);

        for (auto* c : managedControls())
            c->setVisible(visible);

        resized();
        repaint();
    }

    model::GuitarSettings                      settings_;
    std::array<int, model::kNumGuitarStrings>  ringing_ { -1, -1, -1, -1, -1, -1 };
    bool                                       contentVisible_ = false;
    bool                                       updating_       = false;
    int                                        hoverString_    = -1;
    int                                        hoverFret_      = -1;
    juce::String                               trackName_;
    juce::uint32                               trackColour_ = 0;

    juce::Label     placeholder_;
    std::array<juce::ComboBox, model::kNumGuitarStrings> tuningBoxes_;
    juce::Label     decayLabel_, brightnessLabel_, pickPositionLabel_, pickHardnessLabel_, muteOnReleaseLabel_;
    juce::Slider    decay_, brightness_, pickPosition_, pickHardness_, muteOnRelease_;

    // Phase 1 of the guitar work (docs/PLAN.md §33): how much velocity
    // brightens a note, how much energy crosses at the bridge, and how far the
    // strings are spread.
    juce::Label     velocitySenseLabel_, couplingLabel_, widthLabel_, stiffnessLabel_;
    juce::Slider    velocitySense_, coupling_, width_, stiffness_;
    juce::Label     pickupHzLabel_, pickupQLabel_, palmDecayLabel_, palmBrightLabel_;
    juce::Slider    pickupHz_, pickupQ_, palmDecay_, palmBright_;
    juce::OwnedArray<juce::TextButton> toneButtons_;

    juce::OwnedArray<juce::TextButton> chordButtons_;
    juce::ComboBox     chordMode_;
    juce::ToggleButton writeToClip_;
    juce::ComboBox  strumDirection_;
    juce::Label     strumSpreadLabel_, strumHumaniseLabel_;
    juce::Slider    strumSpread_, strumHumanise_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FretboardPane)
};

} // namespace looper
