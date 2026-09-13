#include <catch2/catch_test_macros.hpp>

#include <engine/AudioExport.h>

#include <cmath>

using namespace looper::engine;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr double kToneHz     = 1000.0;
constexpr int    kNumSamples = 24000; // half a second

/** A stereo test signal: a tone on the left, the same tone inverted on the
    right, so a format that collapses or swaps channels is caught rather than
    passing on a mono-identical pair. */
juce::AudioBuffer<float> makeTestBuffer()
{
    juce::AudioBuffer<float> buffer (2, kNumSamples);
    for (int n = 0; n < kNumSamples; ++n)
    {
        const auto v = (float) (0.5 * std::sin (2.0 * kPi * kToneHz * n / kSampleRate));
        buffer.setSample (0, n, v);
        buffer.setSample (1, n, -v);
    }
    return buffer;
}

juce::File scratchFile (ExportFormat format)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getNonexistentChildFile ("looper-export-test",
                                         "." + extensionFor (format));
}

/** Magnitude at one frequency, by direct correlation - the same technique the
    engine tests use. */
double magnitudeAt (const juce::AudioBuffer<float>& buffer, int channel, double hz, double rate)
{
    double re = 0.0, im = 0.0;
    const int n = buffer.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        const double phase = 2.0 * kPi * hz * (double) i / rate;
        re += buffer.getSample (channel, i) * std::cos (phase);
        im -= buffer.getSample (channel, i) * std::sin (phase);
    }
    return std::sqrt (re * re + im * im) / (double) n;
}

ExportOptions optionsFor (ExportFormat format, int bits = 24)
{
    ExportOptions options;
    options.format        = format;
    options.sampleRate    = kSampleRate;
    options.bitsPerSample = bits;
    return options;
}

juce::String nameOf (ExportFormat format) { return displayNameFor (format); }
}

TEST_CASE ("Every export format writes a file a decoder can read back", "[engine][export]")
{
    // The claim the feature rests on. Written, then read back through
    // AudioFormatManager - which registers the platform's own codecs, so an
    // MP3 is decoded by CoreAudio here rather than by anything of ours.
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    const auto source = makeTestBuffer();

    for (auto format : allExportFormats())
    {
        INFO ("format: " << nameOf (format));

        const auto file = scratchFile (format);
        REQUIRE (writeAudioFile (file, source, optionsFor (format)));
        REQUIRE (file.existsAsFile());
        REQUIRE (file.getSize() > 0);

        std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));

        if (reader == nullptr)
        {
            // No decoder for this format on this platform. Rather than skip -
            // which would quietly stop testing MP3 the day CoreAudio changed -
            // assert the file at least begins with a valid MPEG frame sync,
            // so a truncated or empty write still fails here.
            REQUIRE (format == ExportFormat::Mp3);

            juce::MemoryBlock raw;
            REQUIRE (file.loadFileAsData (raw));
            REQUIRE (raw.getSize() > 4);

            const auto* bytes = static_cast<const juce::uint8*> (raw.getData());
            REQUIRE (bytes[0] == 0xFF);
            REQUIRE ((bytes[1] & 0xE0) == 0xE0);

            file.deleteFile();
            continue;
        }

        CHECK (reader->sampleRate == kSampleRate);
        CHECK (reader->numChannels == 2);

        // A lossy encoder pads: MP3 adds encoder delay and pads the final
        // frame, so the length is "at least what went in", not "exactly".
        CHECK (reader->lengthInSamples >= kNumSamples - 1);

        juce::AudioBuffer<float> decoded ((int) reader->numChannels, kNumSamples);
        REQUIRE (reader->read (&decoded, 0, kNumSamples, 0, true, true));

        file.deleteFile();
    }
}

TEST_CASE ("A lossless export comes back sample-accurate", "[engine][export]")
{
    // WAV, AIFF and FLAC must reproduce the mix within the quantisation of the
    // depth they were asked for - that is the whole reason to choose one.
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    const auto source = makeTestBuffer();

    for (auto format : { ExportFormat::Wav, ExportFormat::Aiff, ExportFormat::Flac })
    {
        for (int bits : possibleBitDepths (format))
        {
            INFO ("format: " << nameOf (format) << " at " << bits << " bits");

            const auto file = scratchFile (format);
            REQUIRE (writeAudioFile (file, source, optionsFor (format, bits)));

            std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
            REQUIRE (reader != nullptr);
            REQUIRE (reader->lengthInSamples == kNumSamples);

            juce::AudioBuffer<float> decoded (2, kNumSamples);
            REQUIRE (reader->read (&decoded, 0, kNumSamples, 0, true, true));

            // One LSB at the written depth, with a little room for the
            // encoder's rounding. 32-bit is float, so it is exact.
            const float tolerance = bits >= 32 ? 1.0e-6f : 2.0f / (float) (1 << (bits - 1));

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < kNumSamples; ++n)
                    worst = juce::jmax (worst, std::abs (decoded.getSample (ch, n)
                                                         - source.getSample (ch, n)));

            INFO ("worst sample error " << worst << " vs tolerance " << tolerance);
            CHECK (worst <= tolerance);

            file.deleteFile();
        }
    }
}

TEST_CASE ("A lossy export preserves the signal it was given", "[engine][export]")
{
    // Not sample equality - that is not what a lossy codec promises - but the
    // tone has to survive and the channels have to stay the way round they
    // were. A codec wired up wrongly still produces a playable file, so
    // "it decoded" is not enough on its own.
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    const auto source = makeTestBuffer();

    for (auto format : { ExportFormat::OggVorbis, ExportFormat::Mp3 })
    {
        const auto file = scratchFile (format);
        REQUIRE (writeAudioFile (file, source, optionsFor (format)));

        std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
        if (reader == nullptr)
        {
            file.deleteFile();
            continue; // covered by the frame-sync check in the round-trip test
        }

        INFO ("format: " << nameOf (format));

        juce::AudioBuffer<float> decoded ((int) reader->numChannels, kNumSamples);
        REQUIRE (reader->read (&decoded, 0, kNumSamples, 0, true, true));

        // Measured away from the edges, where a lossy codec's encoder delay
        // and padding live.
        juce::AudioBuffer<float> middle (2, kNumSamples / 2);
        for (int ch = 0; ch < 2; ++ch)
            middle.copyFrom (ch, 0, decoded, ch, kNumSamples / 4, kNumSamples / 2);

        const double atTone   = magnitudeAt (middle, 0, kToneHz, kSampleRate);
        const double atOther  = magnitudeAt (middle, 0, 3000.0, kSampleRate);

        INFO ("1kHz " << atTone << "  3kHz " << atOther);

        // The tone is there (0.5 amplitude reads about 0.25) and nothing much
        // else is.
        CHECK (atTone > 0.15);
        CHECK (atOther < atTone * 0.1);

        file.deleteFile();
    }
}

TEST_CASE ("Every bit depth the dialog can offer actually produces a file", "[engine][export]")
{
    // This is the specific way export fails silently: an unsupported
    // combination returns a null writer at the *end* of a long render, after
    // the work is done. The dialog is built from possibleBitDepths, so every
    // value it can show has to be one that writes.
    const auto source = makeTestBuffer();

    for (auto format : allExportFormats())
    {
        const auto depths = possibleBitDepths (format);
        CHECK (depths.isEmpty() == ! usesBitDepth (format));

        for (int bits : depths)
        {
            INFO ("format: " << nameOf (format) << " at " << bits << " bits");
            const auto file = scratchFile (format);
            CHECK (writeAudioFile (file, source, optionsFor (format, bits)));
            file.deleteFile();
        }
    }
}

TEST_CASE ("Every quality option the dialog can offer actually produces a file", "[engine][export]")
{
    const auto source = makeTestBuffer();

    for (auto format : allExportFormats())
    {
        const auto qualities = qualityOptionsFor (format);
        CHECK (qualities.isEmpty() == usesBitDepth (format));

        for (int i = 0; i < qualities.size(); ++i)
        {
            INFO ("format: " << nameOf (format) << " quality " << qualities[i]);

            auto options = optionsFor (format);
            options.qualityIndex = i;

            const auto file = scratchFile (format);
            CHECK (writeAudioFile (file, source, options));
            file.deleteFile();
        }
    }
}

TEST_CASE ("Every offered sample rate exports", "[engine][export]")
{
    const auto source = makeTestBuffer();

    for (auto format : allExportFormats())
    {
        for (int rate : possibleSampleRates (format))
        {
            INFO ("format: " << nameOf (format) << " at " << rate << "Hz");

            auto options = optionsFor (format);
            options.sampleRate = (double) rate;

            const auto file = scratchFile (format);
            CHECK (writeAudioFile (file, source, options));
            file.deleteFile();
        }
    }
}

TEST_CASE ("A failed export leaves no file behind", "[engine][export]")
{
    // A zero-byte .wav where the user asked for one looks like a successful
    // export until they try to play it.
    const auto file = scratchFile (ExportFormat::Wav);

    juce::AudioBuffer<float> empty (2, 0);
    CHECK_FALSE (writeAudioFile (file, empty, optionsFor (ExportFormat::Wav)));
    CHECK_FALSE (file.existsAsFile());
}

TEST_CASE ("Export formats have distinct extensions and names", "[engine][export]")
{
    juce::StringArray extensions, names;
    for (auto format : allExportFormats())
    {
        extensions.add (extensionFor (format));
        names.add (displayNameFor (format));
    }

    CHECK (extensions.size() == kNumExportFormats);
    extensions.removeDuplicates (true);
    names.removeDuplicates (true);

    CHECK (extensions.size() == kNumExportFormats);
    CHECK (names.size() == kNumExportFormats);
}

// ---------------------------------------------------------------------------
// The dialog. Included here rather than in its own file because what is being
// checked is precisely that it agrees with the format table above.

#include <app/ExportAudioDialog.h>

TEST_CASE ("The export dialog offers no combination that fails to write", "[gui][export]")
{
    // The dialog builds its rate/depth/quality lists from the AudioExport
    // queries. This drives it the way a user would - pick each format in turn -
    // and exports what it produces, so the two can't drift apart into a UI that
    // offers something the writer then refuses at the end of a render.
    juce::AlertWindow window ("Export Audio", {}, juce::MessageBoxIconType::NoIcon);
    looper::app::ExportAudioDialog::buildControls (window, 48000.0);

    const auto source = makeTestBuffer();

    for (int formatIndex = 0; formatIndex < kNumExportFormats; ++formatIndex)
    {
        const auto format = allExportFormats()[(size_t) formatIndex];
        INFO ("format: " << nameOf (format));

        window.getComboBoxComponent ("format")->setSelectedItemIndex (formatIndex,
                                                                      juce::dontSendNotification);
        looper::app::ExportAudioDialog::refreshDependentBoxes (window, 48000.0);

        auto* rateBox    = window.getComboBoxComponent ("rate");
        auto* bitsBox    = window.getComboBoxComponent ("bits");
        auto* qualityBox = window.getComboBoxComponent ("quality");

        // A box with nothing to choose is disabled rather than empty, so it
        // reads as "not applicable" instead of "broken".
        CHECK (bitsBox->isEnabled() == usesBitDepth (format));
        CHECK (qualityBox->isEnabled() == ! usesBitDepth (format));
        CHECK (rateBox->isEnabled());

        // Every selectable value in every box, not just the default: the
        // default being safe is the easy half.
        for (int r = 0; r < rateBox->getNumItems(); ++r)
        {
            rateBox->setSelectedItemIndex (r, juce::dontSendNotification);

            for (int b = 0; b < bitsBox->getNumItems(); ++b)
            {
                bitsBox->setSelectedItemIndex (b, juce::dontSendNotification);

                for (int q = 0; q < qualityBox->getNumItems(); ++q)
                {
                    qualityBox->setSelectedItemIndex (q, juce::dontSendNotification);

                    const auto options = looper::app::ExportAudioDialog::readOptions (window);
                    REQUIRE (options.format == format);

                    const auto file = scratchFile (format);
                    INFO ("rate " << options.sampleRate << " bits " << options.bitsPerSample
                                  << " quality " << options.qualityIndex);
                    CHECK (writeAudioFile (file, source, options));
                    file.deleteFile();
                }
            }
        }
    }
}

TEST_CASE ("The export dialog defaults to the device sample rate", "[gui][export]")
{
    // The common export is "what I am hearing, as a file". Defaulting to
    // anything else would resample it by default.
    juce::AlertWindow window ("Export Audio", {}, juce::MessageBoxIconType::NoIcon);
    looper::app::ExportAudioDialog::buildControls (window, 48000.0);

    for (double deviceRate : { 44100.0, 48000.0, 96000.0 })
    {
        looper::app::ExportAudioDialog::refreshDependentBoxes (window, deviceRate);
        const auto options = looper::app::ExportAudioDialog::readOptions (window);

        INFO ("device at " << deviceRate);
        CHECK (options.sampleRate == deviceRate);

        // And 24-bit, which is what this app produced before the depth was a
        // choice at all.
        CHECK (options.bitsPerSample == 24);
    }
}


TEST_CASE ("Dither reaches the exported file", "[engine][export][dither]")
{
    // The end-to-end version of DitherTests' central claim, through the real
    // export path — so this fails if the dither is built but never wired in,
    // which the unit tests cannot see.
    //
    // A tone below one 16-bit quantisation step. Measured, JUCE's writer
    // truncates toward negative infinity rather than rounding, so undithered
    // this sine comes back as a *square wave* alternating between 0 and -1
    // LSB: the fundamental arrives about 59% too loud (4/pi times half a step)
    // and drags a full harmonic series in with it. Truncation does not merely
    // lose quiet material, it replaces it with something that was never
    // played, which is the case for dither in one measurement.
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    constexpr int    bits      = 16;
    const float      lsb       = 1.0f / 32768.0f;
    const float      amplitude = lsb * 0.4f;
    constexpr int    samples   = 1 << 16;
    constexpr double tone      = 48000.0 * 512.0 / (double) samples;

    juce::AudioBuffer<float> quiet (2, samples);
    for (int n = 0; n < samples; ++n)
    {
        const auto v = (float) (amplitude * std::sin (2.0 * kPi * tone * n / kSampleRate));
        quiet.setSample (0, n, v);
        quiet.setSample (1, n, v);
    }

    struct Result { double fundamental, thirdHarmonic; };

    auto exportAndMeasure = [&] (bool dither)
    {
        auto options = optionsFor (ExportFormat::Wav, bits);
        options.dither = dither;

        const auto file = scratchFile (ExportFormat::Wav);
        REQUIRE (writeAudioFile (file, quiet, options));

        std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
        REQUIRE (reader != nullptr);

        juce::AudioBuffer<float> decoded (2, samples);
        REQUIRE (reader->read (&decoded, 0, samples, 0, true, true));
        file.deleteFile();

        return Result { magnitudeAt (decoded, 0, tone, kSampleRate),
                        magnitudeAt (decoded, 0, tone * 3.0, kSampleRate) };
    };

    const auto   plain    = exportAndMeasure (false);
    const auto   dithered = exportAndMeasure (true);
    const double expected = amplitude * 0.5;

    INFO ("expected " << expected
          << "  undithered " << plain.fundamental << " (3rd " << plain.thirdHarmonic << ")"
          << "  dithered " << dithered.fundamental << " (3rd " << dithered.thirdHarmonic << ")");

    // Dithered, the level that comes back is the level that went in.
    CHECK (dithered.fundamental > expected * 0.9);
    CHECK (dithered.fundamental < expected * 1.1);

    // Undithered, it is not — the square wave's fundamental is well above it.
    CHECK (plain.fundamental > expected * 1.3);

    // And the harmonic truncation invented is gone, which is the part that
    // actually sounds like distortion rather than like a level error.
    CHECK (dithered.thirdHarmonic < plain.thirdHarmonic * 0.2);
}

TEST_CASE ("Dither is not applied where nothing is quantised", "[engine][export][dither]")
{
    // 32-bit float writes the samples as they are. Adding noise there would be
    // damage with no benefit — there is no quantisation error to decorrelate.
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    const auto source = makeTestBuffer();

    auto options = optionsFor (ExportFormat::Wav, 32);
    options.dither = true;

    const auto file = scratchFile (ExportFormat::Wav);
    REQUIRE (writeAudioFile (file, source, options));

    std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
    REQUIRE (reader != nullptr);

    juce::AudioBuffer<float> decoded (2, kNumSamples);
    REQUIRE (reader->read (&decoded, 0, kNumSamples, 0, true, true));
    file.deleteFile();

    // Bit-identical, dither flag or not.
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < kNumSamples; ++n)
            REQUIRE (decoded.getSample (ch, n) == source.getSample (ch, n));
}

TEST_CASE ("Dithered exports are reproducible", "[engine][export][dither]")
{
    // Two exports of the same mix produce the same bytes. Dither seeded from a
    // clock would make every export different, which quietly breaks any
    // workflow that compares or caches renders.
    const auto source = makeTestBuffer();
    const auto options = [] { auto o = optionsFor (ExportFormat::Wav, 16); o.dither = true; return o; }();

    const auto first  = scratchFile (ExportFormat::Wav);
    const auto second = scratchFile (ExportFormat::Wav);

    REQUIRE (writeAudioFile (first, source, options));
    REQUIRE (writeAudioFile (second, source, options));

    juce::MemoryBlock a, b;
    REQUIRE (first.loadFileAsData (a));
    REQUIRE (second.loadFileAsData (b));

    CHECK (a == b);

    first.deleteFile();
    second.deleteFile();
}

TEST_CASE ("The export dialog's Contents option round-trips", "[gui][export][stems]")
{
    // The dialog is the only way a user selects stems, so what readOptions
    // hands back has to be what was picked — and the three modes have to be
    // distinguishable, since two of them writing the same thing would be a
    // silent no-op rather than a visible bug.
    juce::AlertWindow window ("Export Audio", {}, juce::MessageBoxIconType::NoIcon);
    looper::app::ExportAudioDialog::buildControls (window, 48000.0);

    auto* contentsBox = window.getComboBoxComponent ("contents");
    REQUIRE (contentsBox != nullptr);
    REQUIRE (contentsBox->getNumItems() == kNumExportContents);

    for (int i = 0; i < kNumExportContents; ++i)
    {
        contentsBox->setSelectedItemIndex (i, juce::dontSendNotification);
        const auto options = looper::app::ExportAudioDialog::readOptions (window);

        INFO ("contents index " << i << ": " << displayNameFor (options.contents));
        CHECK ((int) options.contents == i);
    }

    // The default is the plain master mix — adding stems is opting in, not
    // something an ordinary export starts doing by surprise.
    contentsBox->setSelectedItemIndex (0, juce::dontSendNotification);
    CHECK (looper::app::ExportAudioDialog::readOptions (window).contents
           == ExportContents::MasterMix);
}

TEST_CASE ("Export contents modes say what they write", "[engine][export][stems]")
{
    // writesMasterMix/writesStems are what exportProject branches on, so
    // getting one of them backwards would either skip the mix or skip the
    // stems with nothing to show for it.
    CHECK (writesMasterMix (ExportContents::MasterMix));
    CHECK_FALSE (writesStems (ExportContents::MasterMix));

    CHECK (writesMasterMix (ExportContents::MasterMixAndStems));
    CHECK (writesStems (ExportContents::MasterMixAndStems));

    CHECK_FALSE (writesMasterMix (ExportContents::StemsOnly));
    CHECK (writesStems (ExportContents::StemsOnly));

    // Every mode writes something. A mode that wrote nothing would look like a
    // failed export with no error.
    for (int i = 0; i < kNumExportContents; ++i)
    {
        const auto contents = (ExportContents) i;
        INFO (displayNameFor (contents));
        CHECK ((writesMasterMix (contents) || writesStems (contents)));
    }
}
