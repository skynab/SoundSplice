#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/AudioFormats.h"
#include "engine/SampleSequence.h"

namespace soundsplice::engine::sequencefile
{
/**
    Sample sequences (engine/SampleSequence.h) on disk: a small ".sseq" text
    file listing its spans, and the blocks of audio written for it.

    The format is registered with a juce::AudioFormatManager like any other
    (registerFormats below), so everything that plays, draws, analyses or
    exports audio reads a sequence as one continuous file without knowing it's
    made of pieces. Only the edit path has to know, because it's the one
    writing them.

    A span's file is stored relative to the sequence file when it lies in the
    same folder, and in full otherwise, so a project's audio folder can be
    moved as a whole while a sequence can still point into an imported file
    that lives somewhere else.
*/

inline constexpr const char* kExtension = ".sseq";

/** The most frames written to one block file: about 22 seconds at 48 kHz, so
    an edit rewrites at most that much either side of what it changed — and
    usually far less, because only its own samples are written at all. */
inline constexpr int kBlockFrames = 1 << 20;

inline bool isSequenceFile(const juce::File& file)
{
    return file.hasFileExtension(kExtension);
}

inline juce::File fileFromPath(const std::string& path)
{
    return juce::File(juce::String::fromUTF8(path.c_str()));
}

inline std::string pathOf(const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

/** @p sequence's span paths as they are written into @p sequenceFile. */
inline sequence::SampleSequence withStoredSpanPaths(sequence::SampleSequence sequence, const juce::File& sequenceFile)
{
    const auto folder = sequenceFile.getParentDirectory();
    for (auto& span : sequence.spans)
    {
        const auto file = fileFromPath(span.file);
        if (file.isAChildOf(folder))
            span.file = file.getRelativePathFrom(folder).replaceCharacter('\\', '/').toStdString();
    }
    return sequence;
}

/** @p sequence as read from @p sequenceFile, with every span path made full. */
inline sequence::SampleSequence withResolvedSpanPaths(sequence::SampleSequence sequence, const juce::File& sequenceFile)
{
    const auto folder = sequenceFile.getParentDirectory();
    for (auto& span : sequence.spans)
    {
        const auto text = juce::String::fromUTF8(span.file.c_str());
        if (! juce::File::isAbsolutePath(text))
            span.file = pathOf(folder.getChildFile(text.replaceCharacter('/', juce::File::getSeparatorChar())));
    }
    return sequence;
}

/** Parses @p text as the contents of @p sequenceFile, resolving its paths. */
inline std::optional<sequence::SampleSequence> parseFile(const juce::String& text, const juce::File& sequenceFile,
                                                         std::string* error = nullptr)
{
    sequence::SampleSequence parsed;
    if (! sequence::parse(text.toStdString(), parsed, error))
        return std::nullopt;
    return withResolvedSpanPaths(std::move(parsed), sequenceFile);
}

/** The sequence in @p sequenceFile, with full span paths. */
inline std::optional<sequence::SampleSequence> load(const juce::File& sequenceFile, std::string* error = nullptr)
{
    if (! sequenceFile.existsAsFile())
    {
        if (error != nullptr)
            *error = "no such file";
        return std::nullopt;
    }
    return parseFile(sequenceFile.loadFileAsString(), sequenceFile, error);
}

/** Writes @p sequence (full span paths) to @p sequenceFile. Through a
    temporary file, so a failed write never leaves half a sequence behind. */
inline bool save(const juce::File& sequenceFile, const sequence::SampleSequence& sequence)
{
    if (sequenceFile.getParentDirectory().createDirectory().failed())
        return false;

    const auto text = sequence::serialize(withStoredSpanPaths(sequence, sequenceFile));

    juce::TemporaryFile temp(sequenceFile);
    return temp.getFile().replaceWithText(juce::String::fromUTF8(text.c_str()), false, false, "\n")
        && temp.overwriteTargetFileWithTemporary();
}

/**
    Reads a sequence as one continuous stream of float samples, opening each
    span's file the first time it's needed.

    A span file with fewer channels than the sequence repeats its last one, the
    rule the players follow too, and a missing or unreadable file reads as
    silence rather than failing the whole read: one lost block shouldn't take
    the rest of a two-hour recording with it.
*/
class SequenceReader : public juce::AudioFormatReader
{
public:
    SequenceReader(juce::InputStream* stream, sequence::SampleSequence sequence)
        : juce::AudioFormatReader(stream, "SoundSplice sequence"),
          sequence_(std::move(sequence))
    {
        audioformats::registerAll(formats_); // not sequences: a span never points at another sequence

        sampleRate            = sequence_.sampleRate;
        numChannels           = (unsigned int) sequence_.numChannels;
        bitsPerSample         = 32;
        usesFloatingPointData = true;

        spanStarts_.reserve(sequence_.spans.size());
        for (const auto& span : sequence_.spans)
        {
            spanStarts_.push_back(lengthInSamples);
            lengthInSamples += span.length;
        }
    }

    bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                     juce::int64 startSampleInFile, int numSamples) override
    {
        for (int ch = 0; ch < numDestChannels; ++ch)
            if (destChannels[ch] != nullptr)
                std::fill_n(destChannels[ch] + startOffsetInDestBuffer, numSamples, 0);

        if (numSamples <= 0 || spanStarts_.empty())
            return true;

        // The last span starting at or before the first sample wanted.
        auto index = (size_t) std::max<std::ptrdiff_t>(
            0, std::upper_bound(spanStarts_.begin(), spanStarts_.end(), startSampleInFile) - spanStarts_.begin() - 1);

        int done = 0;
        for (; index < sequence_.spans.size() && done < numSamples; ++index)
        {
            const auto& span     = sequence_.spans[index];
            const auto  spanFrom = spanStarts_[index];
            const auto  wanted   = startSampleInFile + done;
            const auto  spanTo   = spanFrom + span.length;

            if (wanted >= spanTo)
                continue;

            // A read starting before the sequence (the base class pads those)
            // leaves a leading gap, which the zero fill above has covered.
            const auto skip  = std::max<juce::int64>(0, spanFrom - wanted);
            done            += (int) std::min<juce::int64>(skip, numSamples - done);
            const int  count = (int) std::min<juce::int64>(numSamples - done, spanTo - (startSampleInFile + done));
            if (count <= 0)
                continue;

            if (auto* reader = readerFor(span.file))
            {
                const int readerChannels = juce::jmax(1, (int) reader->numChannels);
                scratch_.setSize(readerChannels, count, false, false, true);
                reader->read(scratch_.getArrayOfWritePointers(), readerChannels,
                             span.start + (startSampleInFile + done - spanFrom), count);

                for (int ch = 0; ch < numDestChannels; ++ch)
                {
                    if (destChannels[ch] == nullptr)
                        continue;

                    // Floating-point data: the int pointers really hold floats.
                    const auto* source = scratch_.getReadPointer(juce::jmin(ch, readerChannels - 1));
                    std::copy_n(source, count, (float*) destChannels[ch] + startOffsetInDestBuffer + done);
                }
            }

            done += count;
        }

        return true;
    }

private:
    juce::AudioFormatReader* readerFor(const std::string& path)
    {
        auto it = readers_.find(path);
        if (it == readers_.end())
        {
            const auto file = fileFromPath(path);
            std::unique_ptr<juce::AudioFormatReader> reader;
            if (! isSequenceFile(file))
                reader.reset(formats_.createReaderFor(file));
            it = readers_.emplace(path, std::move(reader)).first;
        }
        return it->second.get();
    }

    sequence::SampleSequence  sequence_;
    std::vector<juce::int64>  spanStarts_;
    juce::AudioFormatManager  formats_;
    juce::AudioBuffer<float>  scratch_;
    std::map<std::string, std::unique_ptr<juce::AudioFormatReader>> readers_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequenceReader)
};

/** Read-only: a sequence is written by writeBlocks and save, never by an
    AudioFormatWriter, since its whole point is not writing the audio it
    already has. */
class SequenceAudioFormat : public juce::AudioFormat
{
public:
    SequenceAudioFormat() : juce::AudioFormat("SoundSplice sequence", kExtension) {}

    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths() override { return { 32 }; }
    bool             canDoStereo() override { return true; }
    bool             canDoMono() override { return true; }

    juce::AudioFormatReader* createReaderFor(juce::InputStream* stream, bool deleteStreamIfOpeningFails) override
    {
        // Span paths are relative to the sequence file, so only a stream that
        // knows which file it is can be opened. AudioFormatManager and
        // AudioThumbnail's FileInputSource both open files this way.
        std::optional<sequence::SampleSequence> parsed;
        if (auto* fileStream = dynamic_cast<juce::FileInputStream*>(stream))
            parsed = parseFile(fileStream->readEntireStreamAsString(), fileStream->getFile());

        if (! parsed)
        {
            if (deleteStreamIfOpeningFails)
                delete stream;
            return nullptr;
        }

        return new SequenceReader(stream, std::move(*parsed));
    }

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>&,
                                                             const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;
    }

    using juce::AudioFormat::createWriterFor;
};

/** The formats every reader of project audio needs: every file format
    (engine/AudioFormats.h), and sequences. */
inline void registerFormats(juce::AudioFormatManager& formats)
{
    audioformats::registerAll(formats);
    formats.registerFormat(new SequenceAudioFormat(), false);
}

/** @p audio as a sequence: the sequence itself for a sequence file, or one
    span covering the whole of any other audio file. */
inline std::optional<sequence::SampleSequence> sequenceOf(const juce::File& audio)
{
    if (isSequenceFile(audio))
        return load(audio);

    juce::AudioFormatManager formats;
    audioformats::registerAll(formats);

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audio));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return std::nullopt;

    sequence::SampleSequence out;
    out.sampleRate  = reader->sampleRate;
    out.numChannels = juce::jmax(1, (int) reader->numChannels);
    if (reader->lengthInSamples > 0)
        out.spans.push_back({ pathOf(audio), 0, reader->lengthInSamples });
    return out;
}

/** Every file @p audio's samples come from: @p audio itself, and for a
    sequence the files its spans read. */
inline juce::Array<juce::File> filesUsedBy(const juce::File& audio)
{
    juce::Array<juce::File> files;
    files.add(audio);

    if (isSequenceFile(audio))
        if (const auto loaded = load(audio))
            for (const auto& span : loaded->spans)
                files.addIfNotAlreadyThere(fileFromPath(span.file));

    return files;
}

/** Writes @p buffer into @p folder as 32-bit float WAV blocks of at most
    @p blockFrames frames, named after @p stem, and returns the spans playing
    them in order. Float, so audio that went through an edit comes back bit
    for bit. Nothing, and no files left behind, if any write fails. */
inline std::optional<std::vector<sequence::Span>> writeBlocks(const juce::File& folder, const juce::String& stem,
                                                              const juce::AudioBuffer<float>& buffer,
                                                              double sampleRate, int blockFrames = kBlockFrames)
{
    std::vector<sequence::Span> spans;
    juce::Array<juce::File>     written;

    const auto fail = [&written]
    {
        for (const auto& file : written)
            file.deleteFile();
        return std::nullopt;
    };

    if (folder.createDirectory().failed() || sampleRate <= 0.0 || blockFrames <= 0)
        return fail();

    for (int from = 0; from < buffer.getNumSamples(); from += blockFrames)
    {
        const int  count = juce::jmin(blockFrames, buffer.getNumSamples() - from);
        const auto file  = folder.getNonexistentChildFile(stem + " block", ".wav");

        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return fail();
        written.add(file);

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(sampleRate)
                                                      .withNumChannels(buffer.getNumChannels())
                                                      .withBitsPerSample(32)
                                                      .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer(buffer, from, count))
            return fail();

        writer.reset(); // flushes and closes before the next block's name is chosen
        spans.push_back({ pathOf(file), 0, count });
    }

    return spans;
}

/** Reads @p count frames of @p audio (any format, sequences included) from
    @p start into @p out, sized to the file's channels. Past the end reads as
    silence. False if the file can't be opened. */
inline bool readRange(const juce::File& audio, juce::int64 start, int count, juce::AudioBuffer<float>& out)
{
    juce::AudioFormatManager formats;
    registerFormats(formats);

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audio));
    if (reader == nullptr)
        return false;

    const int channels = juce::jmax(1, (int) reader->numChannels);
    out.setSize(channels, juce::jmax(0, count));
    out.clear();
    return count <= 0 || reader->read(out.getArrayOfWritePointers(), channels, start, count);
}

} // namespace soundsplice::engine::sequencefile
