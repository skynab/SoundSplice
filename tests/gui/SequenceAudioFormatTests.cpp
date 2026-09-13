#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <engine/SequenceAudioFormat.h>

using namespace soundsplice::engine;
namespace files = soundsplice::engine::sequencefile;

namespace
{
    struct TempFolder
    {
        juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SoundSpliceSequenceTests")
                              .getNonexistentChildFile("run", "", false);

        TempFolder() { root.createDirectory(); }
        ~TempFolder() { root.deleteRecursively(); }
    };

    /** @p frames of stereo audio where every sample is distinct: left i, right -i. */
    juce::AudioBuffer<float> ramp(int frames, float scale = 1.0e-4f)
    {
        juce::AudioBuffer<float> buffer(2, frames);
        for (int i = 0; i < frames; ++i)
        {
            buffer.setSample(0, i, (float) i * scale);
            buffer.setSample(1, i, -(float) i * scale);
        }
        return buffer;
    }

    /** A plain 32-bit float WAV holding @p buffer. */
    juce::File writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, double rate = 48000.0)
    {
        auto blocks = files::writeBlocks(file.getParentDirectory(), "tmp", buffer, rate, buffer.getNumSamples());
        REQUIRE(blocks);
        REQUIRE(blocks->size() == 1);
        REQUIRE(files::fileFromPath(blocks->front().file).moveFileTo(file));
        return file;
    }

    juce::AudioBuffer<float> readAll(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        files::registerFormats(formats);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        REQUIRE(reader != nullptr);

        juce::AudioBuffer<float> out((int) reader->numChannels, (int) reader->lengthInSamples);
        REQUIRE(reader->read(out.getArrayOfWritePointers(), out.getNumChannels(), 0, out.getNumSamples()));
        return out;
    }

    bool sameSamples(const juce::AudioBuffer<float>& a, int aFrom, const juce::AudioBuffer<float>& b, int bFrom,
                     int count)
    {
        for (int ch = 0; ch < juce::jmin(a.getNumChannels(), b.getNumChannels()); ++ch)
            for (int i = 0; i < count; ++i)
                if (a.getSample(ch, aFrom + i) != b.getSample(ch, bFrom + i))
                    return false;
        return true;
    }
}

TEST_CASE("A plain audio file is a sequence of one span", "[gui][sequence]")
{
    TempFolder temp;
    const auto wav = writeWav(temp.root.getChildFile("take.wav"), ramp(1000), 44100.0);

    const auto sequence = files::sequenceOf(wav);
    REQUIRE(sequence);
    REQUIRE(sequence->sampleRate == 44100.0);
    REQUIRE(sequence->numChannels == 2);
    REQUIRE(sequence->spans == std::vector<sequence::Span> { { files::pathOf(wav), 0, 1000 } });

    REQUIRE_FALSE(files::sequenceOf(temp.root.getChildFile("missing.wav")));
}

TEST_CASE("Audio written as blocks reads back bit for bit", "[gui][sequence]")
{
    TempFolder temp;
    const auto audio  = ramp(2500);
    const auto blocks = files::writeBlocks(temp.root, "edit", audio, 48000.0, 1000);

    REQUIRE(blocks);
    REQUIRE(blocks->size() == 3);
    REQUIRE((*blocks)[2].length == 500);

    sequence::SampleSequence sequence { 48000.0, 2, *blocks };
    const auto sequenceFile = temp.root.getChildFile("edit.sseq");
    REQUIRE(files::save(sequenceFile, sequence));

    const auto read = readAll(sequenceFile);
    REQUIRE(read.getNumSamples() == 2500);
    REQUIRE(sameSamples(read, 0, audio, 0, 2500));

    // A range across a block boundary, without reading the rest.
    juce::AudioBuffer<float> part;
    REQUIRE(files::readRange(sequenceFile, 990, 20, part));
    REQUIRE(sameSamples(part, 0, audio, 990, 20));
}

TEST_CASE("An edit writes only its own samples and leaves the original file alone", "[gui][sequence]")
{
    TempFolder temp;
    const auto original = ramp(1000);
    const auto wav      = writeWav(temp.root.getChildFile("take.wav"), original);
    const auto before   = wav.getLastModificationTime();
    const auto size     = wav.getSize();

    // Samples 100..200 replaced by 10 samples of 0.5.
    juce::AudioBuffer<float> patch(2, 10);
    for (int ch = 0; ch < 2; ++ch)
        std::fill_n(patch.getWritePointer(ch), 10, 0.5f);

    const auto blocks = files::writeBlocks(temp.root, "take", patch, 48000.0);
    REQUIRE(blocks);

    const auto edited = sequence::replaced(*files::sequenceOf(wav), 100, 200, *blocks);
    const auto file   = temp.root.getChildFile("take.sseq");
    REQUIRE(files::save(file, edited));

    const auto read = readAll(file);
    REQUIRE(read.getNumSamples() == 910);
    REQUIRE(sameSamples(read, 0, original, 0, 100));
    REQUIRE(read.getSample(0, 100) == 0.5f);
    REQUIRE(read.getSample(1, 109) == 0.5f);
    REQUIRE(sameSamples(read, 110, original, 200, 800));

    REQUIRE(wav.getSize() == size);
    REQUIRE(wav.getLastModificationTime() == before);
    REQUIRE(files::filesUsedBy(file).contains(wav));
}

TEST_CASE("A sequence file refers to blocks beside it relatively, so the folder can move", "[gui][sequence]")
{
    TempFolder temp;
    const auto here     = temp.root.getChildFile("here");
    const auto imported = writeWav(temp.root.getChildFile("library/import.wav"), ramp(100));
    const auto blocks   = files::writeBlocks(here, "edit", ramp(50), 48000.0);
    REQUIRE(blocks);

    auto spans = *blocks;
    spans.push_back({ files::pathOf(imported), 0, 100 });
    REQUIRE(files::save(here.getChildFile("edit.sseq"), { 48000.0, 2, spans }));

    const auto text = here.getChildFile("edit.sseq").loadFileAsString();
    REQUIRE(text.contains("span 0 50 edit block.wav"));
    REQUIRE(text.contains(imported.getFullPathName()));

    const auto there = temp.root.getChildFile("there");
    REQUIRE(here.moveFileTo(there));

    const auto moved = files::load(there.getChildFile("edit.sseq"));
    REQUIRE(moved);
    REQUIRE(files::fileFromPath(moved->spans[0].file) == there.getChildFile("edit block.wav"));
    REQUIRE(readAll(there.getChildFile("edit.sseq")).getNumSamples() == 150);
}

TEST_CASE("A missing block reads as silence and a mono block fills both channels", "[gui][sequence]")
{
    TempFolder temp;

    juce::AudioBuffer<float> mono(1, 10);
    std::fill_n(mono.getWritePointer(0), 10, 0.25f);
    const auto monoBlock = files::writeBlocks(temp.root, "mono", mono, 48000.0);
    REQUIRE(monoBlock);

    std::vector<sequence::Span> spans { { files::pathOf(temp.root.getChildFile("gone.wav")), 0, 10 } };
    spans.push_back(monoBlock->front());

    const auto file = temp.root.getChildFile("patchy.sseq");
    REQUIRE(files::save(file, { 48000.0, 2, spans }));

    const auto read = readAll(file);
    REQUIRE(read.getNumSamples() == 20);
    REQUIRE(read.getSample(0, 5) == 0.0f);
    REQUIRE(read.getSample(0, 15) == 0.25f);
    REQUIRE(read.getSample(1, 15) == 0.25f);
}

TEST_CASE("A file that only looks like a sequence isn't read as one", "[gui][sequence]")
{
    TempFolder temp;
    const auto file = temp.root.getChildFile("bogus.sseq");
    REQUIRE(file.replaceWithText("not a sequence"));

    juce::AudioFormatManager formats;
    files::registerFormats(formats);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader == nullptr);
}
