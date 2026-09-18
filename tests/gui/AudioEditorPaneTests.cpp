#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/AudioEditorPane.h>

#include <cmath>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** A pane showing a clip, in the state the app puts it in. The file need
        not exist: the thumbnail is what reads it, and every gesture below is
        about geometry, which comes from the length we pass in. */
    std::unique_ptr<AudioEditorPane> makeReadyPane(int width, int height, double seconds)
    {
        auto pane = std::make_unique<AudioEditorPane>();
        pane->setVisible(true);
        pane->setSize(width, height);
        pane->setClip(juce::File("/nonexistent/take.wav"), seconds, 0.0f, "Audio 1", 0xff3080ff);
        pane->resized();
        return pane;
    }

    juce::MouseEvent eventAt(juce::Component& target, juce::Point<float> position,
                             juce::Point<float> mouseDownPosition)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now    = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, juce::ModifierKeys(), 1.0f,
                                0.0f, 0.0f, 0.0f, 0.0f, &target, &target,
                                now, mouseDownPosition, now, 1, false);
    }

    // As in ArrangementViewTests: the mouse overrides are public here, but
    // dispatching through the base class keeps these helpers identical in
    // shape to the ones that need it.
    void sendMouseDown(AudioEditorPane& p, const juce::MouseEvent& e) { static_cast<juce::Component&>(p).mouseDown(e); }
    void sendMouseDrag(AudioEditorPane& p, const juce::MouseEvent& e) { static_cast<juce::Component&>(p).mouseDrag(e); }
    void sendMouseUp(AudioEditorPane& p, const juce::MouseEvent& e)   { static_cast<juce::Component&>(p).mouseUp(e); }

    /** The first button with @p text anywhere under @p root, or nullptr.
        Found through the component tree rather than a member, so the test
        sees what the pane actually presents. */
    juce::Button* findButton(juce::Component& root, const juce::String& text)
    {
        for (int i = 0; i < root.getNumChildComponents(); ++i)
        {
            auto* child = root.getChildComponent(i);
            if (auto* button = dynamic_cast<juce::Button*>(child))
                if (button->getButtonText() == text)
                    return button;
            if (auto* found = findButton(*child, text))
                return found;
        }
        return nullptr;
    }

    /** A point inside the waveform area, at a fraction across it. The pane
        reserves a header and two tool rows, so the vertical middle is
        reliably inside the waveform at any usable size. */
    juce::Point<float> waveformPoint(const AudioEditorPane& pane, float fractionAcross)
    {
        const auto bounds = pane.getLocalBounds();
        return { (float) bounds.getWidth() * fractionAcross, (float) bounds.getCentreY() };
    }
}

TEST_CASE("A drag across the waveform selects a range", "[gui][audioeditor]")
{
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    AudioRange reported;
    int        callbacks = 0;
    pane->onSelectionChanged = [&](AudioRange r) { reported = r; ++callbacks; };

    const auto from = waveformPoint(*pane, 0.25f);
    const auto to   = waveformPoint(*pane, 0.75f);

    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));

    REQUIRE_FALSE(pane->selection().isEmpty());
    REQUIRE(pane->selection() == reported);
    REQUIRE(callbacks > 0);

    // Roughly the middle of the view. Loose bounds on purpose: the exact
    // pixels depend on the pane's margins, and pinning them would make this a
    // layout test rather than a gesture one.
    //
    // Note the view is longer than the file — it extends past the end so the
    // cursor can be placed there to paste after the recording — so a fraction
    // across the view maps to a later time than the same fraction of the
    // file. The selection itself still clamps to the file.
    REQUIRE(pane->selection().startSeconds > 1.0);
    REQUIRE(pane->selection().endSeconds <= 10.0);
    REQUIRE(pane->selection().lengthSeconds() > 2.0);
}

TEST_CASE("Dragging right-to-left selects the same range", "[gui][audioeditor]")
{
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto left  = waveformPoint(*pane, 0.3f);
    const auto right = waveformPoint(*pane, 0.7f);

    sendMouseDown(*pane, eventAt(*pane, right, right));
    sendMouseDrag(*pane, eventAt(*pane, left, right));
    sendMouseUp(*pane, eventAt(*pane, left, right));
    const auto backwards = pane->selection();

    sendMouseDown(*pane, eventAt(*pane, left, left));
    sendMouseDrag(*pane, eventAt(*pane, right, left));
    sendMouseUp(*pane, eventAt(*pane, right, left));
    const auto forwards = pane->selection();

    REQUIRE_FALSE(backwards.isEmpty());
    REQUIRE(std::abs(backwards.startSeconds - forwards.startSeconds) < 1.0e-9);
    REQUIRE(std::abs(backwards.endSeconds - forwards.endSeconds) < 1.0e-9);
}

TEST_CASE("A plain click clears the selection", "[gui][audioeditor]")
{
    // "Click somewhere to deselect" is the expected gesture, and an empty
    // range is how the pane spells "no selection" — which the owner reads as
    // "apply to the whole clip".
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto from = waveformPoint(*pane, 0.2f);
    const auto to   = waveformPoint(*pane, 0.8f);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));
    REQUIRE_FALSE(pane->selection().isEmpty());

    const auto click = waveformPoint(*pane, 0.5f);
    sendMouseDown(*pane, eventAt(*pane, click, click));
    sendMouseUp(*pane, eventAt(*pane, click, click));

    REQUIRE(pane->selection().isEmpty());
}

TEST_CASE("A selection never runs outside the file", "[gui][audioeditor]")
{
    // Dragging past the end of the component is normal, and the range is
    // later used to index audio — so it has to saturate at the file's bounds
    // rather than running past them.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto from = waveformPoint(*pane, 0.5f);
    const juce::Point<float> wayPast { 100000.0f, from.y };

    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, wayPast, from));
    sendMouseUp(*pane, eventAt(*pane, wayPast, from));

    REQUIRE(pane->selection().startSeconds >= 0.0);
    REQUIRE(pane->selection().endSeconds <= 10.0);
}

TEST_CASE("Selecting a different clip drops the previous selection", "[gui][audioeditor]")
{
    // A selection is a position in one particular file. Carrying it to
    // another would point at unrelated audio while looking deliberate —
    // which for a destructive action is the worst kind of wrong.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto from = waveformPoint(*pane, 0.2f);
    const auto to   = waveformPoint(*pane, 0.8f);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));
    REQUIRE_FALSE(pane->selection().isEmpty());

    pane->setClip(juce::File("/nonexistent/other.wav"), 4.0, 0.0f, "Audio 2", 0xff30ff80);
    REQUIRE(pane->selection().isEmpty());
}

TEST_CASE("Re-showing the same clip keeps the selection", "[gui][audioeditor]")
{
    // The refresh cascade re-shows the current clip on all sorts of
    // unrelated edits. If that dropped the selection, it would vanish
    // whenever anything else in the app changed.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto from = waveformPoint(*pane, 0.2f);
    const auto to   = waveformPoint(*pane, 0.8f);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));
    const auto before = pane->selection();

    pane->setClip(juce::File("/nonexistent/take.wav"), 10.0, -3.0f, "Audio 1", 0xff3080ff);
    REQUIRE(pane->selection() == before);
}

TEST_CASE("Clicking outside the waveform doesn't start a selection", "[gui][audioeditor]")
{
    // The tool rows carry buttons; a press that lands on them must not also
    // begin a drag underneath.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const juce::Point<float> onToolRow { 400.0f, 4.0f };
    sendMouseDown(*pane, eventAt(*pane, onToolRow, onToolRow));
    sendMouseDrag(*pane, eventAt(*pane, waveformPoint(*pane, 0.9f), onToolRow));
    sendMouseUp(*pane, eventAt(*pane, waveformPoint(*pane, 0.9f), onToolRow));

    REQUIRE(pane->selection().isEmpty());
}

TEST_CASE("A pane with no clip ignores gestures", "[gui][audioeditor]")
{
    JuceFixture fixture;
    auto        pane = std::make_unique<AudioEditorPane>();
    pane->setVisible(true);
    pane->setSize(800, 300);
    pane->setNoAudioClipSelected();

    const juce::Point<float> middle { 400.0f, 150.0f };
    sendMouseDown(*pane, eventAt(*pane, middle, middle));
    sendMouseDrag(*pane, eventAt(*pane, { 600.0f, 150.0f }, middle));
    sendMouseUp(*pane, eventAt(*pane, { 600.0f, 150.0f }, middle));

    REQUIRE(pane->selection().isEmpty());
}

TEST_CASE("Capture Noise Print is only offered with a selection", "[gui][audioeditor]")
{
    // The workflow spelled out in the controls: a print taken from the whole
    // clip would describe the material as much as the noise, so capturing
    // without a selection must not be reachable in the first place.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    auto* capture = findButton(*pane, "Capture Noise Print");
    REQUIRE(capture != nullptr);
    REQUIRE_FALSE(capture->isEnabled());

    const auto from = waveformPoint(*pane, 0.2f);
    const auto to   = waveformPoint(*pane, 0.4f);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));

    REQUIRE(capture->isEnabled());
}

TEST_CASE("Reduce Noise is only offered once a print exists", "[gui][audioeditor]")
{
    // Otherwise it's a button that silently does nothing, which reads as the
    // feature being broken rather than as a step being missing.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    auto* reduce = findButton(*pane, "Reduce Noise");
    REQUIRE(reduce != nullptr);
    REQUIRE_FALSE(reduce->isEnabled());

    pane->setNoisePrintCaptured(true);
    REQUIRE(reduce->isEnabled());
}

TEST_CASE("A noise print doesn't survive switching to another clip", "[gui][audioeditor]")
{
    // A print describes one recording's noise floor. Carrying it to a
    // different file would subtract the wrong spectrum while looking
    // deliberate — and this action rewrites audio.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    pane->setNoisePrintCaptured(true);
    auto* reduce = findButton(*pane, "Reduce Noise");
    REQUIRE(reduce != nullptr);
    REQUIRE(reduce->isEnabled());

    pane->setClip(juce::File("/nonexistent/other.wav"), 5.0, 0.0f, "Audio 2", 0xff30ff80);
    REQUIRE_FALSE(reduce->isEnabled());
}

namespace
{
    /** Renders the pane into an image and reports how many pixels differ from
        a second render. Comparing what is actually *drawn* is the only way to
        catch "the edit is applied but invisible", which is exactly the bug
        the waveform renderer replaced AudioThumbnail to fix. */
    juce::Image renderPane(AudioEditorPane& pane)
    {
        juce::Image image(juce::Image::ARGB, pane.getWidth(), pane.getHeight(), true);
        juce::Graphics g(image);
        pane.paintEntireComponent(g, true);
        return image;
    }

    int differingPixels(const juce::Image& a, const juce::Image& b)
    {
        int count = 0;
        for (int y = 0; y < a.getHeight(); ++y)
            for (int x = 0; x < a.getWidth(); ++x)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                    ++count;
        return count;
    }

    /** A loud, obviously-shaped signal so a gain change has something big to
        scale. */
    WaveformPeaks loudPeaks(int samples)
    {
        std::vector<float> channel((size_t) samples);
        for (int i = 0; i < samples; ++i)
            channel[(size_t) i] = 0.8f * (float) std::sin(0.01 * i);

        WaveformPeaks peaks;
        peaks.build({ channel });
        return peaks;
    }
}

TEST_CASE("Changing the clip gain visibly changes the waveform", "[gui][audioeditor]")
{
    // The bug this renderer exists for: the waveform was drawn from the file
    // on disk with a hardcoded vertical zoom of 1.0, so turning the gain
    // down or pressing Normalize left it pixel-identical. The edit was real
    // and completely invisible.
    JuceFixture fixture;

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setSize(600, 300);
    pane.setClip(juce::File("/nonexistent/take.wav"), 10.0, 0.0f, "Audio 1", 0xff3080ff);
    pane.setWaveform(loudPeaks(48000 * 10), 48000.0);
    pane.resized();

    const auto atUnity = renderPane(pane);

    pane.setClip(juce::File("/nonexistent/take.wav"), 10.0, -18.0f, "Audio 1", 0xff3080ff);
    const auto quieter = renderPane(pane);

    INFO(differingPixels(atUnity, quieter) << " pixels changed");
    REQUIRE(differingPixels(atUnity, quieter) > 100);
}

TEST_CASE("A waveform with no peaks yet says so rather than drawing nothing",
          "[gui][audioeditor]")
{
    // Peaks are read on the message thread when the file changes, so there
    // is a frame where a clip is shown with none. A blank lane is
    // indistinguishable from silence.
    JuceFixture fixture;

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setSize(600, 300);
    pane.setClip(juce::File("/nonexistent/take.wav"), 10.0, 0.0f, "Audio 1", 0xff3080ff);
    pane.resized();

    // Renders without crashing, and is not simply an empty lane.
    const auto empty = renderPane(pane);
    pane.setWaveform(loudPeaks(48000), 48000.0);
    const auto drawn = renderPane(pane);

    REQUIRE(differingPixels(empty, drawn) > 100);
}

TEST_CASE("Clicking the waveform asks to move the song playhead", "[gui][audioeditor]")
{
    // The editor shows the song's timeline, not one of its own — clicking it
    // is the same gesture as clicking the ruler in the Tracks pane.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    double requested = -1.0;
    int    calls     = 0;
    pane->onSeekRequested = [&](double seconds) { requested = seconds; ++calls; };

    const auto point = waveformPoint(*pane, 0.5f);
    sendMouseDown(*pane, eventAt(*pane, point, point));
    sendMouseUp(*pane, eventAt(*pane, point, point));

    REQUIRE(calls == 1);
    REQUIRE(requested > 3.0);
    REQUIRE(requested < 7.0);
}

TEST_CASE("Dragging to select does not also seek", "[gui][audioeditor]")
{
    // Otherwise every selection would yank the playhead to wherever the drag
    // happened to start.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    int calls = 0;
    pane->onSeekRequested = [&](double) { ++calls; };

    const auto from = waveformPoint(*pane, 0.2f);
    const auto to   = waveformPoint(*pane, 0.8f);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));

    REQUIRE(calls == 0);
    REQUIRE_FALSE(pane->selection().isEmpty());
}

TEST_CASE("A tiny wobble is a click, not a selection", "[gui][audioeditor]")
{
    // Real clicks move a pixel or two. Without a threshold every one of them
    // would leave a sliver of selection behind, which the destructive
    // commands would then happily act on.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    int calls = 0;
    pane->onSeekRequested = [&](double) { ++calls; };

    const auto point   = waveformPoint(*pane, 0.5f);
    const juce::Point<float> wobble { point.x + 1.0f, point.y };

    sendMouseDown(*pane, eventAt(*pane, point, point));
    sendMouseDrag(*pane, eventAt(*pane, wobble, point));
    sendMouseUp(*pane, eventAt(*pane, wobble, point));

    REQUIRE(calls == 1);
    REQUIRE(pane->selection().isEmpty());
}

TEST_CASE("The playhead is drawn even when stopped", "[gui][audioeditor]")
{
    // It is the edit cursor as well as the playhead. A cursor you can place
    // but not see would be no use for deciding where to click next.
    JuceFixture fixture;

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setSize(600, 300);
    pane.setClip(juce::File("/nonexistent/take.wav"), 10.0, 0.0f, "Audio 1", 0xff3080ff);
    pane.setWaveform(loudPeaks(48000 * 10), 48000.0);
    pane.resized();

    const auto atStart = renderPane(pane);
    pane.setPlaybackState(false, 5.0); // stopped, halfway through
    const auto moved = renderPane(pane);

    INFO(differingPixels(atStart, moved) << " pixels changed");
    REQUIRE(differingPixels(atStart, moved) > 20);
}

TEST_CASE("The cursor can be placed past the end of the file", "[gui][audioeditor]")
{
    // Without room past the last sample there is nowhere to put the cursor to
    // paste after the recording — the click clamps back onto the end and the
    // paste lands in the wrong place.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    double requested = -1.0;
    pane->onSeekRequested = [&](double seconds) { requested = seconds; };

    // Fit shows the whole span including the tail room, so the far right of
    // the view is past the end of the file.
    const auto farRight = waveformPoint(*pane, 0.99f);
    sendMouseDown(*pane, eventAt(*pane, farRight, farRight));
    sendMouseUp(*pane, eventAt(*pane, farRight, farRight));

    INFO("cursor at " << requested << "s in a 10s file");
    REQUIRE(requested > 10.0);
    REQUIRE(pane->cursorSeconds() > 10.0);
}

TEST_CASE("A selection still can't run past the end of the file", "[gui][audioeditor]")
{
    // The cursor may go past the end; a selection may not. Selecting silence
    // that isn't in the file would make every destructive command act on a
    // range that doesn't exist.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto from = waveformPoint(*pane, 0.5f);
    const auto to   = waveformPoint(*pane, 0.99f);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));

    REQUIRE(pane->selection().endSeconds <= 10.0);
}

TEST_CASE("The cursor starts at zero for a fresh clip", "[gui][audioeditor]")
{
    // Paste with no selection uses the cursor, so a stale one carried from
    // another file would drop audio at an arbitrary point.
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 300, 10.0);

    const auto point = waveformPoint(*pane, 0.6f);
    sendMouseDown(*pane, eventAt(*pane, point, point));
    sendMouseUp(*pane, eventAt(*pane, point, point));
    REQUIRE(pane->cursorSeconds() > 1.0);

    pane->setClip(juce::File("/nonexistent/other.wav"), 4.0, 0.0f, "Audio 2", 0xff30ff80);
    REQUIRE(pane->cursorSeconds() == 0.0);
}

TEST_CASE("Alt-dragging over the samples redraws them, once zoomed in to see them", "[gui][audioeditor]")
{
    JuceFixture fixture;
    auto pane = makeReadyPane(800, 400, 60.0);

    int                channel = -1;
    long               first   = -1;
    std::vector<float> drawn;
    pane->onSamplesDrawn = [&](int c, long f, const std::vector<float>& values)
    {
        channel = c;
        first   = f;
        drawn   = values;
    };

    const auto alt = [&](juce::Point<float> position, juce::Point<float> down)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now    = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, juce::ModifierKeys(juce::ModifierKeys::altModifier), 1.0f,
                                0.0f, 0.0f, 0.0f, 0.0f, pane.get(), pane.get(), now, down, now, 1, false);
    };

    const auto from  = waveformPoint(*pane, 0.45f);
    const auto to    = waveformPoint(*pane, 0.55f);
    const auto top   = juce::Point<float>(to.x, from.y - 1000.0f); // far above: full scale

    SECTION("zoomed out, an Alt-drag doesn't draw")
    {
        sendMouseDown(*pane, alt(from, from));
        sendMouseDrag(*pane, alt(top, from));
        sendMouseUp(*pane, alt(top, from));
        REQUIRE(drawn.empty());
    }

    SECTION("zoomed in, it draws the line from where it started to where it ended")
    {
        auto* zoomIn = findButton(*pane, "+");
        REQUIRE(zoomIn != nullptr);
        for (int i = 0; i < 14; ++i)
            zoomIn->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50); // triggerClick is asynchronous

        // Two seconds of silence at 48kHz around the middle of the view.
        SampleDetail detail;
        detail.sampleRate   = 48000.0;
        detail.startSeconds = 30.0;
        detail.channels     = { std::vector<float>(96000, 0.0f) };
        pane->setSampleDetail(detail);
        REQUIRE(pane->canDrawSamples());

        sendMouseDown(*pane, alt(from, from));
        sendMouseDrag(*pane, alt(top, from));
        sendMouseUp(*pane, alt(top, from));

        REQUIRE(channel == 0);
        REQUIRE(first >= 30 * 48000);
        REQUIRE(drawn.size() >= 2);
        REQUIRE(std::abs(drawn.back() - 1.0f) < 1.0e-6f); // clamped to full scale
        for (size_t i = 1; i < drawn.size(); ++i)
            REQUIRE(drawn[i] >= drawn[i - 1]); // a rising line, not a comb

    }
}

TEST_CASE("The dB scale draws a quiet waveform taller", "[gui][audioeditor]")
{
    JuceFixture fixture;

    std::vector<float> channel((size_t) 48000 * 10);
    for (size_t i = 0; i < channel.size(); ++i)
        channel[i] = 0.01f * (float) std::sin(0.01 * (double) i); // -40 dB: a hair on the linear scale

    WaveformPeaks peaks;
    peaks.build({ channel });

    AudioEditorPane pane;
    pane.setVisible(true);
    pane.setSize(600, 300);
    pane.setClip(juce::File("/nonexistent/take.wav"), 10.0, 0.0f, "Audio 1", 0xff3080ff);
    pane.setWaveform(std::move(peaks), 48000.0);
    pane.resized();

    const auto linear = renderPane(pane);
    REQUIRE_FALSE(pane.showsDbScale());

    pane.setDbScale(true);
    REQUIRE(pane.showsDbScale());
    const auto decibels = renderPane(pane);

    INFO(differingPixels(linear, decibels) << " pixels changed");
    REQUIRE(differingPixels(linear, decibels) > 1000);
}

TEST_CASE("On the spectrogram a diagonal drag selects a box of frequencies too", "[gui][audioeditor]")
{
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 400, 10.0);

    // Over the waveform, a diagonal drag is only ever a time range.
    const auto centre = waveformPoint(*pane, 0.25f);
    const auto from   = centre.translated(0.0f, -30.0f);
    const auto to     = waveformPoint(*pane, 0.6f).translated(0.0f, 30.0f);

    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));
    REQUIRE_FALSE(pane->selection().isEmpty());
    REQUIRE_FALSE(pane->frequencyBand().has_value());

    // On the spectrogram the same drag also takes the frequencies it spans,
    // higher up the pane being higher in frequency.
    pane->setSpectrogramView(true);
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseDrag(*pane, eventAt(*pane, to, from));
    sendMouseUp(*pane, eventAt(*pane, to, from));

    const auto band = pane->frequencyBand();
    REQUIRE(band.has_value());
    REQUIRE(band->first < band->second);
    REQUIRE(band->first > 20.0);

    // A drag that stays level selects every frequency, as on the waveform.
    const auto level = waveformPoint(*pane, 0.7f);
    sendMouseDown(*pane, eventAt(*pane, centre, centre));
    sendMouseDrag(*pane, eventAt(*pane, level, centre));
    sendMouseUp(*pane, eventAt(*pane, level, centre));
    REQUIRE_FALSE(pane->selection().isEmpty());
    REQUIRE_FALSE(pane->frequencyBand().has_value());

    // And a click clears it.
    sendMouseDown(*pane, eventAt(*pane, centre, centre));
    sendMouseUp(*pane, eventAt(*pane, centre, centre));
    REQUIRE_FALSE(pane->frequencyBand().has_value());
}

TEST_CASE("Ctrl-drag on the spectrogram paints with the healing brush", "[gui][audioeditor]")
{
    JuceFixture fixture;
    auto        pane = makeReadyPane(800, 400, 10.0);
    pane->setSpectrogramView(true);

    const auto from = waveformPoint(*pane, 0.3f);
    const auto to   = waveformPoint(*pane, 0.4f).translated(0.0f, -20.0f);
    const auto ctrl = juce::ModifierKeys(juce::ModifierKeys::commandModifier);

    const auto withMods = [&](juce::Point<float> position)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now    = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, ctrl, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, pane.get(), pane.get(),
                                now, from, now, 1, false);
    };

    sendMouseDown(*pane, withMods(from));
    sendMouseDrag(*pane, withMods(to));
    sendMouseUp(*pane, withMods(to));

    const auto brush = pane->spectralBrush();
    REQUIRE(brush.has_value());
    REQUIRE(brush->dabs.size() > 2);          // a solid stroke, not just its ends
    REQUIRE_FALSE(pane->frequencyBand().has_value());

    // The selection spans the stroke's time, so the edit covers it.
    REQUIRE_FALSE(pane->selection().isEmpty());
    const auto [start, end] = brush->timeSpan();
    REQUIRE(pane->selection().startSeconds <= start + 1e-9);
    REQUIRE(pane->selection().endSeconds >= std::min(end, 10.0) - 1e-9);

    // A plain click clears the painting.
    sendMouseDown(*pane, eventAt(*pane, from, from));
    sendMouseUp(*pane, eventAt(*pane, from, from));
    REQUIRE_FALSE(pane->spectralBrush().has_value());
}
