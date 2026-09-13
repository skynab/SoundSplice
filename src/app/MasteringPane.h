#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioFileTypes.h"

#include "engine/EqCurve.h"
#include "engine/MasteringPreset.h"
#include "model/Effects.h"

namespace looper
{
/**
    The mastering rack's controls: the last stage before the mix leaves the
    app.

    Its own dock pane rather than more rows on the Master panel, which is
    hand-laid 26px rows with no scrolling viewport and would simply run out of
    vertical space. The layout here is two columns so the whole rack fits
    without scrolling at a normal pane size.

    Also a drop target for audio files from the OS. Mastering is the last
    thing you do to a mix, so "I want to master this file" is a completely
    reasonable way to arrive here with nothing in the project yet — and
    having to know that the way in is a differently-named menu item on
    another pane is exactly the kind of thing that reads as "I can't import
    audio". Dropping here brings the file into the project as a new track,
    which is what the rack then processes.

    Owns no document state: it shows a model::MasteringSettings and reports
    changes, the same separation every other pane keeps. The drag-start/end
    pair around the continuous controls is what makes a whole slider drag one
    undo step.
*/
class MasteringPane final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            public juce::DragAndDropTarget
{
public:
    std::function<void(const model::MasteringSettings&)> onSettingsChanged;
    std::function<void()>                                onSettingsDragStart;
    std::function<void()>                                onSettingsDragEnd;
    std::function<void(engine::MasteringPreset)>         onPresetRequested;

    /** Audio files dropped onto the pane. The owner decides what that means
        (a new track per file) — this pane has no document access. */
    std::function<void(const juce::Array<juce::File>&)>  onFilesDropped;

    MasteringPane()
    {
        enabledButton_.setButtonText("Enable mastering");
        enabledButton_.onClick = [this] { reportInstantEdit([this] { settings_.enabled = enabledButton_.getToggleState(); }); };
        content_.addAndMakeVisible(enabledButton_);

        for (int i = 0; i < engine::kNumMasteringPresets; ++i)
        {
            const auto preset = (engine::MasteringPreset) i;
            auto*      button = presetButtons_.add(new juce::TextButton(engine::masteringPresetName(preset)));
            button->onClick   = [this, preset] { if (onPresetRequested) onPresetRequested(preset); };
            content_.addAndMakeVisible(button);
        }

        setupSection(eqHeader_, "EQ");
        setupSlider(lowHzSlider_,   20.0, 500.0,   1.0, " Hz", [this] { settings_.lowShelfHz = (float) lowHzSlider_.getValue(); });
        setupSlider(lowDbSlider_,  -18.0,  18.0,   0.1, " dB", [this] { settings_.lowShelfDb = (float) lowDbSlider_.getValue(); });
        setupSlider(peakHzSlider_, 200.0, 8000.0,  1.0, " Hz", [this] { settings_.peakHz     = (float) peakHzSlider_.getValue(); });
        setupSlider(peakDbSlider_, -18.0,  18.0,   0.1, " dB", [this] { settings_.peakDb     = (float) peakDbSlider_.getValue(); });
        setupSlider(peakQSlider_,    0.2,   4.0,  0.05, "",    [this] { settings_.peakQ      = (float) peakQSlider_.getValue(); });
        setupSlider(highHzSlider_, 2000.0, 16000.0, 1.0, " Hz", [this] { settings_.highShelfHz = (float) highHzSlider_.getValue(); });
        setupSlider(highDbSlider_, -18.0,  18.0,   0.1, " dB", [this] { settings_.highShelfDb = (float) highDbSlider_.getValue(); });

        setupLabel(lowHzLabel_, "Low Hz");
        setupLabel(lowDbLabel_, "Low dB");
        setupLabel(peakHzLabel_, "Peak Hz");
        setupLabel(peakDbLabel_, "Peak dB");
        setupLabel(peakQLabel_, "Peak Q");
        setupLabel(highHzLabel_, "High Hz");
        setupLabel(highDbLabel_, "High dB");

        setupSection(toolsHeader_, "Exciter / Width / Space");
        setupSlider(exciterSlider_,   0.0, 100.0, 1.0, " %",  [this] { settings_.exciterAmount      = (float) (exciterSlider_.getValue() / 100.0); });
        setupSlider(exciterHzSlider_, 500.0, 12000.0, 1.0, " Hz", [this] { settings_.exciterCrossoverHz = (float) exciterHzSlider_.getValue(); });
        setupSlider(widthSlider_,     0.0, 200.0, 1.0, " %",  [this] { settings_.width               = (float) (widthSlider_.getValue() / 100.0); });
        setupSlider(reverbSlider_,    0.0, 100.0, 1.0, " %",  [this] { settings_.reverbAmount        = (float) (reverbSlider_.getValue() / 100.0); });
        setupSlider(reverbRoomSlider_, 0.0, 100.0, 1.0, " %", [this] { settings_.reverbRoomSize      = (float) (reverbRoomSlider_.getValue() / 100.0); });

        setupLabel(exciterLabel_, "Exciter");
        setupLabel(exciterHzLabel_, "Exc Hz");
        setupLabel(widthLabel_, "Width");
        setupLabel(reverbLabel_, "Reverb");
        setupLabel(reverbRoomLabel_, "Room");

        setupSection(loudnessHeader_, "Loudness");
        setupSlider(driveSlider_,   0.0, 24.0, 0.1, " dB", [this] { settings_.maximizerInputDb   = (float) driveSlider_.getValue(); });
        setupSlider(ceilingSlider_, -12.0, 0.0, 0.1, " dB", [this] { settings_.maximizerCeilingDb = (float) ceilingSlider_.getValue(); });
        setupSlider(releaseSlider_, 10.0, 500.0, 1.0, " ms", [this] { settings_.maximizerReleaseMs = (float) releaseSlider_.getValue(); });
        setupSlider(outputSlider_, -24.0, 12.0, 0.1, " dB", [this] { settings_.outputGainDb       = (float) outputSlider_.getValue(); });

        setupLabel(driveLabel_, "Drive");
        setupLabel(ceilingLabel_, "Ceiling");
        setupLabel(releaseLabel_, "Release");
        setupLabel(outputLabel_, "Output");
        setupLabel(reductionLabel_, "GR");

        // The rack has more controls than fit at a normal pane height, and a
        // row laid out with no room left gets zero height — present,
        // hit-testing against nothing. A viewport is the fix, and is exactly
        // what the Master panel lacks (which is why this rack didn't go
        // there); building this pane without one reproduced the same bug.
        content_.addAndMakeVisible(curve_);
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
    }

    void setSettings(const model::MasteringSettings& settings)
    {
        settings_ = settings;
        updating_ = true;

        enabledButton_.setToggleState(settings.enabled, juce::dontSendNotification);

        lowHzSlider_.setValue(settings.lowShelfHz, juce::dontSendNotification);
        lowDbSlider_.setValue(settings.lowShelfDb, juce::dontSendNotification);
        peakHzSlider_.setValue(settings.peakHz, juce::dontSendNotification);
        peakDbSlider_.setValue(settings.peakDb, juce::dontSendNotification);
        peakQSlider_.setValue(settings.peakQ, juce::dontSendNotification);
        highHzSlider_.setValue(settings.highShelfHz, juce::dontSendNotification);
        highDbSlider_.setValue(settings.highShelfDb, juce::dontSendNotification);

        exciterSlider_.setValue(settings.exciterAmount * 100.0, juce::dontSendNotification);
        exciterHzSlider_.setValue(settings.exciterCrossoverHz, juce::dontSendNotification);
        widthSlider_.setValue(settings.width * 100.0, juce::dontSendNotification);
        reverbSlider_.setValue(settings.reverbAmount * 100.0, juce::dontSendNotification);
        reverbRoomSlider_.setValue(settings.reverbRoomSize * 100.0, juce::dontSendNotification);

        driveSlider_.setValue(settings.maximizerInputDb, juce::dontSendNotification);
        ceilingSlider_.setValue(settings.maximizerCeilingDb, juce::dontSendNotification);
        releaseSlider_.setValue(settings.maximizerReleaseMs, juce::dontSendNotification);
        outputSlider_.setValue(settings.outputGainDb, juce::dontSendNotification);

        updating_ = false;
        repaint();
    }

    /** Live gain reduction from the limiter, for the meter. Driven by the
        owner's timer — the pane has no access to the engine. */
    void setReductionDb(float db)
    {
        if (std::abs(db - reductionDb_) < 0.05f)
            return;
        reductionDb_ = db;
        repaint();
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1a1a1e)); }

    /** Over the children, not behind them: the controls live inside a
        viewport that covers the whole pane, so a highlight drawn in paint()
        would be completely hidden by it. */
    void paintOverChildren(juce::Graphics& g) override
    {
        if (! fileDragActive_)
            return;

        // Only shown mid-drag: a permanent "drop files here" banner would be
        // clutter on a pane whose actual job is the controls.
        g.setColour(juce::Colours::cyan.withAlpha(0.12f));
        g.fillAll();
        g.setColour(juce::Colours::cyan.withAlpha(0.9f));
        g.drawRect(getLocalBounds(), 2);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText("Drop audio to add it to the project",
                   getLocalBounds(), juce::Justification::centred);
    }

    // juce::DragAndDropTarget — drags from the app's own Files pane, which
    // are a different JUCE mechanism from drags out of the OS below.
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return audiofiles::fileFromDragDescription(details.description) != juce::File{};
    }

    void itemDragEnter(const SourceDetails&) override { fileDragActive_ = true;  repaint(); }
    void itemDragExit(const SourceDetails&) override  { fileDragActive_ = false; repaint(); }

    void itemDropped(const SourceDetails& details) override
    {
        fileDragActive_ = false;
        repaint();

        const auto file = audiofiles::fileFromDragDescription(details.description);
        if (file != juce::File{} && onFilesDropped)
            onFilesDropped({ file });
    }

    // juce::FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return audiofiles::containsImportableAudio(files);
    }

    void fileDragEnter(const juce::StringArray&, int, int) override
    {
        fileDragActive_ = true;
        repaint();
    }

    void fileDragExit(const juce::StringArray&) override
    {
        fileDragActive_ = false;
        repaint();
    }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        fileDragActive_ = false;
        repaint();

        const auto importable = audiofiles::importableFilesIn(files);
        if (! importable.isEmpty() && onFilesDropped)
            onFilesDropped(importable);
    }

    /** The EQ response and gain-reduction readout, drawn as a child of the
        scrolling content so it scrolls with the controls it describes. */
    void paintCurve(juce::Graphics& g)
    {
        const auto curve = curve_.getLocalBounds().toFloat().withTrimmedBottom(kMeterHeight + 4);
        if (curve.isEmpty())
            return;

        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(curve, 3.0f);

        const float zeroY = yForDb(0.0f, curve);
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.drawLine(curve.getX(), zeroY, curve.getRight(), zeroY);

        // Built from the same ShelfPeakFilter the rack runs, so the drawing
        // can't claim something the audio doesn't do — see
        // engine::masteringEqMagnitudeDb.
        juce::Path path;
        const int  steps = juce::jmax(2, (int) curve.getWidth() / 3);
        for (int i = 0; i <= steps; ++i)
        {
            const float proportion = (float) i / (float) steps;
            const float hz         = hzForProportion(proportion);
            const float db         = engine::masteringEqMagnitudeDb(settings_, hz);
            const float x          = curve.getX() + proportion * curve.getWidth();
            const float y          = yForDb(db, curve);

            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }

        g.setColour(settings_.enabled ? juce::Colours::cyan.withAlpha(0.9f)
                                      : juce::Colours::white.withAlpha(0.25f));
        g.strokePath(path, juce::PathStrokeType(1.5f));

        // Gain-reduction meter, in the strip under the curve — both are
        // children of the same view, so the meter is derived from the same
        // bounds rather than tracked separately.
        const auto meter = curve_.getLocalBounds().toFloat()
                               .removeFromBottom((float) kMeterHeight);
        if (! meter.isEmpty())
        {
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillRoundedRectangle(meter, 2.0f);

            const float amount = juce::jlimit(0.0f, 1.0f, -reductionDb_ / kMeterRangeDb);
            if (amount > 0.0f)
            {
                g.setColour(juce::Colours::orange.withAlpha(0.85f));
                g.fillRoundedRectangle(meter.withWidth(meter.getWidth() * amount), 2.0f);
            }

            g.setColour(juce::Colours::white.withAlpha(0.75f));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(juce::String(reductionDb_, 1) + " dB", meter, juce::Justification::centred);
        }
    }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds());

        // The content is sized to what the rack *needs*, never to what the
        // pane happens to be — that's what stops a short pane producing
        // zero-height rows, and what the viewport then scrolls.
        const int width = juce::jmax(kMinContentWidth,
                                     viewport_.getMaximumVisibleWidth());
        content_.setSize(width, requiredHeight());
        layoutContent();
    }

private:
    /** Rows in the taller of the two columns, plus the fixed blocks above
        them. Kept as a computed height rather than a constant so adding a
        control can't silently overflow the content the way it overflowed the
        pane. */
    int requiredHeight() const
    {
        const int fixedTop   = kRowHeight * 2 + 4 + 6 + kCurveHeight + 4 + kMeterHeight + 6;
        const int leftRows   = 1 + 7; // header + seven EQ rows
        const int rightRows  = 1 + 5 + 1 + 4; // two headers and their rows
        return fixedTop + kRowHeight * juce::jmax(leftRows, rightRows) + 8;
    }

    void layoutContent()
    {
        auto area = content_.getLocalBounds().reduced(8);

        enabledButton_.setBounds(area.removeFromTop(kRowHeight));
        area.removeFromTop(4);

        auto presetRow = area.removeFromTop(kRowHeight);
        for (int i = 0; i < presetButtons_.size(); ++i)
        {
            const int remaining = presetButtons_.size() - i;
            const int buttonWidth = juce::jmax(1, presetRow.getWidth() / remaining);
            presetButtons_[i]->setBounds(presetRow.removeFromLeft(buttonWidth).reduced(1));
        }
        area.removeFromTop(6);

        curve_.setBounds(area.removeFromTop(kCurveHeight + 4 + kMeterHeight));
        area.removeFromTop(6);

        // Two columns, so the rack reads as three grouped sections rather
        // than one very long list.
        auto left  = area.removeFromLeft(juce::jmax(1, area.getWidth() / 2));
        auto right = area;

        layoutHeader(eqHeader_, left);
        layoutRow(left, lowHzLabel_, lowHzSlider_);
        layoutRow(left, lowDbLabel_, lowDbSlider_);
        layoutRow(left, peakHzLabel_, peakHzSlider_);
        layoutRow(left, peakDbLabel_, peakDbSlider_);
        layoutRow(left, peakQLabel_, peakQSlider_);
        layoutRow(left, highHzLabel_, highHzSlider_);
        layoutRow(left, highDbLabel_, highDbSlider_);

        layoutHeader(toolsHeader_, right);
        layoutRow(right, exciterLabel_, exciterSlider_);
        layoutRow(right, exciterHzLabel_, exciterHzSlider_);
        layoutRow(right, widthLabel_, widthSlider_);
        layoutRow(right, reverbLabel_, reverbSlider_);
        layoutRow(right, reverbRoomLabel_, reverbRoomSlider_);

        layoutHeader(loudnessHeader_, right);
        layoutRow(right, driveLabel_, driveSlider_);
        layoutRow(right, ceilingLabel_, ceilingSlider_);
        layoutRow(right, releaseLabel_, releaseSlider_);
        layoutRow(right, outputLabel_, outputSlider_);
    }

private:
    static constexpr int   kMinContentWidth = 360;
    static constexpr int   kRowHeight    = 22;
    static constexpr int   kCurveHeight  = 70;
    static constexpr int   kMeterHeight  = 16;
    static constexpr int   kLabelWidth   = 58;
    static constexpr float kMinDb        = -18.0f;
    static constexpr float kMaxDb        = 18.0f;
    static constexpr float kMeterRangeDb = 12.0f;

    static float hzForProportion(float proportion)
    {
        // Log-spaced across the audible band, the way a frequency axis is
        // always read.
        return 20.0f * std::pow(1000.0f, proportion);
    }

    static float yForDb(float db, juce::Rectangle<float> area)
    {
        const float clamped = juce::jlimit(kMinDb, kMaxDb, db);
        const float t       = (clamped - kMinDb) / (kMaxDb - kMinDb);
        return area.getBottom() - t * area.getHeight();
    }

    void layoutHeader(juce::Label& label, juce::Rectangle<int>& area)
    {
        label.setBounds(area.removeFromTop(kRowHeight).reduced(2, 0));
    }

    void layoutRow(juce::Rectangle<int>& area, juce::Label& label, juce::Slider& slider)
    {
        auto row = area.removeFromTop(kRowHeight);
        label.setBounds(row.removeFromLeft(juce::jmin(kLabelWidth, row.getWidth())));
        slider.setBounds(row.reduced(2, 1));
    }

    void setupSection(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.85f));
        label.setInterceptsMouseClicks(false, false);
        content_.addAndMakeVisible(label);
    }

    void setupLabel(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
        label.setInterceptsMouseClicks(false, false);
        content_.addAndMakeVisible(label);
    }

    void setupSlider(juce::Slider& slider, double lo, double hi, double step,
                     const juce::String& suffix, std::function<void()> apply)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = [this, apply = std::move(apply)]
        {
            if (updating_)
                return;
            apply();
            notify();
            repaint(); // the curve follows the EQ sliders live
        };
        slider.onDragStart = [this] { if (onSettingsDragStart) onSettingsDragStart(); };
        slider.onDragEnd   = [this] { if (onSettingsDragEnd)   onSettingsDragEnd(); };
        content_.addAndMakeVisible(slider);
    }

    /** For a discrete control: a click has no "during" to bracket, so both
        ends fire back to back around the one edit it makes — same reasoning
        as SynthEditor::reportInstantEdit. */
    void reportInstantEdit(std::function<void()> apply)
    {
        if (updating_)
            return;
        if (onSettingsDragStart) onSettingsDragStart();
        apply();
        notify();
        if (onSettingsDragEnd) onSettingsDragEnd();
        repaint();
    }

    void notify()
    {
        if (onSettingsChanged)
            onSettingsChanged(settings_);
    }

    /** Forwards its paint to the pane, so the response curve and GR meter
        live inside the scrolling content while the drawing code stays with
        the settings it reads. */
    struct CurveView final : juce::Component
    {
        explicit CurveView(MasteringPane& owner) : owner_(owner) { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics& g) override { owner_.paintCurve(g); }
        MasteringPane& owner_;
    };

    model::MasteringSettings settings_;
    bool                     updating_    = false;
    float                    reductionDb_ = 0.0f;
    bool                     fileDragActive_ = false;

    juce::Viewport viewport_;
    juce::Component content_;
    CurveView       curve_ { *this };

    juce::ToggleButton                 enabledButton_;
    juce::OwnedArray<juce::TextButton> presetButtons_;

    juce::Label  eqHeader_, toolsHeader_, loudnessHeader_;
    juce::Label  lowHzLabel_, lowDbLabel_, peakHzLabel_, peakDbLabel_, peakQLabel_;
    juce::Label  highHzLabel_, highDbLabel_;
    juce::Label  exciterLabel_, exciterHzLabel_, widthLabel_, reverbLabel_, reverbRoomLabel_;
    juce::Label  driveLabel_, ceilingLabel_, releaseLabel_, outputLabel_, reductionLabel_;

    juce::Slider lowHzSlider_, lowDbSlider_, peakHzSlider_, peakDbSlider_, peakQSlider_;
    juce::Slider highHzSlider_, highDbSlider_;
    juce::Slider exciterSlider_, exciterHzSlider_, widthSlider_, reverbSlider_, reverbRoomSlider_;
    juce::Slider driveSlider_, ceilingSlider_, releaseSlider_, outputSlider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasteringPane)
};

} // namespace looper
