#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Track.h"

#include "AudioFileTypes.h"
#include "AudioSelection.h"
#include "TrackColours.h"
#include "SampleDetail.h"
#include "SampleDraw.h"
#include "SpectrogramImage.h"
#include "WaveformScale.h"
#include "WaveformPeaks.h"

namespace soundsplice
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
        step, exactly as the mixer faders do. */
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

    /** The pane is zoomed in past what its peaks can show and needs the
        actual samples of [fromSeconds, toSeconds) of the clip, answered with
        setSampleDetail. This pane reads no files itself. */
    std::function<void(double fromSeconds, double toSeconds)> onSampleDetailNeeded;

    /** A stroke of the draw tool (Alt-drag, zoomed in until the samples are
        drawn as a line) has ended: samples [@p firstSample, @p firstSample +
        values.size()) of @p channel, counted from the clip's start, should
        now be @p values. The pane shows the stroke only while it's being
        drawn; the edit is the owner's, and what it writes comes back as new
        peaks and samples. */
    std::function<void(int channel, long firstSample, const std::vector<float>& values)> onSamplesDrawn;

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

        // Every managed control is parented up front: a control that was
        // never parented lays out and hides perfectly while drawing nothing.
        for (auto* control : managedControls())
            addChildComponent(control);

        setContentVisible(false);
    }

    /** Shows a clip for editing: @p lengthSeconds of @p file, starting
        @p windowStartSeconds into it. Every position this pane reports is in
        seconds from that start, not from the start of the file. @p gainDb is
        the clip's current trim. */
    void setClip(const juce::File& file, double lengthSeconds, float gainDb,
                 const juce::String& trackName, juce::uint32 trackColour,
                 double windowStartSeconds = 0.0)
    {
        // A trimmed or split clip can be a different stretch of the same
        // file, which moves every position in it just as a new file would.
        const bool sameWindow    = std::abs(windowStartSeconds - windowStartSeconds_) <= 1.0e-9
                                && std::abs(lengthSeconds - geometry_.fileLengthSeconds) <= 1.0e-9;
        // A stroke of the draw tool writes the clip to a new file of the same
        // length; being thrown back out to the whole clip after every stroke
        // would make drawing unusable.
        const bool differentFile = (file != file_ || ! sameWindow) && ! (keepViewForRedraw_ && sameWindow);

        file_               = file;
        windowStartSeconds_ = windowStartSeconds;
        trackName_   = trackName;
        trackColour_ = trackColour;

        geometry_.fileLengthSeconds = juce::jmax(0.0, lengthSeconds);

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

        sampleDetail_ = {}; // another clip's, or another window of this one
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

    /** What the healing brush has painted on the spectrogram (Ctrl-drag, Cmd
        on a Mac), if anything: the selection then spans its time. */
    std::optional<spectrogramimage::Brush> spectralBrush() const
    {
        if (! spectrogramView_ || brush_.isEmpty() || selection_.isEmpty())
            return std::nullopt;
        return brush_;
    }

    /** The frequencies the selection covers, lowest first, when it was made
        as a box on the spectrogram: a spectral selection. Nothing for a
        selection of every frequency. */
    std::optional<std::pair<double, double>> frequencyBand() const
    {
        if (! spectrogramView_ || selection_.isEmpty() || bandHighHz_ <= bandLowHz_ || ! brush_.isEmpty())
            return std::nullopt;
        return std::pair { bandLowHz_, bandHighHz_ };
    }

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

        // New audio: any samples held for a close zoom describe the old.
        sampleDetail_ = {};
        requestSampleDetailIfZoomedIn();
        repaint();
    }

    /** Draws the waveform with height following level in decibels rather
        than sample value (see app/WaveformScale.h), so quiet passages can be
        seen. A view setting: the audio is the same either way. */
    void setDbScale(bool db)
    {
        if (db == dbScale_)
            return;
        dbScale_ = db;
        repaint();
    }

    bool showsDbScale() const noexcept { return dbScale_; }

    /** Shows the clip as a spectrogram, level by frequency over time, in
        place of the waveform. The owner builds it (setSpectrogram) as it
        builds the waveform's peaks, and only while this is on. */
    void setSpectrogramView(bool on)
    {
        if (on == spectrogramView_)
            return;
        spectrogramView_ = on;
        repaint();
    }

    bool showsSpectrogram() const noexcept { return spectrogramView_; }

    /** How frequency is laid out up the spectrogram. The analysis is kept,
        so changing it redraws without reading the clip again. */
    void setSpectrogramScale(spectrogramimage::Scale scale)
    {
        if (scale == spectrogramScale_)
            return;
        spectrogramScale_ = scale;
        if (! spectrogramData_.isEmpty())
            spectrogram_ = spectrogramimage::imageOf(spectrogramData_, 256, spectrogramScale_);
        repaint();
    }

    spectrogramimage::Scale spectrogramScale() const noexcept { return spectrogramScale_; }

    void setSpectrogram(const engine::SpectrogramData& data)
    {
        spectrogramData_     = data;
        spectrogram_         = spectrogramimage::imageOf(data, 256, spectrogramScale_);
        spectrogramColumns_  = data.columns;
        spectrogramSeconds_  = data.secondsPerColumn;
        spectrogramWindow_   = data.windowSeconds;
        spectrogramNyquist_  = data.sampleRate * 0.5;
        repaint();
    }

    void clearSpectrogram()
    {
        spectrogram_     = {};
        spectrogramData_ = {};
        repaint();
    }

    /** The samples asked for through onSampleDetailNeeded. */
    void setSampleDetail(SampleDetail detail)
    {
        sampleDetail_ = std::move(detail);
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
        if (! selection_.isEmpty() && ! frequencyBand())
        {
            const float x1 = juce::jlimit((float) area.getX(), (float) area.getRight(),
                                          geometry_.xForSeconds(selection_.startSeconds));
            const float x2 = juce::jlimit((float) area.getX(), (float) area.getRight(),
                                          geometry_.xForSeconds(selection_.endSeconds));
            g.setColour(juce::Colours::cyan.withAlpha(0.22f));
            g.fillRect(juce::Rectangle<float>(x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
        }

        paintWaveform(g, area);

        if (spectrogramView_ && ! brush_.isEmpty())
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(area);
            g.setColour(juce::Colours::cyan.withAlpha(0.28f));
            const float rx = (float) (brush_.radiusSeconds / juce::jmax(1.0e-12, geometry_.secondsPerPixel));
            const float ry = (float) (brush_.radiusProportion * area.getHeight());
            for (const auto& dab : brush_.dabs)
            {
                const float x = geometry_.xForSeconds(dab.seconds);
                const float y = (float) area.getBottom() - (float) dab.proportion * (float) area.getHeight();
                g.fillEllipse(x - rx, y - ry, rx * 2.0f, ry * 2.0f);
            }
        }

        // A spectral selection is a box, drawn over the spectrogram since it
        // has no background to sit on as the waveform does.
        if (const auto band = frequencyBand())
        {
            const float x1 = geometry_.xForSeconds(selection_.startSeconds);
            const float x2 = geometry_.xForSeconds(selection_.endSeconds);
            const float y1 = yForHz(band->second, area);
            const float y2 = yForHz(band->first, area);
            const auto  box = juce::Rectangle<float>(x1, y1, x2 - x1, y2 - y1).getIntersection(area.toFloat());
            g.setColour(juce::Colours::cyan.withAlpha(0.15f));
            g.fillRect(box);
            g.setColour(juce::Colours::cyan.withAlpha(0.9f));
            g.drawRect(box, 1.0f);
        }

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

        if (! selection_.isEmpty() && ! frequencyBand())
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
        if (spectrogramView_)
        {
            paintSpectrogram(g, area);
            return;
        }

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

    /** The spectrogram over the part of the clip in view: each column is
        placed at the middle of the window it was measured over, so a sound
        lines up with where the waveform would show it. Frequency is marked
        up the left. */
    static constexpr float kBrushRadiusPixels = 9.0f;

    /** Starts a painting: the brush is sized from the view as it is now, so
        it covers on the spectrogram what it covers on screen. */
    void beginBrush(juce::Point<float> position)
    {
        const auto area = waveformArea();
        brush_.dabs.clear();
        brush_.radiusSeconds    = kBrushRadiusPixels * juce::jmax(1.0e-9, geometry_.secondsPerPixel);
        brush_.radiusProportion = kBrushRadiusPixels / (double) juce::jmax(1, area.getHeight());
        brush_.scale            = spectrogramScale_;
        brush_.nyquist          = spectrogramNyquist_;
        bandLowHz_ = bandHighHz_ = 0.0;
        painting_               = true;
        lastBrushPoint_         = position;
        addDab(position);
        repaint();
    }

    /** Dabs along the way from the last point, close enough to make a solid
        stroke however fast the mouse moved. */
    void paintBrushTo(juce::Point<float> position)
    {
        const float distance = lastBrushPoint_.getDistanceFrom(position);
        const int   steps    = juce::jmax(1, (int) std::ceil(distance / (kBrushRadiusPixels * 0.5f)));
        for (int i = 1; i <= steps; ++i)
            addDab(lastBrushPoint_ + (position - lastBrushPoint_) * ((float) i / (float) steps));
        lastBrushPoint_ = position;
        repaint();
    }

    void addDab(juce::Point<float> position)
    {
        const auto area = waveformArea();
        spectrogramimage::Brush::Dab dab;
        dab.seconds    = geometry_.secondsForX(position.x);
        dab.proportion = juce::jlimit(0.0, 1.0, 1.0 - (double) (position.y - (float) area.getY()) / (double) juce::jmax(1, area.getHeight()));
        brush_.dabs.push_back(dab);
    }

    /** The painting's time becomes the selection, which is what the edit
        reads and writes; its frequencies are the brush's own. */
    void endBrush()
    {
        painting_ = false;
        if (brush_.isEmpty())
            return;

        const auto [from, to] = brush_.timeSpan();
        const auto dabs       = brush_.dabs;
        setSelection(AudioRange::fromDrag(from, to).clampedTo(geometry_.fileLengthSeconds));
        brush_.dabs = dabs; // setSelection keeps it unless the range came out empty
        if (selection_.isEmpty())
            brush_.dabs.clear();
        notifySelection();
        repaint();
    }

    /** The frequency at height @p y in the spectrogram's area. */
    double hzForY(float y) const
    {
        const auto   area       = waveformArea();
        const double proportion = area.getHeight() > 0
                                    ? 1.0 - (double) (y - (float) area.getY()) / (double) area.getHeight()
                                    : 0.0;
        return spectrogramimage::frequencyAt(proportion, spectrogramNyquist_, spectrogramScale_);
    }

    float yForHz(double hz, juce::Rectangle<int> area) const
    {
        return (float) area.getBottom()
             - (float) spectrogramimage::proportionOf(hz, spectrogramNyquist_, spectrogramScale_) * (float) area.getHeight();
    }

    void paintSpectrogram(juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (! spectrogram_.isValid() || spectrogramSeconds_ <= 0.0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawText("Reading spectrogram...", area, juce::Justification::centred);
            return;
        }

        const double viewFrom   = geometry_.visibleStartSeconds;
        const double viewTo     = viewFrom + geometry_.visibleSeconds((float) area.getWidth());
        const double firstAt    = spectrogramWindow_ * 0.5;
        const double columnFrom = (viewFrom - firstAt) / spectrogramSeconds_;
        const double columnTo   = (viewTo - firstAt) / spectrogramSeconds_;

        // The columns in view, stretched to the area: a clip the view shows
        // less than all of stretches its columns wider rather than repeating.
        const auto source = juce::Rectangle<float>((float) columnFrom, 0.0f, (float) (columnTo - columnFrom),
                                                   (float) spectrogram_.getHeight());
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(area);
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            const auto transform = juce::AffineTransform::translation(-source.getX(), 0.0f)
                                       .scaled((float) area.getWidth() / source.getWidth(),
                                               (float) area.getHeight() / source.getHeight())
                                       .translated((float) area.getX(), (float) area.getY());
            g.drawImageTransformed(spectrogram_, transform);
        }

        g.setFont(juce::FontOptions(10.0f));
        const auto marks = spectrogramScale_ == spectrogramimage::Scale::Logarithmic
                             ? std::vector<double> { 100.0, 1000.0, 10000.0 }
                             : std::vector<double> { 1000.0, 2000.0, 5000.0, 10000.0, 15000.0 };
        for (double hz : marks)
        {
            if (hz >= spectrogramNyquist_)
                continue;
            const float y = yForHz(hz, area);
            g.setColour(juce::Colours::white.withAlpha(0.18f));
            g.drawHorizontalLine((int) y, (float) area.getX(), (float) area.getRight());
            g.setColour(juce::Colours::white.withAlpha(0.6f));
            g.drawText(hz >= 1000.0 ? juce::String((int) (hz / 1000.0)) + " kHz" : juce::String((int) hz) + " Hz",
                       juce::Rectangle<float>((float) area.getX() + 3.0f, y - 12.0f, 60.0f, 11.0f),
                       juce::Justification::centredLeft);
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

        if (e.mods.isAltDown() && canDrawSamples())
        {
            beginStroke(e.position);
            return;
        }

        // Painting with the healing brush: Ctrl-drag on the spectrogram.
        if (spectrogramView_ && e.mods.isCommandDown())
        {
            beginBrush(e.position);
            return;
        }

        brush_.dabs.clear();

        dragAnchorSeconds_ = geometry_.secondsForX((float) e.position.x);
        dragAnchorHz_      = hzForY(e.position.y);
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
        if (drawing_)
        {
            continueStroke(e.position);
            return;
        }

        if (painting_)
        {
            paintBrushTo(e.position);
            return;
        }

        if (! dragging_)
            return;

        // A few pixels of travel separates "clicked to place the cursor" from
        // "dragged to select"; without a threshold, the hand-wobble in any
        // real click would leave a one-pixel selection behind.
        if (std::abs(e.getDistanceFromDragStartX()) > kDragThresholdPixels)
            draggedFar_ = true;

        if (draggedFar_)
        {
            // On the spectrogram a drag is a box, time across and frequency
            // up, unless it hardly moved vertically, which selects every
            // frequency as a drag over the waveform does.
            if (spectrogramView_ && std::abs(e.getDistanceFromDragStartY()) > kDragThresholdPixels)
            {
                const double hz = hzForY(e.position.y);
                bandLowHz_      = std::min(dragAnchorHz_, hz);
                bandHighHz_     = std::max(dragAnchorHz_, hz);
            }
            else
            {
                bandLowHz_ = bandHighHz_ = 0.0;
            }

            setSelection(AudioRange::fromDrag(dragAnchorSeconds_,
                                              geometry_.secondsForX((float) e.position.x))
                             .clampedTo(geometry_.fileLengthSeconds));
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (drawing_)
        {
            endStroke();
            return;
        }

        if (painting_)
        {
            endBrush();
            return;
        }

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

    void mouseMove(const juce::MouseEvent& e) override
    {
        updateDrawCursor(e.mods, e.getPosition());
    }

    void modifierKeysChanged(const juce::ModifierKeys& mods) override
    {
        updateDrawCursor(mods, getMouseXYRelative());
    }

    /** Whether the draw tool works at the current zoom: the samples are held
        for the whole view and far enough apart to be drawn as a line, so a
        stroke lands on the samples the eye is placing it on. */
    bool canDrawSamples() const
    {
        const auto area = waveformArea();
        if (! contentVisible_ || area.isEmpty() || sampleDetail_.isEmpty())
            return false;

        const double from = geometry_.visibleStartSeconds;
        const double to   = from + geometry_.visibleSeconds((float) area.getWidth());
        return geometry_.secondsPerPixel * sampleDetail_.sampleRate < 1.0
            && sampleDetail_.covers(from, to, geometry_.fileLengthSeconds);
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
        // work — which is what fixed widths here once produced.
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

        requestSampleDetailIfZoomedIn(); // a wider pane shows more samples
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
        const auto  height  = [this](float sample) { return waveformscale::heightFor(sample, dbScale_); };

        // dBFS gridlines. Levels are judged in decibels, and a linear
        // waveform with no reference makes -6 and -12 look nearly identical.
        // The dB scale spreads its range evenly, so its lines are wider apart.
        const std::initializer_list<float> linearLines { -6.0f, -12.0f, -18.0f };
        const std::initializer_list<float> dbLines     { -12.0f, -24.0f, -36.0f, -48.0f };
        for (float db : dbScale_ ? dbLines : linearLines)
        {
            const float fraction = waveformscale::heightFor(juce::Decibels::decibelsToGain(db), dbScale_);
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

        // Zoomed in past what the peaks can show: draw the samples themselves,
        // once they've arrived (until then, the peaks, blocky but not blank).
        const double viewFrom = geometry_.visibleStartSeconds
                              + (double) ((float) lane.getX() - geometry_.contentLeft) * secondsPerPixel;
        const double viewTo   = viewFrom + (double) lane.getWidth() * secondsPerPixel;
        if (SampleDetail::wanted(secondsPerPixel, peaksSampleRate_)
            && sampleDetail_.covers(viewFrom, viewTo, geometry_.fileLengthSeconds))
        {
            paintChannelFromSamples(g, lane, channel, gain, centreY, halfH);
            return;
        }

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
                g.drawVerticalLine(x, centreY - height(bin.maximum) * halfH, centreY - height(bin.minimum) * halfH);
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
            g.drawVerticalLine(x, centreY - height(top) * halfH, centreY - height(bottom) * halfH);

            // The RMS level inside the peaks, lighter, as Audacity draws it:
            // the peaks show the loudest instant, RMS how loud it sounds, and
            // two passages with the same peaks can be very different to hear.
            // Kept within the peaks, so it never draws past them.
            const float level = juce::jlimit(0.0f, 1.0f,
                                             peaks_.rms(channel, juce::jmax(0, from), juce::jmax(1, to)) * gain);
            const float rmsTop    = centreY - height(juce::jmin(level, top)) * halfH;
            const float rmsBottom = centreY - height(juce::jmax(-level, bottom)) * halfH;
            if (level > 0.0f && rmsBottom > rmsTop)
            {
                g.setColour(juce::Colours::white.withAlpha(0.45f));
                g.drawVerticalLine(x, rmsTop, rmsBottom);
            }
        }
    }

    /** One channel drawn from its actual samples rather than its peaks, for a
        view zoomed in to a few samples per pixel or fewer. At a sample a pixel
        or more, each pixel is the exact extremes of its samples; closer than
        that, the samples are joined up as the line they are, and marked once
        they're far enough apart to pick out, which is where a click or an
        edit point is placed by eye. Anything the clip's gain would push past
        full scale is red, as in the peaks view. */
    void paintChannelFromSamples(juce::Graphics& g, juce::Rectangle<int> lane, int channel, float gain,
                                 float centreY, float halfH)
    {
        juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(lane);

        const double secondsPerPixel = geometry_.secondsPerPixel;
        const double samplesPerPixel = secondsPerPixel * sampleDetail_.sampleRate;
        const auto   yFor            = [this, centreY, halfH, gain](float sample)
        {
            return centreY - waveformscale::heightFor(sample * gain, dbScale_) * halfH;
        };

        if (samplesPerPixel >= 1.0)
        {
            for (int x = lane.getX(); x < lane.getRight(); ++x)
            {
                const double start = geometry_.visibleStartSeconds
                                   + (double) ((float) x - geometry_.contentLeft) * secondsPerPixel;
                const auto bin = sampleDetail_.range(channel, start, start + secondsPerPixel);
                if (bin.isEmpty())
                    continue;

                const float top    = yFor(bin.maximum);
                const float bottom = juce::jmax(top + 1.0f, yFor(bin.minimum));
                g.setColour(bin.magnitude() * gain > 1.0f ? juce::Colours::red
                                                          : juce::Colours::aquamarine.withAlpha(0.85f));
                g.drawVerticalLine(x, top, bottom);
            }
            return;
        }

        const double viewFrom = geometry_.visibleStartSeconds
                              + (double) ((float) lane.getX() - geometry_.contentLeft) * secondsPerPixel;
        const double viewTo   = viewFrom + (double) lane.getWidth() * secondsPerPixel;
        const long   first    = sampleDetail_.indexAt(viewFrom) - 1;
        const long   last     = sampleDetail_.indexAt(viewTo) + 1;
        const bool   marked   = 1.0 / samplesPerPixel >= 6.0;

        juce::Path line;
        bool       started = false;

        for (long i = first; i <= last; ++i)
        {
            float sample = 0.0f;
            if (! sampleDetail_.sampleAt(channel, i, sample))
                continue;

            const double seconds = sampleDetail_.startSeconds + (double) i / sampleDetail_.sampleRate;
            const float  x       = geometry_.xForSeconds(seconds);
            const float  y       = yFor(sample);

            if (! started)
            {
                line.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                line.lineTo(x, y);
            }

            if (marked)
            {
                g.setColour(std::abs(sample * gain) > 1.0f ? juce::Colours::red : juce::Colours::aquamarine);
                g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
            }
        }

        g.setColour(juce::Colours::aquamarine.withAlpha(0.85f));
        g.strokePath(line, juce::PathStrokeType(1.5f));
    }

    void setSelection(AudioRange range)
    {
        selection_ = range;
        if (selection_.isEmpty())
        {
            bandLowHz_ = bandHighHz_ = 0.0;
            brush_.dabs.clear();
        }
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

        auto text = juce::String(selection_.startSeconds, 3) + "s - " + juce::String(selection_.endSeconds, 3)
                  + "s  (" + juce::String(selection_.lengthSeconds(), 3) + "s)";
        if (const auto band = frequencyBand())
            text << ",  " << juce::String((int) std::lround(band->first)) << " - "
                 << juce::String((int) std::lround(band->second)) << " Hz";
        else if (spectrogramView_ && ! brush_.isEmpty())
            text << ",  painted";
        selectionLabel_.setText(text, juce::dontSendNotification);
    }

    int laneCount() const
    {
        return juce::jmax(1, peaks_.isEmpty() ? (int) sampleDetail_.channels.size() : peaks_.numChannels());
    }

    void updateDrawCursor(const juce::ModifierKeys& mods, juce::Point<int> position)
    {
        const bool draw = mods.isAltDown() && waveformArea().contains(position) && canDrawSamples();
        setMouseCursor(draw || drawing_ ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    }

    /** The sample index into sampleDetail_ nearest @p x, and the value a
        point at @p y on @p channel's lane stands for, undoing the clip's gain
        so the sample drawn is where the line shows it. */
    std::pair<long, float> strokePoint(juce::Point<float> position, int channel) const
    {
        const auto  area  = waveformArea();
        const int   laneH = area.getHeight() / laneCount();
        const auto  lane  = area.withY(area.getY() + channel * laneH).withHeight(laneH);
        const float halfH = juce::jmax(1.0f, (float) lane.getHeight() * 0.5f);
        const float gain  = juce::Decibels::decibelsToGain(gainDb_);

        const double seconds = geometry_.secondsForX(position.x);
        const long   index   = (long) std::lround((seconds - sampleDetail_.startSeconds) * sampleDetail_.sampleRate);
        const float  shown   = ((float) lane.getCentreY() - position.y) / halfH;
        return { index, gain > 0.0f ? waveformscale::sampleFor(shown, dbScale_) / gain : 0.0f };
    }

    void beginStroke(juce::Point<float> position)
    {
        const auto area  = waveformArea();
        const int  laneH = juce::jmax(1, area.getHeight() / laneCount());

        drawChannel_ = juce::jlimit(0, (int) sampleDetail_.channels.size() - 1,
                                    juce::jlimit(0, laneCount() - 1, ((int) position.y - area.getY()) / laneH));
        drawOriginal_ = sampleDetail_.channels[(size_t) drawChannel_];
        drawTouched_  = {};
        drawing_      = true;

        const auto [index, value] = strokePoint(position, drawChannel_);
        drawLastIndex_ = index;
        drawLastValue_ = value;
        strokeTo(index, value);
    }

    void continueStroke(juce::Point<float> position)
    {
        const auto [index, value] = strokePoint(position, drawChannel_);
        strokeTo(index, value);
        drawLastIndex_ = index;
        drawLastValue_ = value;
    }

    void strokeTo(long index, float value)
    {
        auto&      samples = sampleDetail_.channels[(size_t) drawChannel_];
        const auto touched = app::sampledraw::drawLine(samples, drawLastIndex_, drawLastValue_, index, value);
        if (! touched.isEmpty())
            drawTouched_.include(touched.first, touched.last);
        repaint();
    }

    void endStroke()
    {
        drawing_ = false;
        if (drawTouched_.isEmpty() || drawChannel_ >= (int) sampleDetail_.channels.size())
            return;

        auto&      samples = sampleDetail_.channels[(size_t) drawChannel_];
        const auto values  = std::vector<float>(samples.begin() + drawTouched_.first,
                                                samples.begin() + drawTouched_.last + 1);

        // Back to what the clip actually holds: if the edit is refused, the
        // view mustn't go on showing a stroke that was never written.
        const bool changed = values != std::vector<float>(drawOriginal_.begin() + drawTouched_.first,
                                                          drawOriginal_.begin() + drawTouched_.last + 1);
        samples = std::move(drawOriginal_);
        drawOriginal_.clear();
        repaint();

        if (! changed || ! onSamplesDrawn)
            return;

        const long first = (long) std::lround(sampleDetail_.startSeconds * sampleDetail_.sampleRate) + drawTouched_.first;

        keepViewForRedraw_ = true;
        onSamplesDrawn(drawChannel_, first, values);
        keepViewForRedraw_ = false;
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
        requestSampleDetailIfZoomedIn();
        repaint();
    }

    void zoomToFit()
    {
        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        geometry_.secondsPerPixel     = geometry_.secondsPerPixelToFit((float) area.getWidth());
        geometry_.visibleStartSeconds = 0.0;
        requestSampleDetailIfZoomedIn(); // a clip short enough can fit at sample level
        repaint();
    }

    /** Asks for the visible samples, and half a view either side so a small
        zoom doesn't read again, when the view is close enough to need them
        and what's held doesn't already cover it. */
    void requestSampleDetailIfZoomedIn()
    {
        const auto area = waveformArea();
        if (! contentVisible_ || area.isEmpty() || ! onSampleDetailNeeded
            || ! SampleDetail::wanted(geometry_.secondsPerPixel, peaksSampleRate_))
            return;

        const double from = geometry_.visibleStartSeconds;
        const double to   = from + geometry_.visibleSeconds((float) area.getWidth());
        if (sampleDetail_.covers(from, to, geometry_.fileLengthSeconds))
            return;

        const double margin = (to - from) * 0.5;
        onSampleDetailNeeded(juce::jmax(0.0, from - margin), juce::jmin(geometry_.fileLengthSeconds, to + margin));
    }

    /** Every control this pane shows and hides, in one place, so none can be
        missed when they are parented or their visibility is toggled. */
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
    double       windowStartSeconds_ = 0.0;
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
    SampleDetail     sampleDetail_; // the visible samples, once zoomed in past the peaks
    double           peaksSampleRate_ = 0.0;
    bool             dbScale_         = false;
    bool             spectrogramView_ = false;
    double           dragAnchorHz_    = 0.0;
    spectrogramimage::Brush brush_;
    bool             painting_        = false;
    juce::Point<float> lastBrushPoint_;
    double           bandLowHz_       = 0.0; // a spectral selection's band; none when equal
    double           bandHighHz_      = 0.0;
    juce::Image      spectrogram_;
    engine::SpectrogramData  spectrogramData_;
    spectrogramimage::Scale  spectrogramScale_ = spectrogramimage::Scale::Logarithmic;
    int              spectrogramColumns_ = 0;
    double           spectrogramSeconds_ = 0.0;
    double           spectrogramWindow_  = 0.0;
    double           spectrogramNyquist_ = 24000.0;

    // The draw tool's stroke in progress, drawn straight into sampleDetail_.
    bool                 drawing_           = false;
    bool                 keepViewForRedraw_ = false;
    int                  drawChannel_       = 0;
    long                 drawLastIndex_     = 0;
    float                drawLastValue_     = 0.0f;
    app::sampledraw::Touched drawTouched_;
    std::vector<float>   drawOriginal_; // the channel before the stroke
    float            gainDb_          = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEditorPane)
};

} // namespace soundsplice
