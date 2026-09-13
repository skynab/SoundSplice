#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace looper::engine
{
/**
    MP3 encoding, as a `juce::AudioFormat`.

    The one export format JUCE cannot produce on its own: `MP3AudioFormat`
    exists but its `createWriterFor` is `jassertfalse; return nullptr;` - it
    decodes only. The framework's other offer, `LAMEEncoderAudioFormat`, runs an
    installed `lame` *executable* over a temporary WAV file, which makes MP3
    export depend on what happens to be on the user's PATH.

    So this talks to libmp3lame directly (vendored - see cmake/lame.cmake). No
    subprocess, no temporary file, and MP3 export works on a machine with
    nothing installed.

    Being an AudioFormat rather than a bare function is the point: engine::
    AudioExport then treats MP3 exactly like WAV or FLAC instead of carrying a
    special case for it.
*/
class Mp3AudioFormat final : public juce::AudioFormat
{
public:
    Mp3AudioFormat();

    /** Constant bitrates, in kbps. Index into this is what
        AudioFormatWriterOptions::withQualityOptionIndex selects. */
    static juce::Array<int> bitratesKbps();

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;

    bool canDoStereo() override { return true; }
    bool canDoMono() override   { return true; }
    bool isCompressed() override { return true; }

    juce::StringArray getQualityOptions() override;

    /** Encoding only - use juce::AudioFormatManager to read MP3s back
        (CoreAudioFormat on macOS, MediaFoundation on Windows). Returns null. */
    juce::AudioFormatReader* createReaderFor (juce::InputStream*,
                                              bool deleteStreamIfOpeningFails) override;

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                                              const juce::AudioFormatWriterOptions& options) override;

    using juce::AudioFormat::createWriterFor;
};

} // namespace looper::engine
