#include "engine/AudioFormats.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <opusfile.h>
#include <wavpack.h>

#include "engine/PcmContainers.h"

namespace soundsplice::engine
{
namespace
{
    /** Zeros @p numSamples of each destination channel from @p offset. */
    void clearDestination(int* const* destChannels, int numDestChannels, int offset, int numSamples)
    {
        for (int ch = 0; ch < numDestChannels; ++ch)
            if (destChannels[ch] != nullptr)
                std::fill_n(destChannels[ch] + offset, numSamples, 0);
    }

    //==========================================================================
    /** Samples laid out as a PcmContainer describes, decoded as Import Raw
        Data decodes them. */
    class PcmContainerReader final : public juce::AudioFormatReader
    {
    public:
        PcmContainerReader(juce::InputStream* stream, const juce::String& name, PcmContainer container)
            : juce::AudioFormatReader(stream, name), container_(container)
        {
            sampleRate            = container.format.sampleRate;
            numChannels           = (unsigned int) container.format.channels;
            lengthInSamples       = (juce::int64) container.frames;
            bitsPerSample         = 32;
            usesFloatingPointData = true;
        }

        bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                         juce::int64 startSampleInFile, int numSamples) override
        {
            clearDestination(destChannels, numDestChannels, startOffsetInDestBuffer, numSamples);

            const auto first = juce::jmax<juce::int64>(0, startSampleInFile);
            const auto end   = juce::jmin<juce::int64>(lengthInSamples, startSampleInFile + numSamples);
            if (end <= first)
                return true;

            const auto& format     = container_.format;
            const int   channels   = format.channels;
            const int   sampleSize = bytesPerSample(format.encoding);
            const int   frameBytes = format.frameBytes();
            const int   frames     = (int) (end - first);

            bytes_.resize((size_t) frames * (size_t) frameBytes);
            if (! input->setPosition((juce::int64) container_.dataOffset + first * frameBytes))
                return false;
            const int got    = input->read(bytes_.data(), (int) bytes_.size());
            const int usable = juce::jmax(0, got) / frameBytes;

            const int skip = (int) (first - startSampleInFile);
            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                if (destChannels[ch] == nullptr)
                    continue;
                auto*     out    = (float*) destChannels[ch] + startOffsetInDestBuffer + skip;
                const int source = juce::jmin(ch, channels - 1);
                for (int n = 0; n < usable; ++n)
                    out[n] = rawpcm::decodeSample(bytes_.data() + ((size_t) n * (size_t) channels + (size_t) source) * (size_t) sampleSize,
                                                  format.encoding, format.bigEndian);
            }
            return true;
        }

    private:
        PcmContainer              container_;
        std::vector<std::uint8_t> bytes_;
    };

    using ContainerParser = std::optional<PcmContainer> (*)(const pcmcontainer::ReadAt&, std::uint64_t);

    class PcmContainerFormat final : public juce::AudioFormat
    {
    public:
        PcmContainerFormat(const juce::String& name, const juce::StringArray& extensions, ContainerParser parse)
            : juce::AudioFormat(name, extensions), parse_(parse)
        {
        }

        juce::Array<int> getPossibleSampleRates() override { return {}; }
        juce::Array<int> getPossibleBitDepths() override { return {}; }
        bool             canDoStereo() override { return true; }
        bool             canDoMono() override { return true; }

        juce::AudioFormatReader* createReaderFor(juce::InputStream* stream, bool deleteStreamIfOpeningFails) override
        {
            const auto start  = stream->getPosition();
            const auto length = stream->getTotalLength() - start;

            const pcmcontainer::ReadAt read = [stream, start](std::uint64_t offset, std::uint8_t* destination, std::size_t count)
            {
                return stream->setPosition(start + (juce::int64) offset)
                    && stream->read(destination, (int) count) == (int) count;
            };

            if (length > 0)
                if (auto container = parse_(read, (std::uint64_t) length))
                    if (container->frames > 0)
                        return new PcmContainerReader(stream, getFormatName(), *container);

            if (deleteStreamIfOpeningFails)
                delete stream;
            return nullptr;
        }

        std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>&,
                                                                 const juce::AudioFormatWriterOptions&) override
        {
            return nullptr;
        }

        using juce::AudioFormat::createWriterFor;

    private:
        ContainerParser parse_;
    };

    //==========================================================================
    namespace opusio
    {
        int read(void* stream, unsigned char* buffer, int count)
        {
            return juce::jmax(0, static_cast<juce::InputStream*>(stream)->read(buffer, count));
        }

        int seek(void* stream, opus_int64 offset, int whence)
        {
            auto*      in   = static_cast<juce::InputStream*>(stream);
            const auto base = whence == SEEK_CUR ? in->getPosition() : whence == SEEK_END ? in->getTotalLength() : 0;
            return in->setPosition(base + offset) ? 0 : -1;
        }

        opus_int64 tell(void* stream)
        {
            return static_cast<juce::InputStream*>(stream)->getPosition();
        }
    }

    /** An Ogg Opus file, always decoded at Opus's 48 kHz. A chained file whose
        links have different channel counts is read with the first link's. */
    class OpusReader final : public juce::AudioFormatReader
    {
    public:
        OpusReader(juce::InputStream* stream, OggOpusFile* file)
            : juce::AudioFormatReader(stream, "Opus file"), file_(file)
        {
            sampleRate            = 48000.0;
            numChannels           = (unsigned int) juce::jmax(1, op_channel_count(file, 0));
            lengthInSamples       = juce::jmax<juce::int64>(0, op_pcm_total(file, -1));
            bitsPerSample         = 32;
            usesFloatingPointData = true;
        }

        ~OpusReader() override { op_free(file_); }

        bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                         juce::int64 startSampleInFile, int numSamples) override
        {
            clearDestination(destChannels, numDestChannels, startOffsetInDestBuffer, numSamples);

            auto       at  = juce::jmax<juce::int64>(0, startSampleInFile);
            const auto end = juce::jmin<juce::int64>(lengthInSamples, startSampleInFile + numSamples);
            if (end <= at)
                return true;

            // Room for the most one call returns, 120 ms at 48 kHz. A later
            // link with more channels just gets fewer frames back per call.
            constexpr int kMaxFrames = 5760;
            const int     room       = kMaxFrames * (int) numChannels;
            decoded_.resize((size_t) room);

            // Not in what's already decoded, and not where the decoder is, or
            // coming up soon: seek. A decoder that starts from a seek takes a
            // while to settle, longer than opusfile's own 80 ms pre-roll on
            // steady tones, so it starts a second early and decodes forward.
            // That way a read gives the same audio however it was reached,
            // which a waveform drawn in pieces and an edit that keeps the
            // audio either side of it both rely on.
            const bool buffered = at >= bufferStart_ && at < bufferStart_ + bufferFrames_;
            if (! buffered && (at < decoderPosition_ || at > decoderPosition_ + kSettleFrames))
            {
                const auto from = juce::jmax<juce::int64>(0, at - kSettleFrames);
                if (op_pcm_seek(file_, from) != 0)
                    return false;
                decoderPosition_ = from;
                bufferFrames_    = 0;
            }

            while (at < end)
            {
                // A call decodes a whole packet, often more than was asked
                // for, so what's left over stays for the next read.
                if (at < bufferStart_ || at >= bufferStart_ + bufferFrames_)
                {
                    int link = 0;
                    const int frames = op_read_float(file_, decoded_.data(), room, &link);
                    if (frames <= 0)
                    {
                        bufferFrames_ = 0;
                        break; // an error or the end: the rest reads as silence
                    }

                    bufferStart_      = decoderPosition_;
                    bufferFrames_     = frames;
                    bufferChannels_   = juce::jmax(1, op_channel_count(file_, link));
                    decoderPosition_ += frames;
                    continue;
                }

                const int  from   = (int) (at - bufferStart_);
                const int  count  = (int) juce::jmin<juce::int64>(end - at, bufferFrames_ - from);
                const auto offset = startOffsetInDestBuffer + (int) (at - startSampleInFile);

                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (destChannels[ch] != nullptr)
                    {
                        const int source = juce::jmin(ch, bufferChannels_ - 1);
                        auto*     out    = (float*) destChannels[ch] + offset;
                        for (int n = 0; n < count; ++n)
                            out[n] = decoded_[(size_t) ((from + n) * bufferChannels_ + source)];
                    }

                at += count;
            }
            return true;
        }

    private:
        static constexpr juce::int64 kSettleFrames = 48000;

        OggOpusFile*       file_;
        juce::int64        decoderPosition_ = 0; // the frame op_read_float decodes next
        juce::int64        bufferStart_     = 0; // the frame decoded_ starts at
        int                bufferFrames_    = 0;
        int                bufferChannels_  = 1;
        std::vector<float> decoded_;
    };

    class OpusAudioFormat final : public juce::AudioFormat
    {
    public:
        OpusAudioFormat() : juce::AudioFormat("Opus file", juce::StringArray { ".opus" }) {}

        juce::Array<int> getPossibleSampleRates() override { return { 48000 }; }
        juce::Array<int> getPossibleBitDepths() override { return {}; }
        bool             canDoStereo() override { return true; }
        bool             canDoMono() override { return true; }
        bool             isCompressed() override { return true; }

        juce::AudioFormatReader* createReaderFor(juce::InputStream* stream, bool deleteStreamIfOpeningFails) override
        {
            static const OpusFileCallbacks callbacks { opusio::read, opusio::seek, opusio::tell, nullptr };

            if (auto* file = op_open_callbacks(stream, &callbacks, nullptr, 0, nullptr))
                return new OpusReader(stream, file);

            if (deleteStreamIfOpeningFails)
                delete stream;
            return nullptr;
        }

        std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>&,
                                                                 const juce::AudioFormatWriterOptions&) override
        {
            return nullptr;
        }

        using juce::AudioFormat::createWriterFor;
    };

    //==========================================================================
    namespace wavpackio
    {
        juce::InputStream& in(void* id) { return *static_cast<juce::InputStream*>(id); }

        int32_t readBytes(void* id, void* data, int32_t count) { return juce::jmax(0, in(id).read(data, count)); }
        int64_t getPos(void* id) { return in(id).getPosition(); }
        int     setPosAbs(void* id, int64_t pos) { return in(id).setPosition(pos) ? 0 : -1; }

        int setPosRel(void* id, int64_t delta, int mode)
        {
            const auto base = mode == SEEK_CUR ? in(id).getPosition() : mode == SEEK_END ? in(id).getTotalLength() : 0;
            return in(id).setPosition(base + delta) ? 0 : -1;
        }

        // Only ever the byte just read, so stepping back one is the same thing.
        int pushBackByte(void* id, int c)
        {
            return in(id).setPosition(in(id).getPosition() - 1) ? c : EOF;
        }

        int64_t getLength(void* id) { return in(id).getTotalLength(); }
        int     canSeek(void*) { return 1; }
    }

    class WavPackReader final : public juce::AudioFormatReader
    {
    public:
        WavPackReader(juce::InputStream* stream, WavpackContext* context)
            : juce::AudioFormatReader(stream, "WavPack file"), context_(context)
        {
            sampleRate            = (double) WavpackGetSampleRate(context);
            numChannels           = (unsigned int) juce::jmax(1, WavpackGetNumChannels(context));
            lengthInSamples       = juce::jmax<juce::int64>(0, WavpackGetNumSamples64(context));
            bitsPerSample         = 32;
            usesFloatingPointData = true;
            isFloat_              = (WavpackGetMode(context) & MODE_FLOAT) != 0;
            scale_                = 1.0f / (float) (1u << (juce::jlimit(1, 4, WavpackGetBytesPerSample(context)) * 8 - 1));
        }

        ~WavPackReader() override { WavpackCloseFile(context_); }

        bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                         juce::int64 startSampleInFile, int numSamples) override
        {
            clearDestination(destChannels, numDestChannels, startOffsetInDestBuffer, numSamples);

            auto       at  = juce::jmax<juce::int64>(0, startSampleInFile);
            const auto end = juce::jmin<juce::int64>(lengthInSamples, startSampleInFile + numSamples);
            if (end <= at)
                return true;

            if (at != position_)
            {
                if (! WavpackSeekSample64(context_, at))
                    return false;
                position_ = at;
            }

            const int channels = (int) numChannels;
            constexpr int kBlock = 4096;
            decoded_.resize((size_t) kBlock * (size_t) channels);

            while (at < end)
            {
                const auto wanted = (uint32_t) juce::jmin<juce::int64>(end - at, kBlock);
                const auto frames = (int) WavpackUnpackSamples(context_, decoded_.data(), wanted);
                if (frames <= 0)
                    break;

                const auto offset = startOffsetInDestBuffer + (int) (at - startSampleInFile);
                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (destChannels[ch] != nullptr)
                    {
                        const int source = juce::jmin(ch, channels - 1);
                        auto*     out    = (float*) destChannels[ch] + offset;
                        for (int n = 0; n < frames; ++n)
                        {
                            const auto sample = decoded_[(size_t) (n * channels + source)];
                            if (isFloat_)
                                std::memcpy(out + n, &sample, sizeof(float));
                            else
                                out[n] = (float) sample * scale_;
                        }
                    }

                at        += frames;
                position_ += frames;
            }
            return true;
        }

    private:
        WavpackContext*      context_;
        juce::int64          position_ = 0;
        bool                 isFloat_  = false;
        float                scale_    = 1.0f;
        std::vector<int32_t> decoded_;
    };

    class WavPackAudioFormat final : public juce::AudioFormat
    {
    public:
        WavPackAudioFormat() : juce::AudioFormat("WavPack file", juce::StringArray { ".wv" }) {}

        juce::Array<int> getPossibleSampleRates() override { return {}; }
        juce::Array<int> getPossibleBitDepths() override { return {}; }
        bool             canDoStereo() override { return true; }
        bool             canDoMono() override { return true; }
        bool             isCompressed() override { return true; }

        juce::AudioFormatReader* createReaderFor(juce::InputStream* stream, bool deleteStreamIfOpeningFails) override
        {
            static WavpackStreamReader64 callbacks {
                wavpackio::readBytes, nullptr, wavpackio::getPos, wavpackio::setPosAbs, wavpackio::setPosRel,
                wavpackio::pushBackByte, wavpackio::getLength, wavpackio::canSeek, nullptr, nullptr
            };

            // Floats scaled to +/-1, and DSD audio decimated to PCM.
            char error[80] {};
            if (auto* context = WavpackOpenFileInputEx64(&callbacks, stream, nullptr, error,
                                                         OPEN_NORMALIZE | OPEN_DSD_AS_PCM, 0))
            {
                if (WavpackGetNumSamples64(context) > 0 && WavpackGetSampleRate(context) > 0)
                    return new WavPackReader(stream, context);
                WavpackCloseFile(context);
            }

            if (deleteStreamIfOpeningFails)
                delete stream;
            return nullptr;
        }

        std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>&,
                                                                 const juce::AudioFormatWriterOptions&) override
        {
            return nullptr;
        }

        using juce::AudioFormat::createWriterFor;
    };
}

void audioformats::registerAll(juce::AudioFormatManager& formats)
{
    formats.registerBasicFormats();
    formats.registerFormat(new OpusAudioFormat(), false);
    formats.registerFormat(new WavPackAudioFormat(), false);
    formats.registerFormat(new PcmContainerFormat("Wave64 file", { ".w64" }, pcmcontainer::parseWave64), false);
    formats.registerFormat(new PcmContainerFormat("RF64 file", { ".rf64", ".bw64" }, pcmcontainer::parseRf64), false);
    formats.registerFormat(new PcmContainerFormat("CAF file", { ".caf" }, pcmcontainer::parseCaf), false);
}

} // namespace soundsplice::engine
