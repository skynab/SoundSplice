#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <engine/AudioFormats.h>

#include <cmath>
#include <vector>

#if JUCE_WINDOWS
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
#endif

using namespace soundsplice;

#if JUCE_WINDOWS
namespace
{
    /** An M4A (AAC, 44.1 kHz stereo) of @p seconds of a 440 Hz sine on the left
        and 660 Hz on the right, made with Windows' own AAC encoder. */
    bool writeM4a(const juce::File& file, double seconds)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
            return false;

        constexpr UINT32 rate = 44100, channels = 2;
        IMFSinkWriter* writer = nullptr;
        if (FAILED(MFCreateSinkWriterFromURL(file.getFullPathName().toWideCharPointer(), nullptr, nullptr, &writer)))
            return false;

        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbps
        DWORD stream = 0;
        const bool added = SUCCEEDED(writer->AddStream(out, &stream));
        out->Release();

        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
        const bool typed = added && SUCCEEDED(writer->SetInputMediaType(stream, in, nullptr));
        in->Release();
        if (! typed || FAILED(writer->BeginWriting()))
        {
            writer->Release();
            return false;
        }

        const int frames = (int) (seconds * rate);
        constexpr int block = 4410;
        for (int at = 0; at < frames; at += block)
        {
            const int n = juce::jmin(block, frames - at);
            IMFMediaBuffer* buffer = nullptr;
            MFCreateMemoryBuffer((DWORD) (n * channels * 2), &buffer);
            BYTE* data = nullptr;
            buffer->Lock(&data, nullptr, nullptr);
            auto* pcm = reinterpret_cast<int16_t*>(data);
            for (int i = 0; i < n; ++i)
            {
                const double t = (at + i) / (double) rate;
                pcm[i * 2]     = (int16_t) (12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
                pcm[i * 2 + 1] = (int16_t) (12000.0 * std::sin(2.0 * 3.14159265358979 * 660.0 * t));
            }
            buffer->Unlock();
            buffer->SetCurrentLength((DWORD) (n * channels * 2));
            IMFSample* sample = nullptr;
            MFCreateSample(&sample);
            sample->AddBuffer(buffer);
            sample->SetSampleTime((LONGLONG) (at * 1.0e7 / rate));
            sample->SetSampleDuration((LONGLONG) (n * 1.0e7 / rate));
            writer->WriteSample(stream, sample);
            sample->Release();
            buffer->Release();
        }
        const bool ok = SUCCEEDED(writer->Finalize());
        writer->Release();
        return ok;
    }

    /** The strength of @p hz in @p x (a Goertzel), normalised. */
    double strength(const float* x, int n, double hz, double rate)
    {
        const double w = 2.0 * 3.14159265358979 * hz / rate, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double s = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        return std::sqrt(juce::jmax(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n;
    }
}

TEST_CASE("M4A files import through the system's decoder, and seek exactly", "[gui][mediafoundation]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    const juce::TemporaryFile temp(".m4a");
    if (! writeM4a(temp.getFile(), 3.0))
        SKIP("this Windows has no AAC encoder to make a test file with");

    juce::AudioFormatManager formats;
    engine::audioformats::registerAll(formats);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(temp.getFile()));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->getFormatName() == "Media Foundation file");
    REQUIRE(reader->sampleRate == 44100.0);
    REQUIRE(reader->numChannels == 2);
    REQUIRE(std::abs((double) reader->lengthInSamples - 3.0 * 44100.0) < 4410.0); // AAC pads a little

    const int frames = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> all(2, frames);
    REQUIRE(reader->read(&all, 0, frames, 0, true, true));

    // The tones are where they should be: 440 Hz left, 660 Hz right.
    const int from = 44100, count = 22050;
    REQUIRE(strength(all.getReadPointer(0, from), count, 440.0, 44100.0) > 0.1);
    REQUIRE(strength(all.getReadPointer(0, from), count, 660.0, 44100.0) < 0.02);
    REQUIRE(strength(all.getReadPointer(1, from), count, 660.0, 44100.0) > 0.1);

    // A read in the middle - a seek - gives exactly what reading through did.
    juce::AudioBuffer<float> middle(2, 1000);
    REQUIRE(reader->read(&middle, 0, 1000, 70000, true, true));
    for (int i = 0; i < 1000; i += 7)
        REQUIRE(middle.getSample(0, i) == all.getSample(0, 70000 + i));
    // And back to the start again.
    juce::AudioBuffer<float> start(2, 500);
    REQUIRE(reader->read(&start, 0, 500, 100, true, true));
    for (int i = 0; i < 500; i += 11)
        REQUIRE(start.getSample(1, i) == all.getSample(1, 100 + i));
}
#endif
