#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <engine/AudioExport.h>
#include <engine/AudioFormats.h>

#include <cmath>
#include <cstring>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    juce::AudioBuffer<float> tone()
    {
        juce::AudioBuffer<float> audio(2, 44100);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                audio.setSample(ch, i, 0.3f * (float) std::sin(i * 0.05));
        return audio;
    }

    engine::ExportTags someTags(const juce::File& cover)
    {
        engine::ExportTags tags;
        tags.title   = juce::String::fromUTF8("Episode 12: Caf\xc3\xa9");
        tags.artist  = "The Host";
        tags.album   = "The Show";
        tags.year    = "2026";
        tags.comment = "Recorded live";
        tags.track   = "12";
        tags.coverArt = cover;
        tags.chapters = { { 0.0, 0.4, "Intro" }, { 0.4, 1.0, "Interview" } };
        return tags;
    }

    /** A tiny PNG, as a cover. */
    juce::File writeCover(const juce::TemporaryFile& temp)
    {
        juce::Image image(juce::Image::RGB, 8, 8, true);
        juce::Graphics(image).fillAll(juce::Colours::orange);
        juce::FileOutputStream out(temp.getFile());
        juce::PNGImageFormat().writeImageToStream(image, out);
        out.flush();
        return temp.getFile();
    }

    bool contains(const juce::MemoryBlock& data, const juce::String& text)
    {
        const auto* needle = text.toRawUTF8();
        const auto  length = text.getNumBytesAsUTF8();
        const auto* bytes  = static_cast<const char*>(data.getData());
        for (size_t i = 0; i + length <= data.getSize(); ++i)
            if (std::memcmp(bytes + i, needle, length) == 0)
                return true;
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        engine::audioformats::registerAll(formats);
        return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
    }
}

TEST_CASE("An MP3 export carries an ID3v2.4 tag with cover and chapters", "[gui][tags]")
{
    JuceFixture fixture;
    const juce::TemporaryFile coverFile(".png"), out(".mp3");

    engine::ExportOptions options;
    options.format     = engine::ExportFormat::Mp3;
    options.sampleRate = 44100.0;
    options.tags       = someTags(writeCover(coverFile));
    REQUIRE(engine::writeAudioFile(out.getFile(), tone(), options));

    juce::MemoryBlock data;
    REQUIRE(out.getFile().loadFileAsData(data));
    const auto* bytes = static_cast<const uint8_t*>(data.getData());
    REQUIRE(std::memcmp(bytes, "ID3\x04", 4) == 0);

    // Its size is syncsafe, and the audio starts straight after it.
    const size_t size = ((size_t) bytes[6] << 21) | ((size_t) bytes[7] << 14) | ((size_t) bytes[8] << 7) | bytes[9];
    REQUIRE(10 + size < data.getSize());
    REQUIRE(bytes[10 + size] == 0xff); // an MPEG frame sync

    for (const auto* id : { "TIT2", "TPE1", "TALB", "TDRC", "TRCK", "COMM", "APIC", "CHAP", "CTOC" })
        REQUIRE(contains(data, id));
    REQUIRE(contains(data, juce::String::fromUTF8("Episode 12: Caf\xc3\xa9")));
    REQUIRE(contains(data, "Interview"));

    // And it still plays.
    auto decoded = reader(out.getFile());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->lengthInSamples > 40000);
}

TEST_CASE("A FLAC export carries Vorbis comments, chapters and a picture", "[gui][tags]")
{
    JuceFixture fixture;
    const juce::TemporaryFile coverFile(".png"), out(".flac");

    engine::ExportOptions options;
    options.format        = engine::ExportFormat::Flac;
    options.sampleRate    = 44100.0;
    options.bitsPerSample = 16;
    options.tags          = someTags(writeCover(coverFile));
    REQUIRE(engine::writeAudioFile(out.getFile(), tone(), options));

    juce::MemoryBlock data;
    REQUIRE(out.getFile().loadFileAsData(data));
    REQUIRE(contains(data, "TITLE=Episode 12"));
    REQUIRE(contains(data, "ARTIST=The Host"));
    REQUIRE(contains(data, "CHAPTER002=00:00:00.400"));
    REQUIRE(contains(data, "CHAPTER002NAME=Interview"));
    REQUIRE(contains(data, "image/png"));

    // The blocks still chain properly: the decoder reads every sample.
    auto decoded = reader(out.getFile());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->lengthInSamples == 44100);
    juce::AudioBuffer<float> back(2, 44100);
    REQUIRE(decoded->read(&back, 0, 44100, 0, true, true));
    REQUIRE(std::abs(back.getSample(0, 1000) - 0.3f * (float) std::sin(1000 * 0.05)) < 0.001f);

    // Written again (a re-export over it), there's still one comment block.
    juce::MemoryBlock again(data);
    REQUIRE(engine::tags::rewriteFlac(again, options.tags));
    REQUIRE(again.getSize() == data.getSize());
}

TEST_CASE("A WAV export carries INFO and BWF tags", "[gui][tags]")
{
    JuceFixture fixture;
    const juce::TemporaryFile out(".wav");

    engine::ExportOptions options;
    options.format = engine::ExportFormat::Wav;
    options.tags   = someTags({});
    options.tags.chapters.clear();
    REQUIRE(engine::writeAudioFile(out.getFile(), tone(), options));

    auto decoded = reader(out.getFile());
    REQUIRE(decoded != nullptr);
    const auto& meta = decoded->metadataValues;
    REQUIRE(meta[juce::WavAudioFormat::riffInfoArtist] == "The Host");
    REQUIRE(meta[juce::WavAudioFormat::riffInfoProductName] == "The Show");
    REQUIRE(meta[juce::WavAudioFormat::bwavOriginator] == "The Host");
    REQUIRE(meta[juce::WavAudioFormat::aswgArtist] == "The Host"); // iXML
}

TEST_CASE("No tags writes the files as before", "[gui][tags]")
{
    JuceFixture fixture;
    const juce::TemporaryFile out(".mp3");
    engine::ExportOptions options;
    options.format     = engine::ExportFormat::Mp3;
    options.sampleRate = 44100.0;
    REQUIRE(engine::writeAudioFile(out.getFile(), tone(), options));
    juce::MemoryBlock data;
    REQUIRE(out.getFile().loadFileAsData(data));
    REQUIRE(std::memcmp(data.getData(), "ID3", 3) != 0);
}

#include <app/ProjectInfoDialog.h>

TEST_CASE("Project Info edits a copy and hands it back", "[gui][tags]")
{
    JuceFixture fixture;
    model::ProjectInfo info;
    info.title    = "Old";
    info.coverArt = "cover.png";

    ProjectInfoDialog dialog(info);
    REQUIRE(dialog.editorForTesting("title")->getText() == "Old");
    REQUIRE(dialog.editorForTesting("cover") == nullptr); // chosen, not typed

    dialog.editorForTesting("title")->setText("  New title  ");
    dialog.editorForTesting("artist")->setText("Someone");
    model::ProjectInfo saved;
    dialog.onSave = [&](const model::ProjectInfo& i) { saved = i; };
    dialog.onSave(dialog.read());
    REQUIRE(saved.title == "New title");
    REQUIRE(saved.artist == "Someone");
    REQUIRE(saved.coverArt == "cover.png");
}

TEST_CASE("WavPack exports are lossless and tagged", "[gui][tags]")
{
    JuceFixture fixture;
    const auto source = tone();

    for (const int bits : { 24, 32 })
    {
        INFO(bits);
        const juce::TemporaryFile out(".wv");
        engine::ExportOptions options;
        options.format        = engine::ExportFormat::WavPack;
        options.sampleRate    = 44100.0;
        options.bitsPerSample = bits;
        options.dither        = false;
        options.tags          = someTags({});
        REQUIRE(engine::writeAudioFile(out.getFile(), source, options));

        auto decoded = reader(out.getFile());
        REQUIRE(decoded != nullptr);
        REQUIRE(decoded->lengthInSamples == source.getNumSamples());
        juce::AudioBuffer<float> back(2, source.getNumSamples());
        REQUIRE(decoded->read(&back, 0, back.getNumSamples(), 0, true, true));
        const float step = bits == 32 ? 0.0f : 1.0f / 8388608.0f;
        for (int i = 0; i < source.getNumSamples(); i += 37)
            REQUIRE(std::abs(back.getSample(0, i) - source.getSample(0, i)) <= step);

        juce::MemoryBlock data;
        REQUIRE(out.getFile().loadFileAsData(data));
        REQUIRE(contains(data, "APETAGEX"));
        REQUIRE(contains(data, "The Host"));
    }
}

TEST_CASE("Opus exports run at 48 kHz, keep their length, and carry comments", "[gui][tags]")
{
    JuceFixture fixture;
    const juce::TemporaryFile coverFile(".png"), out(".opus");

    engine::ExportOptions options;
    options.format       = engine::ExportFormat::Opus;
    options.sampleRate   = 44100.0; // resampled
    options.qualityIndex = 2;
    options.tags         = someTags(writeCover(coverFile));
    REQUIRE(engine::writeAudioFile(out.getFile(), tone(), options));

    auto decoded = reader(out.getFile());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->sampleRate == 48000.0);
    REQUIRE(std::abs((double) decoded->lengthInSamples - 48000.0) <= 2.0); // one second, as it went in

    juce::MemoryBlock data;
    REQUIRE(out.getFile().loadFileAsData(data));
    REQUIRE(contains(data, "OpusTags"));
    REQUIRE(contains(data, "ARTIST=The Host"));
    REQUIRE(contains(data, "METADATA_BLOCK_PICTURE="));
}
