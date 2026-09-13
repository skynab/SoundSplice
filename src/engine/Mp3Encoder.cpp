#include "engine/Mp3Encoder.h"

#include <lame.h>

#include <vector>

namespace looper::engine
{
namespace
{
const char* const kFormatName = "MP3 file";

/** LAME's own sizing rule for the output buffer, from lame.h:
        mp3buf_size in bytes = 1.25 * num_samples + 7200
    Passing 0 would tell LAME to skip the bounds check entirely, which is
    exactly the wrong trade for a few bytes of slack. */
int encodedCapacityFor (int numSamples)
{
    return (int) (1.25 * (double) numSamples) + 7200;
}

/**
    Writes MP3 through libmp3lame.

    Reports itself as floating point, which makes JUCE hand write() the raw
    float samples (bit-cast through `const int**`, the framework's convention)
    rather than quantising them to integers first. That matters: LAME takes
    float input natively, so quantising on the way in would throw away
    precision the encoder was going to use.
*/
class Mp3Writer final : public juce::AudioFormatWriter
{
public:
    Mp3Writer (std::unique_ptr<juce::OutputStream> out, double rate, unsigned int channels, int bitrateKbps)
        : juce::AudioFormatWriter (out.get(), kFormatName, rate, channels, 16)
    {
        out.release(); // the base class owns it from here

        // Bit depth is reported as 16 because an MP3 has no meaningful one and
        // 16 is what a decoder will hand back; usesFloatingPointData is about
        // what write() receives, which is a separate question.
        usesFloatingPointData = true;

        lame_ = lame_init();
        if (lame_ == nullptr)
            return;

        lame_set_in_samplerate (lame_, (int) rate);
        lame_set_num_channels  (lame_, (int) channels);
        lame_set_mode          (lame_, channels == 1 ? MONO : JOINT_STEREO);
        lame_set_brate         (lame_, bitrateKbps);

        // 2 is LAME's "near-best quality, still fast" setting. This runs once
        // per export rather than in any hot path, so the cost is irrelevant
        // next to not throwing away quality in the file the user keeps.
        lame_set_quality       (lame_, 2);

        // No Xing/LAME header. Writing one is a two-step dance: LAME emits a
        // placeholder frame first, and the caller must seek back to the start
        // afterwards and overwrite it with lame_get_lametag_frame(). A generic
        // juce::OutputStream cannot be rewound, so the placeholder would be
        // left in place - a zeroed header is worse than no header, because a
        // decoder trusts it.
        //
        // Nothing is lost: the tag exists to describe variable-bitrate files,
        // and this writes constant bitrate, where duration and seek position
        // both follow from the file size.
        lame_set_bWriteVbrTag  (lame_, 0);

        ready_ = lame_init_params (lame_) >= 0;
    }

    ~Mp3Writer() override
    {
        if (lame_ != nullptr)
        {
            if (ready_)
                flushEncoder();

            lame_close (lame_);
        }
    }

    bool isReady() const noexcept { return ready_; }

    bool write (const int** samplesToWrite, int numSamples) override
    {
        if (! ready_ || output == nullptr)
            return false;

        if (samplesToWrite == nullptr || numSamples <= 0)
            return true; // nothing to do is not a failure

        // usesFloatingPointData means these ints are floats in disguise - see
        // the class comment.
        const auto* left  = reinterpret_cast<const float*> (samplesToWrite[0]);
        const auto* right = numChannels > 1 ? reinterpret_cast<const float*> (samplesToWrite[1]) : left;

        if (left == nullptr)
            return false;

        encoded_.resize ((size_t) encodedCapacityFor (numSamples));

        // The _ieee_float variant takes samples already scaled to +/-1, which
        // is what the mixer produces, so nothing is rescaled on the way in.
        const int produced = lame_encode_buffer_ieee_float (lame_, left, right, numSamples,
                                                            encoded_.data(), (int) encoded_.size());
        if (produced < 0)
            return false;

        return produced == 0 || output->write (encoded_.data(), (size_t) produced);
    }

private:
    /** The encoder buffers internally, so the last partial frame only reaches
        the stream when it is flushed - without this the file is short and its
        final moments are missing. */
    void flushEncoder()
    {
        encoded_.resize (7200);
        const int produced = lame_encode_flush (lame_, encoded_.data(), (int) encoded_.size());

        if (produced > 0 && output != nullptr)
            output->write (encoded_.data(), (size_t) produced);

        if (output != nullptr)
            output->flush();
    }

    lame_global_flags*         lame_  = nullptr;
    bool                       ready_ = false;
    std::vector<unsigned char> encoded_;
};
} // namespace

Mp3AudioFormat::Mp3AudioFormat()
    : juce::AudioFormat (kFormatName, juce::StringArray { ".mp3" })
{
}

juce::Array<int> Mp3AudioFormat::bitratesKbps()
{
    // MPEG-1 Layer III's constant bitrates, from the usable end up. 320 is the
    // format's ceiling; below 128 the artefacts are audible on music, which is
    // why the list stops at 96 rather than going down to 8.
    return { 96, 128, 160, 192, 256, 320 };
}

juce::Array<int> Mp3AudioFormat::getPossibleSampleRates()
{
    // MPEG-1 and MPEG-2 Layer III between them. LAME resamples anything else,
    // silently - so the rates it can store natively are the ones offered.
    return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
}

juce::Array<int> Mp3AudioFormat::getPossibleBitDepths()
{
    // An MP3 has no sample depth of its own. 16 is what a decoder produces and
    // what the writer reports, so it is the only honest answer here.
    return { 16 };
}

juce::StringArray Mp3AudioFormat::getQualityOptions()
{
    juce::StringArray options;
    for (int kbps : bitratesKbps())
        options.add (juce::String (kbps) + " kbps");
    return options;
}

juce::AudioFormatReader* Mp3AudioFormat::createReaderFor (juce::InputStream* stream,
                                                          bool deleteStreamIfOpeningFails)
{
    if (deleteStreamIfOpeningFails)
        delete stream;

    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter>
Mp3AudioFormat::createWriterFor (std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                 const juce::AudioFormatWriterOptions& options)
{
    if (streamToWriteTo == nullptr)
        return nullptr;

    const int numChannels = options.getNumChannels();
    if (numChannels < 1 || numChannels > 2)
        return nullptr; // MP3 is mono or stereo; anything else would be silently wrong

    const auto rates = getPossibleSampleRates();
    if (! rates.contains ((int) options.getSampleRate()))
        return nullptr;

    const auto bitrates     = bitratesKbps();
    const int  qualityIndex = juce::jlimit (0, bitrates.size() - 1, options.getQualityOptionIndex());

    // Every rejection that can be decided from the options alone happens above,
    // *before* the stream is taken - so the usual "consumed on success, left
    // alone on failure" contract holds for all of them. Only lame_init_params
    // failing gets past that point, and it takes the stream down with the
    // writer when this returns null. That is safe (the base class closes and
    // deletes it) but it does leave the caller holding an emptied unique_ptr,
    // so engine::AudioExport deletes the part-written file on a null writer.
    auto writer = std::make_unique<Mp3Writer> (std::move (streamToWriteTo),
                                               options.getSampleRate(),
                                               (unsigned int) numChannels,
                                               bitrates[qualityIndex]);

    return writer->isReady() ? std::move (writer) : nullptr;
}

} // namespace looper::engine
