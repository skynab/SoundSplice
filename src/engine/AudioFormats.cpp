#include "engine/AudioFormats.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <ogg/ogg.h>
#include <opus.h>
#include <opusfile.h>
#include <wavpack.h>

#include "engine/Dither.h"
#include "engine/Resample.h"

#include "engine/PcmContainers.h"

#if JUCE_WINDOWS
 #include <mfapi.h>
 #include <mferror.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #pragma comment(lib, "mfplat.lib")
 #pragma comment(lib, "mfreadwrite.lib")
 #pragma comment(lib, "mfuuid.lib")
 #pragma comment(lib, "ole32.lib")
#endif

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

#if JUCE_WINDOWS
namespace
{
    /**
        M4A, AAC and the audio of a video file (MP4, MOV, WMV), read through
        Windows' own Media Foundation decoders: nothing bundled, nothing
        licensed, and whatever codecs the system has. (On a Mac, JUCE's
        CoreAudioFormat does the same through Core Audio.)

        Streamed, as the other readers are: decoded forward a sample buffer
        at a time, and a read elsewhere seeks - Media Foundation lands on a
        keyframe at or before the time asked, and the decoded samples before
        the one wanted are dropped by their timestamps, so a seek is sample
        exact. The first audio stream only; any video is never decoded.

        Media Foundation wants COM on the thread that calls it, and a reader
        is read from whichever thread plays or renders, so each call makes
        sure of it for its thread.
    */
    class MediaFoundationReader final : public juce::AudioFormatReader
    {
    public:
        static constexpr const char* kName = "Media Foundation file";

        static bool ensureCom()
        {
            thread_local bool done = false;
            if (! done)
            {
                const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                done = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; // already in an apartment: fine
            }
            static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
            return done && started;
        }

        MediaFoundationReader(juce::InputStream* stream, const juce::File& file)
            : juce::AudioFormatReader(stream, kName)
        {
            if (! ensureCom())
                return;
            IMFSourceReader* reader = nullptr;
            if (FAILED(MFCreateSourceReaderFromURL(file.getFullPathName().toWideCharPointer(), nullptr, &reader)))
                return;
            reader_ = reader;

            // The first audio stream, as 32-bit float.
            reader_->SetStreamSelection((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
            reader_->SetStreamSelection((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
            IMFMediaType* wanted = nullptr;
            if (FAILED(MFCreateMediaType(&wanted)))
                return;
            wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            const auto set = reader_->SetCurrentMediaType((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, wanted);
            wanted->Release();
            if (FAILED(set))
                return;

            IMFMediaType* actual = nullptr;
            if (FAILED(reader_->GetCurrentMediaType((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)))
                return;
            UINT32 channels = 0, rate = 0;
            actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            actual->Release();
            if (channels == 0 || rate == 0)
                return;

            PROPVARIANT duration;
            PropVariantInit(&duration);
            if (FAILED(reader_->GetPresentationAttribute((DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)))
                return;
            const auto hundredNs = (juce::int64) duration.uhVal.QuadPart;
            PropVariantClear(&duration);

            sampleRate            = rate;
            numChannels           = channels;
            bitsPerSample         = 32;
            usesFloatingPointData = true;
            lengthInSamples       = (juce::int64) ((double) hundredNs * rate / 1.0e7);
            ok_                   = lengthInSamples > 0;
        }

        ~MediaFoundationReader() override
        {
            if (reader_ != nullptr)
                reader_->Release();
        }

        bool isOk() const noexcept { return ok_; }

        bool readSamples(int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                         juce::int64 startSampleInFile, int numSamples) override
        {
            ensureCom();
            int written = 0;
            if (startSampleInFile != position_)
                seek(startSampleInFile);

            while (written < numSamples)
            {
                if (pendingAt_ >= pending_.size() / numChannels)
                    if (! decodeMore())
                        break;
                const auto available = (int) (pending_.size() / numChannels - pendingAt_);
                const int  n         = juce::jmin(available, numSamples - written);
                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (destChannels[ch] != nullptr)
                    {
                        auto*        out = reinterpret_cast<float*>(destChannels[ch]) + startOffsetInDestBuffer + written;
                        const size_t source = (size_t) juce::jmin(ch, (int) numChannels - 1);
                        for (int i = 0; i < n; ++i)
                            out[i] = pending_[(pendingAt_ + (size_t) i) * numChannels + source];
                    }
                pendingAt_ += (size_t) n;
                written += n;
                position_ += n;
            }

            // Past the end, or a decoder that stopped short: silence.
            for (int ch = 0; ch < numDestChannels; ++ch)
                if (destChannels[ch] != nullptr && written < numSamples)
                    std::fill_n(reinterpret_cast<float*>(destChannels[ch]) + startOffsetInDestBuffer + written,
                                numSamples - written, 0.0f);
            return true;
        }

    private:
        void seek(juce::int64 sample)
        {
            pending_.clear();
            pendingAt_ = 0;
            PROPVARIANT where;
            PropVariantInit(&where);
            // A little early: the decoder may land just after the time asked,
            // and what comes before the sample wanted is dropped anyway.
            const double seconds = juce::jmax(0.0, (double) sample / sampleRate - 0.2);
            where.vt             = VT_I8;
            where.hVal.QuadPart  = (LONGLONG) (seconds * 1.0e7);
            reader_->SetCurrentPosition(GUID_NULL, where);
            PropVariantClear(&where);
            position_ = sample;
        }

        /** The next buffer of samples, into pending_. False at the end. */
        bool decodeMore()
        {
            pending_.clear();
            pendingAt_ = 0;
            while (reader_ != nullptr)
            {
                DWORD     flags = 0;
                LONGLONG  time  = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader_->ReadSample((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &time, &sample)))
                    return false;
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 && sample == nullptr)
                    return false;
                if (sample == nullptr)
                    continue;

                IMFMediaBuffer* buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
                {
                    BYTE* data = nullptr;
                    DWORD bytes = 0;
                    if (SUCCEEDED(buffer->Lock(&data, nullptr, &bytes)))
                    {
                        const auto* floats = reinterpret_cast<const float*>(data);
                        const auto  count  = (size_t) bytes / sizeof(float);
                        // Placed by its own timestamp, whether reading on or
                        // just after a seek, so the two agree to the sample:
                        // what's before the next frame wanted is dropped, and
                        // a gap before it is silence.
                        const auto first = (juce::int64) std::llround((double) time / 1.0e7 * sampleRate);
                        if (first > position_)
                            pending_.assign((size_t) (first - position_) * numChannels, 0.0f);
                        const auto skip = position_ > first ? (size_t) (position_ - first) * numChannels : 0;
                        if (skip < count)
                            pending_.insert(pending_.end(), floats + skip, floats + count);
                        buffer->Unlock();
                    }
                    buffer->Release();
                }
                sample->Release();
                if (! pending_.empty())
                    return true;
            }
            return false;
        }

        IMFSourceReader*   reader_ = nullptr;
        bool               ok_     = false;
        std::vector<float> pending_;   // interleaved
        size_t             pendingAt_ = 0; // frames already handed out
        juce::int64        position_  = 0; // the next frame readSamples will hand out
    };

    class MediaFoundationFormat final : public juce::AudioFormat
    {
    public:
        MediaFoundationFormat()
            : juce::AudioFormat(MediaFoundationReader::kName, juce::StringArray { ".m4a", ".aac", ".mp4", ".m4v", ".mov", ".wma", ".wmv" })
        {
        }

        juce::Array<int> getPossibleSampleRates() override { return {}; }
        juce::Array<int> getPossibleBitDepths() override { return {}; }
        bool             canDoStereo() override { return true; }
        bool             canDoMono() override { return true; }

        juce::AudioFormatReader* createReaderFor(juce::InputStream* stream, bool deleteStreamIfOpeningFails) override
        {
            // Media Foundation opens files by name: only a file stream will do.
            if (auto* fileStream = dynamic_cast<juce::FileInputStream*>(stream))
            {
                auto reader = std::make_unique<MediaFoundationReader>(stream, fileStream->getFile());
                if (reader->isOk())
                    return reader.release();
                reader->input = nullptr; // not ours to delete when opening fails
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
#endif

void audioformats::registerAll(juce::AudioFormatManager& formats)
{
    formats.registerBasicFormats();
    formats.registerFormat(new OpusAudioFormat(), false);
    formats.registerFormat(new WavPackAudioFormat(), false);
    formats.registerFormat(new PcmContainerFormat("Wave64 file", { ".w64" }, pcmcontainer::parseWave64), false);
    formats.registerFormat(new PcmContainerFormat("RF64 file", { ".rf64", ".bw64" }, pcmcontainer::parseRf64), false);
    formats.registerFormat(new PcmContainerFormat("CAF file", { ".caf" }, pcmcontainer::parseCaf), false);
   #if JUCE_WINDOWS
    // Last, so the formats above keep their own files: Media Foundation can
    // read MP3 and WMA too, but JUCE's readers do those already.
    formats.registerFormat(new MediaFoundationFormat(), false);
   #endif
}

namespace audioformats
{
    bool writeWavPack(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate, int bits,
                      int level, bool dither, const ExportTags& tags, bool noiseShaping)
    {
        const int channels = audio.getNumChannels();
        const int frames   = audio.getNumSamples();
        if (channels <= 0 || frames <= 0 || sampleRate <= 0.0)
            return false;
        bits = bits == 16 ? 16 : bits == 32 ? 32 : 24;

        juce::MemoryOutputStream bytes;
        auto blockOut = [](void* id, void* data, int32_t count)
        {
            return static_cast<juce::MemoryOutputStream*>(id)->write(data, (size_t) count) ? 1 : 0;
        };

        auto* context = WavpackOpenFileOutput(blockOut, &bytes, nullptr);
        if (context == nullptr)
            return false;

        WavpackConfig config {};
        config.num_channels     = channels;
        config.channel_mask     = channels == 1 ? 4 : channels == 2 ? 3 : 0;
        config.sample_rate      = (int32_t) std::lround(sampleRate);
        config.bytes_per_sample = bits / 8;
        config.bits_per_sample  = bits;
        config.float_norm_exp   = bits == 32 ? 127 : 0;
        static constexpr int kLevelFlags[] { CONFIG_FAST_FLAG, 0, CONFIG_HIGH_FLAG, CONFIG_HIGH_FLAG | CONFIG_VERY_HIGH_FLAG };
        config.flags |= kLevelFlags[juce::jlimit(0, 3, level)];

        bool ok = WavpackSetConfiguration64(context, &config, frames, nullptr) && WavpackPackInit(context);

        // A block at a time, interleaved as WavPack takes it.
        constexpr int        kBlock = 8192;
        std::vector<int32_t> interleaved((size_t) kBlock * (size_t) channels);
        TpdfDither           ditherer(bits);
        std::vector<NoiseShapedDither> shapers;
        if (dither && noiseShaping && bits < 32)
            for (int ch = 0; ch < channels; ++ch)
                shapers.emplace_back(bits, sampleRate, 0x9E3779B9u + 0x1000193u * (uint32_t) ch);
        const double         scale = (double) ((int64_t) 1 << (bits - 1));
        for (int at = 0; ok && at < frames; at += kBlock)
        {
            const int n = juce::jmin(kBlock, frames - at);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < channels; ++ch)
                {
                    float x = audio.getSample(ch, at + i);
                    auto& out = interleaved[(size_t) (i * channels + ch)];
                    if (bits == 32)
                        std::memcpy(&out, &x, 4);
                    else
                    {
                        if (! shapers.empty())
                            x = shapers[(size_t) ch].processSample(x);
                        else if (dither)
                            x = ditherer.processSample(x);
                        out = (int32_t) juce::jlimit(-scale, scale - 1.0, std::round((double) x * scale));
                    }
                }
            ok = WavpackPackSamples(context, interleaved.data(), (uint32_t) n) != 0;
        }
        ok = ok && WavpackFlushSamples(context);

        // APEv2 tags, as WavPack players read them.
        if (ok && ! tags.empty())
        {
            const std::pair<const char*, const juce::String*> items[] {
                { "Title", &tags.title }, { "Artist", &tags.artist }, { "Album", &tags.album }, { "Year", &tags.year },
                { "Genre", &tags.genre }, { "Comment", &tags.comment }, { "Track", &tags.track },
            };
            for (const auto& [key, value] : items)
                if (value->isNotEmpty())
                    WavpackAppendTagItem(context, key, value->toRawUTF8(), (int) value->getNumBytesAsUTF8());

            juce::MemoryBlock image;
            if (tags.coverArt.existsAsFile() && tags.coverArt.loadFileAsData(image) && image.getSize() > 0)
            {
                juce::MemoryOutputStream item; // "name\0" then the image
                item.write(tags.coverArt.getFileName().toRawUTF8(), tags.coverArt.getFileName().getNumBytesAsUTF8() + 1);
                item.write(image.getData(), image.getSize());
                WavpackAppendBinaryTagItem(context, "Cover Art (Front)", static_cast<const char*>(item.getData()),
                                           (int) item.getDataSize());
            }
            ok = WavpackWriteTag(context) != 0;
        }
        WavpackCloseFile(context);

        return ok && file.replaceWithData(bytes.getData(), bytes.getDataSize());
    }

    bool writeOpus(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate, int bitrateKbps,
                   const ExportTags& tags)
    {
        const int sourceFrames = audio.getNumSamples();
        if (audio.getNumChannels() <= 0 || sourceFrames <= 0 || sampleRate <= 0.0)
            return false;
        const int channels = juce::jmin(2, audio.getNumChannels());

        // Opus runs at 48 kHz: anything else is resampled first.
        std::vector<std::vector<float>> pcm((size_t) channels);
        for (int ch = 0; ch < channels; ++ch)
        {
            pcm[(size_t) ch].assign(audio.getReadPointer(ch), audio.getReadPointer(ch) + sourceFrames);
            if (std::abs(sampleRate - 48000.0) > 0.5)
                pcm[(size_t) ch] = Resampler(sampleRate, 48000.0).processAll(pcm[(size_t) ch]);
        }
        const int frames = (int) pcm[0].size();

        int  error   = 0;
        auto encoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &error);
        if (encoder == nullptr)
            return false;
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(juce::jlimit(16, 512, bitrateKbps) * 1000));
        opus_int32 preSkip = 0;
        opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&preSkip));

        juce::MemoryOutputStream bytes;
        ogg_stream_state         stream;
        ogg_stream_init(&stream, (int) juce::Random::getSystemRandom().nextInt());

        const auto writePages = [&](bool flush)
        {
            ogg_page page;
            while ((flush ? ogg_stream_flush(&stream, &page) : ogg_stream_pageout(&stream, &page)) != 0)
            {
                bytes.write(page.header, (size_t) page.header_len);
                bytes.write(page.body, (size_t) page.body_len);
            }
        };
        const auto submit = [&](const void* data, size_t size, ogg_int64_t granule, bool bos, bool eos, ogg_int64_t number)
        {
            ogg_packet packet {};
            packet.packet     = static_cast<unsigned char*>(const_cast<void*>(data));
            packet.bytes      = (long) size;
            packet.b_o_s      = bos ? 1 : 0;
            packet.e_o_s      = eos ? 1 : 0;
            packet.granulepos = granule;
            packet.packetno   = number;
            ogg_stream_packetin(&stream, &packet);
        };

        // OpusHead: version, channels, pre-skip, the original rate, no gain,
        // mapping family 0.
        {
            juce::MemoryOutputStream head;
            head.write("OpusHead", 8);
            head.writeByte(1);
            head.writeByte((char) channels);
            head.writeShort((short) preSkip);
            head.writeInt((int) std::lround(sampleRate));
            head.writeShort(0);
            head.writeByte(0);
            submit(head.getData(), head.getDataSize(), 0, true, false, 0);
            writePages(true);
        }

        // OpusTags: Vorbis comments, and the cover as METADATA_BLOCK_PICTURE.
        {
            auto comments = tags::vorbisComments(tags);
            if (const auto picture = tags::pictureBlock(tags); picture.getSize() > 0)
                comments.add("METADATA_BLOCK_PICTURE=" + juce::Base64::toBase64(picture.getData(), picture.getSize()));
            juce::MemoryOutputStream tagPacket;
            tagPacket.write("OpusTags", 8);
            const juce::String vendor = juce::String("SoundSplice (") + opus_get_version_string() + ")";
            tagPacket.writeInt((int) vendor.getNumBytesAsUTF8());
            tagPacket.write(vendor.toRawUTF8(), vendor.getNumBytesAsUTF8());
            tagPacket.writeInt(comments.size());
            for (const auto& comment : comments)
            {
                tagPacket.writeInt((int) comment.getNumBytesAsUTF8());
                tagPacket.write(comment.toRawUTF8(), comment.getNumBytesAsUTF8());
            }
            submit(tagPacket.getData(), tagPacket.getDataSize(), 0, false, false, 1);
            writePages(true);
        }

        constexpr int              packetFrames = 960; // 20 ms
        std::vector<float>         block((size_t) (packetFrames * channels));
        std::vector<unsigned char> encoded(4000);
        ogg_int64_t                number = 2;
        bool                       ok     = true;

        // Encoded past the end, to flush the encoder's lookahead out.
        for (int at = 0; ok && at < frames + preSkip; at += packetFrames, ++number)
        {
            for (int n = 0; n < packetFrames; ++n)
                for (int ch = 0; ch < channels; ++ch)
                    block[(size_t) (n * channels + ch)] = at + n < frames ? pcm[(size_t) ch][(size_t) (at + n)] : 0.0f;

            const int size = opus_encode_float(encoder, block.data(), packetFrames, encoded.data(), (opus_int32) encoded.size());
            if (size <= 0)
            {
                ok = false;
                break;
            }
            const bool last = at + packetFrames >= frames + preSkip;
            // A granule position counts decoded samples, pre-skip included;
            // the last one says where the audio really ends.
            submit(encoded.data(), (size_t) size, last ? preSkip + frames : at + packetFrames, false, last, number);
            writePages(false);
        }
        writePages(true);

        ogg_stream_clear(&stream);
        opus_encoder_destroy(encoder);
        return ok && file.replaceWithData(bytes.getData(), bytes.getDataSize());
    }
}

} // namespace soundsplice::engine
