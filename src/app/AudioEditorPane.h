#pragma once

#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Track.h"

#include "AudioFileTypes.h"
#include "AudioSelection.h"
#include "TrackColours.h"
#include "WaveformPeaks.h"

namespace looper
{
/**
    The waveform editor: one audio clip drawn large, with a click-drag range
    selection over it.

    The arrangement's 40px lanes are for arranging — you can see that a clip
    has audio in it, but you can't pick out the gap between two words. Every
    mastering operation starts by pointing at a piece of the recording ("this
    bit is just room noise"), and that gesture needs a view where a tenth of a
    second is more than a pixel wide. Hence a pane of its own rather than
    another gesture layered onto the timeline.

    Also a drop target for audio files from the OS, like the timeline and the
    mastering pane. Dropping a file here means "I want to edit this one", so
    the owner imports it and selects it — the file appears in this editor
    rather than only landing somewhere on the timeline.

    Owns no document state: it draws from a file path plus a length, and
    reports the selection through the callbacks. The actions themselves live
    in MainComponent, the same separation every other pane here keeps.
*/
class AudioEditorPane final : public juce::Component,
                              public juce::FileDragAndDropTarget,
                              public juce::DragAndDropTarget
{
public:
    /** The selected range, in seconds into the file. An empty range means the
        user has cleared the selection — actions then apply to the whole clip,
        so the two are deliberately distinguishable (see AudioRange). */
    std::function<void(AudioRange)> onSelectionChanged;

    /** Apply @p gainDb to the clip. Fired live while the slider moves; the
        drag-start/end pair around it is what makes the whole drag one undo
        step, exactly as SynthEditor and FretboardPane do. */
    std::function<void(float gainDb)> onGainChanged;
    std::function<void()>             onGainDragStart;
    std::function<void()>             onGainDragEnd;

    /** Set the clip's gain so its loudest point reaches the target peak.
        MainComponent measures the file — this pane has no sample data. */
    std::function<void()> onNormaliseRequested;

    /** Move the song's playhead to a point in this clip's file. The editor
        shows the *song* timeline — there is only one — so clicking here is
        the same gesture as clicking the ruler in the Tracks pane. */
    std::function<void(double secondsIntoFile)> onSeekRequested;

    /** Start or stop the song transport. @p fromSeconds is where to start
        from; a selection plays from its beginning. */
    std::function<void(double fromSeconds)> onPlayRequested;
    std::function<void()>                   onStopRequested;

    /** The destructive edit actions. Named rather than one callback with an
        enum so the owner's wiring reads as a list of commands, matching how
        every other pane here reports intent. */
    std::function<void()> onCutRequested;
    std::function<void()> onCopyRequested;
    std::function<void()> onPasteRequested;
    std::function<void()> onDeleteRequested;
    std::function<void()> onTrimRequested;
    std::function<void()> onSplitRequested;
    std::function<void()> onSilenceRequested;
    std::function<void()> onFadeInRequested;
    std::function<void()> onFadeOutRequested;
    std::function<void()> onReverseRequested;
    std::function<void()> onApplyEffectsRequested;
    std::function<void()> onSpeedPitchRequested;

    /** Audio files dropped onto the pane. The owner decides what that means
        (import and select, so it opens here) — this pane has no document
        access. */
    std::function<void(const juce::Array<juce::File>&)> onFilesDropped;

    /** Measure the noise in the current selection, to subtract later. Only
        offered when something is selected: a print taken from the whole clip
        would describe the material as much as the noise, and denoising with
        it would gut the recording. */
    std::function<void()> onCaptureNoisePrintRequested;

    /** Subtract the captured print from the whole clip. Amount is how much
        more than the measured noise to remove; floor bounds how far any one
        frequency may be attenuated (the musical-noise guard). */
    std::function<void(float amountDb, float floorDb)> onReduceNoiseRequested;

    AudioEditorPane()
    {
        placeholderLabel_.setText("Select an Audio clip to edit it", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        selectionLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        selectionLabel_.setInterceptsMouseClicks(false, false);

        gainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        gainSlider_.setRange(-24.0, 24.0, 0.1);
        gainSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 18);
        gainSlider_.setTextValueSuffix(" dB");
        gainSlider_.setTooltip("Level for this clip alone, on top of the track fader");
        gainSlider_.onValueChange = [this]
        {
            gainDb_ = (float) gainSlider_.getValue();
            repaint(); // the waveform follows the gain live
            if (! updating_ && onGainChanged)
                onGainChanged(gainDb_);
        };
        gainSlider_.onDragStart = [this] { if (onGainDragStart) onGainDragStart(); };
        gainSlider_.onDragEnd   = [this] { if (onGainDragEnd)   onGainDragEnd(); };

        gainLabel_.setText("Gain", juce::dontSendNotification);
        gainLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        gainLabel_.setInterceptsMouseClicks(false, false);

        normaliseButton_.setButtonText("Normalize");
        normaliseButton_.setTooltip("Set this clip's gain so its loudest point just reaches full scale");
        normaliseButton_.onClick = [this] { if (onNormaliseRequested) onNormaliseRequested(); };

        playButton_.setButtonText("Play");
        playButton_.setTooltip("Audition the selection, or the whole clip when nothing is selected");
        playButton_.onClick = [this]
        {
            if (playing_)
            {
                if (onStopRequested) onStopRequested();
                return;
            }
            // From the selection's start when there is one, otherwise from
            // wherever the cursor was last put.
            if (onPlayRequested)
                onPlayRequested(selection_.isEmpty() ? playheadSeconds_ : selection_.startSeconds);
        };

        // Two rows of edit commands. Each fires its own callback; the owner
        // decides what they mean and refuses when there's no selection.
        struct EditButtonSpec { juce::TextButton* button; const char* text; std::function<void()>* callback; };
        const EditButtonSpec editSpecs[] {
            { &cutButton_,     "Cut",      &onCutRequested },
            { &copyButton_,    "Copy",     &onCopyRequested },
            { &pasteButton_,   "Paste",    &onPasteRequested },
            { &deleteButton_,  "Delete",   &onDeleteRequested },
            { &trimButton_,    "Trim",     &onTrimRequested },
            { &splitButton_,   "Split",    &onSplitRequested },
            { &silenceButton_, "Silence",  &onSilenceRequested },
            { &fadeInButton_,  "Fade In",  &onFadeInRequested },
            { &fadeOutButton_, "Fade Out", &onFadeOutRequested },
            { &reverseButton_, "Reverse",  &onReverseRequested },
            { &effectsButton_, "Effects...", &onApplyEffectsRequested },
            { &speedPitchButton_, "Speed/Pitch...", &onSpeedPitchRequested },
        };

        for (const auto& spec : editSpecs)
        {
            spec.button->setButtonText(spec.text);
            auto* callback = spec.callback;
            spec.button->onClick = [callback] { if (*callback) (*callback)(); };
        }

        captureNoiseButton_.setButtonText("Capture Noise Print");
        captureNoiseButton_.setTooltip("Select a passage with only background noise, then capture it");
        captureNoiseButton_.onClick = [this] { if (onCaptureNoisePrintRequested) onCaptureNoisePrintRequested(); };

        reduceNoiseButton_.setButtonText("Reduce Noise");
        reduceNoiseButton_.setTooltip("Subtract the captured noise print from the whole clip");
        reduceNoiseButton_.onClick = [this]
        {
            if (onReduceNoiseRequested)
                onReduceNoiseRequested((float) noiseAmountSlider_.getValue(),
                                       (float) noiseFloorSlider_.getValue());
        };

        noiseAmountSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        noiseAmountSlider_.setRange(0.0, 24.0, 0.5);
        noiseAmountSlider_.setValue(12.0, juce::dontSendNotification);
        noiseAmountSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        noiseAmountSlider_.setTextValueSuffix(" dB");
        noiseAmountSlider_.setTooltip("How much more than the measured noise to subtract");
        noiseAmountSlider_.onValueChange = [] {}; // read when Reduce Noise is pressed

        noiseFloorSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        noiseFloorSlider_.setRange(-48.0, -3.0, 1.0);
        noiseFloorSlider_.setValue(-24.0, juce::dontSendNotification);
        noiseFloorSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        noiseFloorSlider_.setTextValueSuffix(" dB");
        noiseFloorSlider_.setTooltip("How far any one frequency may be attenuated - "
                                     "deeper removes more noise but risks warbling artefacts");
        noiseFloorSlider_.onValueChange = [] {}; // read when Reduce Noise is pressed

        noiseAmountLabel_.setText("Amount", juce::dontSendNotification);
        noiseAmountLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        noiseAmountLabel_.setInterceptsMouseClicks(false, false);
        noiseFloorLabel_.setText("Floor", juce::dontSendNotification);
        noiseFloorLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        noiseFloorLabel_.setInterceptsMouseClicks(false, false);

        zoomInButton_.setButtonText("+");
        zoomInButton_.setTooltip("Zoom in");
        zoomInButton_.onClick = [this] { zoomBy(0.5); };

        zoomOutButton_.setButtonText("-");
        zoomOutButton_.setTooltip("Zoom out");
        zoomOutButton_.onClick = [this] { zoomBy(2.0); };

        zoomFitButton_.setButtonText("Fit");
        zoomFitButton_.setTooltip("Show the whole clip");
        zoomFitButton_.onClick = [this] { zoomToFit(); };

        selectAllButton_.setButtonText("Select All");
        selectAllButton_.onClick = [this]
        {
            setSelection({ 0.0, geometry_.fileLengthSeconds });
            notifySelection();
        };

        clearSelectionButton_.setButtonText("Clear");
        clearSelectionButton_.setTooltip("Clear the selection, so actions apply to the whole clip");
        clearSelectionButton_.onClick = [this]
        {
            setSelection({});
            notifySelection();
        };

        // Same guarantee as FretboardPane and SynthEditor: a control that was
        // never parented lays out and hides perfectly while drawing nothing.
        for (auto* control : managedControls())
            addChildComponent(control);

        setContentVisible(false);
    }

    /** Shows @p file for editing. @p gainDb is the clip's current trim. */
    void setClip(const juce::File& file, double fileLengthSeconds, float gainDb,
                 const juce::String& trackName, juce::uint32 trackColour)
    {
        const bool differentFile = file != file_;

        file_        = file;
        trackName_   = trackName;
        trackColour_ = trackColour;

        geometry_.fileLengthSeconds = juce::jmax(0.0, fileLengthSeconds);

        // Room past the last sample to put the cursor in, so audio can be
        // pasted after the end of the recording rather than only inside it.
        geometry_.viewLengthSeconds = geometry_.fileLengthSeconds + kTailRoomSeconds;

        updating_ = true;
        gainSlider_.setValue(gainDb, juce::dontSendNotification);
        updating_ = false;
        gainDb_   = gainDb;

        // A selection is a position in *this* file; carrying it across to a
        // different one would point at unrelated audio while looking
        // deliberate. Zoom is reset with it for the same reason.
        if (differentFile)
        {
            // A noise print describes one particular recording's noise floor.
            // Carrying it to another file would subtract the wrong spectrum
            // while looking deliberate — which for a destructive action is
            // the worst kind of wrong.
            selection_          = {};
            noisePrintCaptured_ = false;
            playing_            = false;
            playheadSeconds_    = 0.0;
            playButton_.setButtonText("Play");
            zoomToFit();
            notifySelection();
        }

        setContentVisible(true);
        repaint();
    }

    void setNoAudioClipSelected()
    {
        file_      = juce::File{};
        selection_ = {};
        setContentVisible(false);
    }

    /** The current selection, or an empty range. Read by the owner when an
        action fires rather than tracked separately. */
    AudioRange selection() const { return selection_; }

    /** Where the cursor sits, in seconds into the file. May be past the end
        of the file — that's how audio gets pasted after the recording. */
    double cursorSeconds() const { return playheadSeconds_; }

    /** The decoded peaks for the clip on show. Built by the owner (which is
        what reads files) and pushed in only when the file actually changes —
        rebuilding on every refresh would re-read the file dozens of times
        per edit. */
    void setWaveform(WaveformPeaks peaks, double sampleRate)
    {
        peaks_            = std::move(peaks);
        peaksSampleRate_  = sampleRate > 0.0 ? sampleRate : 0.0;
        repaint();
    }

    /** Where the *song's* playhead sits within this clip's file, and whether
        the transport is rolling. Pushed by the owner's timer.

        There is one timeline: this is the same playhead the Tracks pane
        draws, expressed in seconds into the file rather than in song beats.
        A position outside the clip simply falls off either edge of the view. */
    void setPlaybackState(bool playing, double positionSeconds)
    {
        const bool stateChanged = playing != playing_;
        if (! stateChanged && std::abs(positionSeconds - playheadSeconds_) < 1.0e-4)
            return;

        playing_         = playing;
        playheadSeconds_ = positionSeconds;

        if (stateChanged)
            playButton_.setButtonText(playing_ ? "Stop" : "Play");

        repaint();
    }

    /** Whether a noise print has been captured for the clip on show. The
        owner owns the print itself; this only drives what's clickable. */
    void setNoisePrintCaptured(bool captured)
    {
        noisePrintCaptured_ = captured;
        updateNoiseControls();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));
        if (! contentVisible_)
            return;

        paintTrackHeader(g, getLocalBounds().removeFromTop(kTrackHeaderHeight),
                         trackName_, trackColour_, model::TrackType::Audio);

        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        g.setColour(juce::Colour(0xff121216));
        g.fillRect(area);

        // The selection is painted under the waveform, so the waveform stays
        // fully legible inside it — a selection drawn on top dims exactly the
        // part the user is trying to look at.
        if (! selection_.isEmpty())
        {
            const float x1 = juce::jlimit((float) area.getX(), (float) area.getRight(),
                                          geometry_.xForSeconds(selection_.startSeconds));
            const float x2 = juce::jlimit((float) area.getX(), (float) area.getRight(),
                                          geometry_.xForSeconds(selection_.endSeconds));
            g.setColour(juce::Colours::cyan.withAlpha(0.22f));
            g.fillRect(juce::Rectangle<float>(x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
        }

        paintWaveform(g, area);

        // Always drawn, not just while rolling: this is the edit cursor as
        // well as the playhead, and a cursor you can place but not see would
        // be no use for deciding where to click next.
        {
            const float x = geometry_.xForSeconds(playheadSeconds_);
            if (x >= (float) area.getX() && x <= (float) area.getRight())
            {
                g.setColour(juce::Colours::yellow.withAlpha(playing_ ? 0.9f : 0.55f));
                g.drawVerticalLine((int) x, (float) area.getY(), (float) area.getBottom());
            }
        }

        if (! selection_.isEmpty())
        {
            g.setColour(juce::Colours::cyan.withAlpha(0.8f));
            for (double edge : { selection_.startSeconds, selection_.endSeconds })
            {
                const float x = geometry_.xForSeconds(edge);
                if (x >= (float) area.getX() && x <= (float) area.getRight())
                    g.drawVerticalLine((int) x, (float) area.getY(), (float) area.getBottom());
            }
        }
    }

    /** Draws the waveform as it will actually sound: scaled by the clip's
        gain, with anything that would clip marked, and the un-gained
        original ghosted behind so the edit is legible as a change. */
    void paintWaveform(juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (peaks_.isEmpty() || peaksSampleRate_ <= 0.0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawText("Reading waveform...", area, juce::Justification::centred);
            return;
        }

        const float gain     = juce::Decibels::decibelsToGain(gainDb_);
        const int   channels = juce::jmax(1, peaks_.numChannels());
        const int   laneH    = area.getHeight() / channels;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto lane = area.withY(area.getY() + ch * laneH).withHeight(laneH);
            paintChannel(g, lane, ch, gain);
        }
    }

    /** Over the children so the highlight isn't hidden behind the button
        rows, which cover a good part of the pane. */
    void paintOverChildren(juce::Graphics& g) override
    {
        if (! fileDragActive_)
            return;

        g.setColour(juce::Colours::cyan.withAlpha(0.12f));
        g.fillAll();
        g.setColour(juce::Colours::cyan.withAlpha(0.9f));
        g.drawRect(getLocalBounds(), 2);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText("Drop audio to open it here", getLocalBounds(), juce::Justification::centred);
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

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! contentVisible_ || ! waveformArea().contains(e.getPosition()))
            return;

        dragAnchorSeconds_ = geometry_.secondsForX((float) e.position.x);
        dragging_          = true;
        draggedFar_        = false;

        // A plain click clears rather than selecting a zero-length range:
        // "click somewhere to deselect" is the expected gesture, and an empty
        // range is exactly how the rest of the pane spells "no selection".
        setSelection({});
        notifySelection();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging_)
            return;

        // A few pixels of travel separates "clicked to place the cursor" from
        // "dragged to select"; without a threshold, the hand-wobble in any
        // real click would leave a one-pixel selection behind.
        if (std::abs(e.getDistanceFromDragStartX()) > kDragThresholdPixels)
            draggedFar_ = true;

        if (draggedFar_)
            setSelection(AudioRange::fromDrag(dragAnchorSeconds_,
                                              geometry_.secondsForX((float) e.position.x))
                             .clampedTo(geometry_.fileLengthSeconds));
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (! dragging_)
            return;

        dragging_ = false;
        notifySelection();

        // A plain click positions the song's playhead, the same as clicking
        // the ruler in the Tracks pane. It also leaves the selection cleared,
        // which is what a click already did.
        if (! draggedFar_)
        {
            playheadSeconds_ = dragAnchorSeconds_;
            repaint();
            if (onSeekRequested)
                onSeekRequested(dragAnchorSeconds_);
        }
    }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);
        area = area.reduced(6);

        auto toolRow = area.removeFromTop(kToolRowHeight);

        // The readout only gets a share once there's enough width for the
        // buttons to still be usable; below that it's dropped to zero and the
        // buttons take the row. A Label at zero width is invisible but
        // harmless, where a *button* at zero width is present, hit-tests
        // against nothing, and is indistinguishable from one that doesn't
        // work — the failure FretboardPane's proportional row exists to
        // prevent, and which fixed widths here reproduced exactly.
        const int labelWidth = toolRow.getWidth() > kLabelNeedsWidth ? kSelectionLabelWidth : 0;
        selectionLabel_.setBounds(toolRow.removeFromRight(labelWidth).reduced(4, 0));

        // Divide what's actually left rather than imposing minimums, so the
        // buttons shrink together instead of the last ones falling off.
        juce::Component* toolButtons[] { &playButton_,
                                         &zoomOutButton_, &zoomInButton_, &zoomFitButton_,
                                         &selectAllButton_, &clearSelectionButton_ };
        const int buttonCount = (int) std::size(toolButtons);
        for (int i = 0; i < buttonCount; ++i)
        {
            const int remaining = buttonCount - i;
            const int width     = juce::jmax(1, toolRow.getWidth() / remaining);
            toolButtons[i]->setBounds(toolRow.removeFromLeft(width).reduced(1));
        }

        // Edit commands in two rows above the denoise row, so the ten of
        // them stay readable rather than being squeezed into one strip.
        //
        // Hidden outright when the pane is too short for them *and* a usable
        // waveform. A control laid out at zero height is present, clickable
        // against nothing and indistinguishable from a broken one; hiding it
        // is the honest outcome, and it comes straight back on resize.
        const bool roomForEditRows = area.getHeight() >= kMinWaveformHeight + 2 * kToolRowHeight;
        for (auto* button : editButtons())
            button->setVisible(contentVisible_ && roomForEditRows);

        if (roomForEditRows)
        {
            auto layoutButtonRow = [](juce::Rectangle<int> row, std::vector<juce::Component*> buttons)
            {
                for (size_t i = 0; i < buttons.size(); ++i)
                {
                    const int remaining = (int) (buttons.size() - i);
                    const int width     = juce::jmax(1, row.getWidth() / remaining);
                    buttons[i]->setBounds(row.removeFromLeft(width).reduced(1));
                }
            };

            layoutButtonRow(area.removeFromBottom(kToolRowHeight),
                            { &silenceButton_, &fadeInButton_, &fadeOutButton_, &reverseButton_,
                          &splitButton_, &effectsButton_, &speedPitchButton_ });
            layoutButtonRow(area.removeFromBottom(kToolRowHeight),
                            { &cutButton_, &copyButton_, &pasteButton_, &deleteButton_, &trimButton_ });
        }

        // The denoise and gain rows get the same treatment, and are dropped
        // first: they're refinements, where the edit commands are the pane's
        // reason to exist.
        const bool roomForToolRows = area.getHeight() >= kMinWaveformHeight + 2 * kToolRowHeight;
        for (auto* control : toolRowControls())
            control->setVisible(contentVisible_ && roomForToolRows);

        if (! roomForToolRows)
            return;

        // Denoise sits above the gain row, in workflow order: capture a
        // print, then reduce, then set the level.
        auto noiseRow = area.removeFromBottom(kToolRowHeight);
        {
            juce::Component* noiseControls[] { &captureNoiseButton_, &reduceNoiseButton_ };
            const int count = (int) std::size(noiseControls);
            const int forButtons = juce::jmax(2, noiseRow.getWidth() / 2);
            auto buttons = noiseRow.removeFromLeft(forButtons);
            for (int i = 0; i < count; ++i)
            {
                const int remaining = count - i;
                const int width     = juce::jmax(1, buttons.getWidth() / remaining);
                noiseControls[i]->setBounds(buttons.removeFromLeft(width).reduced(1));
            }

            // A label capped at a *third* of its section, never a fixed
            // width: at a narrow pane a fixed-width label eats the whole
            // section and leaves the slider at zero — present, draggable
            // against nothing. Same failure the tool row above already had.
            auto layoutLabelled = [](juce::Rectangle<int> section, int maxLabel,
                                     juce::Label& label, juce::Slider& slider)
            {
                label.setBounds(section.removeFromLeft(juce::jmin(maxLabel, section.getWidth() / 3)));
                slider.setBounds(section.reduced(2, 1));
            };

            layoutLabelled(noiseRow.removeFromLeft(juce::jmax(2, noiseRow.getWidth() / 2)),
                           48, noiseAmountLabel_, noiseAmountSlider_);
            layoutLabelled(noiseRow, 36, noiseFloorLabel_, noiseFloorSlider_);
        }

        auto gainRow = area.removeFromBottom(kToolRowHeight);
        normaliseButton_.setBounds(gainRow.removeFromRight(juce::jmax(1, gainRow.getWidth() / 3))
                                       .reduced(1));
        gainLabel_.setBounds(gainRow.removeFromLeft(juce::jmin(36, gainRow.getWidth())));
        gainSlider_.setBounds(gainRow.reduced(2, 1));

        // Whatever is left is the waveform, and the geometry is told where it
        // starts so a pixel means the same thing in paint() and in a click.
        geometry_.contentLeft = (float) area.getX();
        geometry_.visibleStartSeconds =
            geometry_.clampedStart(geometry_.visibleStartSeconds, (float) area.getWidth());
    }

private:
    static constexpr int kToolRowHeight = 24;

    /** Travel that turns a click into a drag-select. */
    static constexpr int kDragThresholdPixels = 3;

    /** How far past the end of the file the view extends. Enough to be
        obviously there and to click into, without making a short clip look
        lost in empty space. */
    static constexpr double kTailRoomSeconds = 2.0;

    /** Below this the waveform stops being something you can select in, so
        control rows are dropped rather than eating into it further. */
    static constexpr int kMinWaveformHeight = 60;

    std::vector<juce::Component*> editButtons()
    {
        return { &cutButton_, &copyButton_, &pasteButton_, &deleteButton_, &trimButton_,
                 &splitButton_, &silenceButton_, &fadeInButton_, &fadeOutButton_, &reverseButton_,
                 &effectsButton_, &speedPitchButton_ };
    }

    std::vector<juce::Component*> toolRowControls()
    {
        return { &captureNoiseButton_, &reduceNoiseButton_,
                 &noiseAmountLabel_, &noiseAmountSlider_,
                 &noiseFloorLabel_, &noiseFloorSlider_,
                 &gainLabel_, &gainSlider_, &normaliseButton_ };
    }

    /** The selection readout's width, and the row width below which it is
        dropped entirely so the buttons keep usable sizes. */
    static constexpr int kSelectionLabelWidth = 240;
    static constexpr int kLabelNeedsWidth     = 420;

    juce::Rectangle<int> waveformArea() const
    {
        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);
        area = area.reduced(6);
        area.removeFromTop(kToolRowHeight);
        area.removeFromBottom(kToolRowHeight);
        return area;
    }

    /** One channel's lane: gridlines, the original ghosted, the gained
        waveform, and clipping marked. */
    void paintChannel(juce::Graphics& g, juce::Rectangle<int> lane, int channel, float gain)
    {
        const float centreY = (float) lane.getCentreY();
        const float halfH   = (float) lane.getHeight() * 0.5f;

        // dBFS gridlines. Levels are judged in decibels, and a linear
        // waveform with no reference makes -6 and -12 look nearly identical.
        for (float db : { -6.0f, -12.0f, -18.0f })
        {
            const float fraction = juce::Decibels::decibelsToGain(db);
            g.setColour(juce::Colours::white.withAlpha(0.07f));
            for (float sign : { -1.0f, 1.0f })
                g.drawHorizontalLine((int) (centreY + sign * fraction * halfH),
                                     (float) lane.getX(), (float) lane.getRight());
        }

        // Full scale, drawn brighter — the line the gained waveform must not
        // cross.
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.drawHorizontalLine(lane.getY(), (float) lane.getX(), (float) lane.getRight());
        g.drawHorizontalLine(lane.getBottom() - 1, (float) lane.getX(), (float) lane.getRight());

        // Centre line, so a silent passage is visibly silent rather than
        // merely thin — the whole judgement being made when picking a noise
        // print.
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.drawHorizontalLine((int) centreY, (float) lane.getX(), (float) lane.getRight());

        const double secondsPerPixel = geometry_.secondsPerPixel > 0.0 ? geometry_.secondsPerPixel : 1.0e-9;
        const bool   showGhost       = std::abs(gainDb_) > 0.05f;

        for (int x = lane.getX(); x < lane.getRight(); ++x)
        {
            const double startSeconds = geometry_.visibleStartSeconds
                                      + (double) (x - (int) geometry_.contentLeft) * secondsPerPixel;
            const double endSeconds   = startSeconds + secondsPerPixel;

            const int from = (int) (startSeconds * peaksSampleRate_);
            const int to   = (int) (endSeconds * peaksSampleRate_);
            if (to <= 0 || from >= peaks_.totalSamples())
                continue;

            const auto bin = peaks_.range(channel, juce::jmax(0, from), juce::jmax(1, to));
            if (bin.isEmpty())
                continue;

            // The original, behind: without something to compare against, a
            // quieter waveform just looks like a quieter recording.
            if (showGhost)
            {
                g.setColour(juce::Colours::white.withAlpha(0.16f));
                g.drawVerticalLine(x, centreY - bin.maximum * halfH, centreY - bin.minimum * halfH);
            }

            const float top    = juce::jlimit(-1.0f, 1.0f, bin.maximum * gain);
            const float bottom = juce::jlimit(-1.0f, 1.0f, bin.minimum * gain);
            const bool  clips  = bin.magnitude() * gain > 1.0f;

            // Clipping in red rather than silently flattened at the lane
            // edge: a waveform that just touches the top looks the same
            // whether it is at full scale or 6dB past it, and those are very
            // different problems.
            g.setColour(clips ? juce::Colours::red
                              : juce::Colours::aquamarine.withAlpha(0.85f));
            g.drawVerticalLine(x, centreY - top * halfH, centreY - bottom * halfH);
        }
    }

    void setSelection(AudioRange range)
    {
        selection_ = range;
        updateSelectionLabel();
        updateNoiseControls();
        repaint();
    }

    /** Capture needs something selected; reduce needs something captured.
        Spelling the workflow out in the controls is what stops "Reduce
        Noise" being a button that silently does nothing. */
    void updateNoiseControls()
    {
        captureNoiseButton_.setEnabled(! selection_.isEmpty());
        reduceNoiseButton_.setEnabled(noisePrintCaptured_);
        noiseAmountSlider_.setEnabled(noisePrintCaptured_);
        noiseFloorSlider_.setEnabled(noisePrintCaptured_);
    }

    void notifySelection()
    {
        if (onSelectionChanged)
            onSelectionChanged(selection_);
    }

    void updateSelectionLabel()
    {
        if (selection_.isEmpty())
        {
            selectionLabel_.setText("No selection (actions apply to the whole clip)",
                                    juce::dontSendNotification);
            return;
        }

        selectionLabel_.setText(juce::String(selection_.startSeconds, 3) + "s - "
                                    + juce::String(selection_.endSeconds, 3) + "s  ("
                                    + juce::String(selection_.lengthSeconds(), 3) + "s)",
                                juce::dontSendNotification);
    }

    void zoomBy(double factor)
    {
        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        // Anchored on the centre of the view, so zooming doesn't wander off
        // whatever the user was looking at.
        const double centre  = geometry_.visibleStartSeconds
                             + geometry_.visibleSeconds((float) area.getWidth()) * 0.5;
        const double fitPerPixel = geometry_.secondsPerPixelToFit((float) area.getWidth());

        geometry_.secondsPerPixel =
            juce::jlimit(1.0e-6, juce::jmax(1.0e-6, fitPerPixel), geometry_.secondsPerPixel * factor);

        geometry_.visibleStartSeconds =
            geometry_.clampedStart(centre - geometry_.visibleSeconds((float) area.getWidth()) * 0.5,
                                   (float) area.getWidth());
        repaint();
    }

    void zoomToFit()
    {
        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        geometry_.secondsPerPixel     = geometry_.secondsPerPixelToFit((float) area.getWidth());
        geometry_.visibleStartSeconds = 0.0;
        repaint();
    }

    /** Every control this pane shows and hides, in one place — see the same
        list in FretboardPane and SynthEditor, and the bug that made it worth
        having. */
    std::vector<juce::Component*> managedControls()
    {
        return { &playButton_,
                 &cutButton_, &copyButton_, &pasteButton_, &deleteButton_, &trimButton_,
                 &splitButton_, &silenceButton_, &fadeInButton_, &fadeOutButton_, &reverseButton_,
                 &effectsButton_, &speedPitchButton_,
                 &selectionLabel_, &gainLabel_, &gainSlider_, &normaliseButton_,
                 &zoomInButton_, &zoomOutButton_, &zoomFitButton_,
                 &selectAllButton_, &clearSelectionButton_,
                 &captureNoiseButton_, &reduceNoiseButton_,
                 &noiseAmountLabel_, &noiseAmountSlider_,
                 &noiseFloorLabel_, &noiseFloorSlider_ };
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholderLabel_.setVisible(! visible);

        for (auto* c : managedControls())
            c->setVisible(visible);

        updateSelectionLabel();
        updateNoiseControls();
        resized();
        repaint();
    }

    AudioSelection geometry_;
    AudioRange     selection_;

    juce::File   file_;
    juce::String trackName_;
    juce::uint32 trackColour_    = 0;
    bool         contentVisible_ = false;
    bool         updating_       = false;
    bool         dragging_       = false;
    double       dragAnchorSeconds_ = 0.0;

    juce::Label      placeholderLabel_;
    juce::Label      selectionLabel_, gainLabel_;
    juce::Slider     gainSlider_;
    juce::TextButton normaliseButton_, zoomInButton_, zoomOutButton_, zoomFitButton_;
    juce::TextButton selectAllButton_, clearSelectionButton_;
    juce::TextButton playButton_;
    juce::TextButton cutButton_, copyButton_, pasteButton_, deleteButton_, trimButton_;
    juce::TextButton splitButton_, silenceButton_, fadeInButton_, fadeOutButton_, reverseButton_;
    juce::TextButton effectsButton_, speedPitchButton_;
    juce::TextButton captureNoiseButton_, reduceNoiseButton_;
    juce::Label      noiseAmountLabel_, noiseFloorLabel_;
    juce::Slider     noiseAmountSlider_, noiseFloorSlider_;
    bool             noisePrintCaptured_ = false;
    bool             playing_            = false;
    bool             draggedFar_         = false;
    bool             fileDragActive_     = false;
    double           playheadSeconds_    = 0.0;
    WaveformPeaks    peaks_;
    double           peaksSampleRate_ = 0.0;
    float            gainDb_          = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEditorPane)
};

} // namespace looper
