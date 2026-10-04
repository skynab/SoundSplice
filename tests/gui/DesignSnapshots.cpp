#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ArrangementView.h>
#include <app/AudioEditorPane.h>
#include <app/DockRegion.h>
#include <app/Theme.h>
#include <app/TrackColours.h>

using namespace soundsplice;

/*
    Renders the panes the design work touches to PNG files, so a change to how
    they're drawn can be looked at without driving the app by hand.

    Hidden ([.snapshot]): it only runs when asked for by name, and needs two
    environment variables - SOUNDSPLICE_SNAPSHOTS, an existing folder to write
    into, and SOUNDSPLICE_SNAPSHOT_AUDIO, a WAV file to draw. For example:

        set SOUNDSPLICE_SNAPSHOTS=C:\temp\shots
        set SOUNDSPLICE_SNAPSHOT_AUDIO=C:\path\to\take.wav
        soundsplice_gui_tests "[.snapshot]"
*/

namespace
{
    juce::MouseEvent eventAt(juce::Component& target, juce::Point<float> position, juce::Point<float> down)
    {
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto now    = juce::Time::getCurrentTime();
        return juce::MouseEvent(source, position, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target,
                                &target, now, down, now, 1, false);
    }

    void pump(int milliseconds) { juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds); }

    void save(juce::Component& component, const juce::File& file)
    {
        const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 1.0f);
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(out.openedOk());
        REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
    }

    /** A region on the workspace colour, as the app floats a pane. */
    struct Floating final : public juce::Component
    {
        DockRegion region;
        Floating() { addAndMakeVisible(region); }
        void paint(juce::Graphics& g) override { g.fillAll(theme::colour(*this, theme::workspaceId)); }
        void resized() override { region.setBounds(getLocalBounds().reduced(6)); }
    };
}

TEST_CASE("Design snapshots", "[.snapshot]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    AppLookAndFeel                  look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);

    const juce::File folder(juce::SystemStats::getEnvironmentVariable("SOUNDSPLICE_SNAPSHOTS", {}));
    const juce::File audio(juce::SystemStats::getEnvironmentVariable("SOUNDSPLICE_SNAPSHOT_AUDIO", {}));
    REQUIRE(folder.isDirectory());
    REQUIRE(audio.existsAsFile());

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audio));
    REQUIRE(reader != nullptr);

    juce::AudioBuffer<float> buffer((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
    std::vector<std::vector<float>> channels;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        channels.emplace_back(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + buffer.getNumSamples());
    WaveformPeaks peaks;
    peaks.build(channels);
    const double seconds = (double) buffer.getNumSamples() / reader->sampleRate;

    SECTION("The waveform editor, with a selection")
    {
        AudioEditorPane pane;
        Floating        frame;
        frame.setSize(1100, 760);
        frame.region.addPanel("Audio", pane);
        frame.setVisible(true);
        frame.resized();

        pane.setClip(audio, seconds, 0.0f, "Guest - Marisa", 0xff57cbd7);
        pane.setWaveform(peaks, reader->sampleRate);
        pane.setSelectionActions({ { "Silence", glyphs::Glyph::speakerSlash, {}, {} },
                                   { "Fade in", glyphs::Glyph::trendUp, {}, {} },
                                   { "Fade out", glyphs::Glyph::trendDown, {}, {} },
                                   { "Normalize", glyphs::Glyph::normalize, {}, {} },
                                   { "Denoise", glyphs::Glyph::waves, {}, {} },
                                   { "Delete", glyphs::Glyph::trash, {}, {} } });
        pane.resized();

        // A drag across the middle of the waveform, as a hand would select.
        const auto  area = pane.getLocalBounds().toFloat();
        const float y    = area.getCentreY();
        const juce::Point<float> from(area.getWidth() * 0.42f, y), to(area.getWidth() * 0.55f, y);
        static_cast<juce::Component&>(pane).mouseDown(eventAt(pane, from, from));
        static_cast<juce::Component&>(pane).mouseDrag(eventAt(pane, to, from));
        static_cast<juce::Component&>(pane).mouseUp(eventAt(pane, to, from));
        pump(100);

        save(frame, folder.getChildFile("editor.png"));
    }

    SECTION("The timeline, with a track in each colour")
    {
        model::Song song;
        const char* names[] = { "Host - Dana", "Guest - Marisa", "Room tone", "SFX", "Music" };
        for (int i = 0; i < 5; ++i)
        {
            auto& track  = model::addTrack(song, model::TrackType::Audio, names[i]);
            track.colour = kTrackColours[1 + i].argb;
            for (int c = 0; c < 2; ++c)
            {
                model::Clip clip;
                clip.audioFile   = audio.getFullPathName().toStdString();
                clip.startBeats  = 2.0 + c * 18.0 + i * 3.0;
                clip.lengthBeats = 12.0;
                model::addClip(song, track.id, clip);
            }
        }

        ArrangementView view;
        Floating        frame;
        frame.setSize(1200, 560);
        frame.region.addPanel("Tracks", view);
        frame.setVisible(true);
        frame.resized();
        view.setSong(song);
        pump(1500); // the thumbnails scan their files in the background
        view.repaint();

        save(frame, folder.getChildFile("timeline.png"));
    }

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}
