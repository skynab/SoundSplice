#pragma once

#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "AudioFileTypes.h"
#include "EffectRender.h"
#include "engine/AudioExport.h"
#include "engine/AudioFormats.h"
#include "engine/Loudness.h"

namespace soundsplice::batch
{
/**
    Batch Process (Audition's): one effect chain, and optionally a loudness
    target, run over every audio file in a folder, each written as a new file
    in an output folder - the originals are never touched.

    One file at a time, start to finish, so a failure in one leaves the rest
    to go on; the caller runs them on a background job when it can (built-in
    effects only - a hosted plugin has to be made on the message thread).
*/
struct Settings
{
    std::vector<model::EffectSlot> chain;
    double                         loudnessLufs = 0.0; // 0: leave the loudness alone
    engine::ExportOptions          output;             // its sample rate is each file's own
    juce::File                     folder;             // where the results go
    double                         bpm = 120.0;        // for tempo-synced effects
};

struct Result
{
    bool         ok = false;
    juce::String error;
    juce::File   written;
};

/** The audio files directly in @p folder, by name. */
inline std::vector<juce::File> audioFilesIn(const juce::File& folder)
{
    std::vector<juce::File> files;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles))
        if (audiofiles::isImportableAudioFile(entry.getFile()))
            files.push_back(entry.getFile());
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareNatural(b.getFileName()) < 0; });
    return files;
}

/** Where @p input's result goes: its name, in @p settings' folder, in the
    output format. Never the input itself: one that would land on its own
    file is written as "<name> processed". */
inline juce::File outputFor(const juce::File& input, const Settings& settings)
{
    const auto extension = "." + engine::extensionFor(settings.output.format);
    auto       out       = settings.folder.getChildFile(input.getFileNameWithoutExtension() + extension);
    if (out == input)
        out = settings.folder.getChildFile(input.getFileNameWithoutExtension() + " processed" + extension);
    return out;
}

inline constexpr double kTruePeakCeiling = -1.0;

/** Reads @p input whole, runs @p settings' chain over it, brings it to the
    loudness target if there is one (true peak held under -1 dBTP), and
    writes it out. */
inline Result processFile(const juce::File& input, const Settings& settings, engine::PluginHost& plugins)
{
    juce::AudioFormatManager formats;
    engine::audioformats::registerAll(formats);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(input));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return { false, "can't be read" };
    if (reader->lengthInSamples > std::numeric_limits<int>::max())
        return { false, "is too long to process in one piece" };

    const int channels = (int) juce::jlimit(1u, 8u, reader->numChannels);
    juce::AudioBuffer<float> audio(channels, (int) reader->lengthInSamples);
    if (! reader->read(&audio, 0, audio.getNumSamples(), 0, true, true))
        return { false, "can't be read" };

    if (! settings.chain.empty())
        if (const auto rendered = renderEffectChain(settings.chain, audio, reader->sampleRate, settings.bpm, plugins); ! rendered.ok)
            return { false, rendered.error };

    if (settings.loudnessLufs < 0.0)
    {
        engine::LoudnessMeter meter;
        meter.prepare(reader->sampleRate, 2);
        const float* both[2] { audio.getReadPointer(0), audio.getReadPointer(juce::jmin(1, channels - 1)) };
        meter.process(both, 2, audio.getNumSamples());
        const auto report = engine::LoudnessReport::of(meter, audio.getNumSamples() / reader->sampleRate);

        engine::LoudnessGain gain;
        if (engine::loudnessGainFor(report.integratedLufs, report.truePeakDb, settings.loudnessLufs, kTruePeakCeiling,
                                    true, gain))
            audio.applyGain(juce::Decibels::decibelsToGain((float) gain.gainDb));
    }

    auto options       = settings.output;
    options.sampleRate = reader->sampleRate;
    const auto out     = outputFor(input, settings);
    if (! engine::writeAudioFile(out, audio, options))
        return { false, "couldn't be written to " + out.getFullPathName() };
    return { true, {}, out };
}

} // namespace soundsplice::batch
