#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <sstream>

#include "engine/ClipSpectralEdits.h"
#include "engine/SequenceAudioFormat.h"

namespace soundsplice::spectralrender
{
/**
    What a clip with spectral edits plays: its file with the edits applied,
    written once to a cache and found there again after, so playback and
    export read an ordinary file and the original is never touched.

    A cached file is named by everything it's made from (the source's path,
    size and time, and each edit), so a changed source or a changed edit
    finds a new name rather than a stale file. Only the stretches the edits
    touch are edited; the rest is copied across as it is.
*/

inline juce::File cacheFolder()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SoundSplice Spectral Edits");
}

inline juce::String keyFor(const juce::File& source, const engine::SpectralRegions& regions)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(17);
    text << source.getFullPathName().toStdString() << "|" << source.getSize() << "|"
         << source.getLastModificationTime().toMilliseconds();
    for (const auto& region : regions)
        text << "|" << region.startSeconds << "," << region.endSeconds << "," << region.lowHz << ","
             << region.highHz << "," << region.gainDb;

    return juce::String::toHexString(juce::String(text.str()).hashCode64())
         + "-" + juce::String::toHexString((juce::int64) juce::String(text.str()).length());
}

/** Writes @p source with @p regions applied to @p destination, a 32-bit
    float WAV. False, writing nothing, if the source can't be read. */
inline bool render(const juce::File& source, const engine::SpectralRegions& regions, const juce::File& destination)
{
    juce::AudioFormatManager formats;
    engine::sequencefile::registerFormats(formats);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    destination.getParentDirectory().createDirectory();
    const auto partial = destination.getSiblingFile(destination.getFileNameWithoutExtension() + ".partial");
    const int  channels = (int) reader->numChannels;
    const auto total    = reader->lengthInSamples;
    const double rate   = reader->sampleRate;
    partial.deleteFile(); // an output stream appends to what's there
    {
        std::unique_ptr<juce::OutputStream> output(partial.createOutputStream());
        if (output == nullptr)
            return false;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(output,
            juce::AudioFormatWriterOptions{}
                .withSampleRate(rate)
                .withNumChannels(channels)
                .withBitsPerSample(32)
                .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr)
            return false;

        // [from, to) of the file, read, edited if @p edit, and written.
        const auto pass = [&](std::int64_t from, std::int64_t to, bool edit)
        {
            constexpr std::int64_t kChunk = 1 << 16;
            for (std::int64_t at = from; at < to;)
            {
                const auto frames = (int) (edit ? to - at : std::min(kChunk, to - at));
                juce::AudioBuffer<float> buffer(channels, frames);
                reader->read(&buffer, 0, frames, at, true, true);
                if (edit)
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        std::vector<float> samples(buffer.getReadPointer(ch), buffer.getReadPointer(ch) + frames);
                        engine::clipspectral::apply(samples, at, rate, regions);
                        buffer.copyFrom(ch, 0, samples.data(), frames);
                    }
                if (! writer->writeFromAudioSampleBuffer(buffer, 0, frames))
                    return false;
                at += frames;
            }
            return true;
        };

        std::int64_t done = 0;
        for (const auto& [from, to] : engine::clipspectral::spans(regions, rate, total))
        {
            if (! pass(done, from, false) || ! pass(from, to, true))
                return false;
            done = to;
        }
        if (! pass(done, total, false))
            return false;
    }

    return partial.moveFileTo(destination);
}

/** The file a clip with @p regions on @p source plays: the source itself
    with none, otherwise the cached render, made now if it isn't there yet.
    Falls back to the source if it can't be made, so the clip still plays. */
inline juce::File fileFor(const juce::File& source, const engine::SpectralRegions& regions)
{
    if (regions.empty() || ! source.existsAsFile())
        return source;

    const auto rendered = cacheFolder().getChildFile(keyFor(source, regions) + ".wav");
    if (rendered.existsAsFile() || render(source, regions, rendered))
        return rendered;
    return source;
}

} // namespace soundsplice::spectralrender
