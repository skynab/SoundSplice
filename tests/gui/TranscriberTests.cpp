#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <engine/Transcriber.h>

#include <vector>

using namespace soundsplice;

TEST_CASE("Transcription says why when it can't run", "[gui][transcribe]")
{
    const std::vector<float> second(16000, 0.0f);

    auto result = engine::transcribe(juce::File::getCurrentWorkingDirectory().getChildFile("no such model.bin"), second, 16000.0);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("No transcription model") != std::string::npos);

    // Something that isn't a model at all.
    const juce::TemporaryFile notAModel(".bin");
    REQUIRE(notAModel.getFile().replaceWithText("this is not a ggml model"));
    result = engine::transcribe(notAModel.getFile(), second, 16000.0);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("isn't a whisper.cpp model") != std::string::npos);

    result = engine::transcribe(notAModel.getFile(), {}, 16000.0);
    REQUIRE_FALSE(result.ok);
}

/** Run with SOUNDSPLICE_WHISPER_MODEL set to a ggml model and
    SOUNDSPLICE_WHISPER_SAMPLE to whisper.cpp's samples/jfk.wav: the real
    thing, word by word. Skipped without them - a model is tens of megabytes. */
TEST_CASE("A real model transcribes speech word by word", "[gui][transcribe][.model]")
{
    const juce::File model(juce::SystemStats::getEnvironmentVariable("SOUNDSPLICE_WHISPER_MODEL", {}));
    const juce::File sample(juce::SystemStats::getEnvironmentVariable("SOUNDSPLICE_WHISPER_SAMPLE", {}));
    if (! model.existsAsFile() || ! sample.existsAsFile())
        SKIP("set SOUNDSPLICE_WHISPER_MODEL and SOUNDSPLICE_WHISPER_SAMPLE");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sample));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> audio(1, (int) reader->lengthInSamples);
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, false);
    const std::vector<float> mono(audio.getReadPointer(0), audio.getReadPointer(0) + audio.getNumSamples());

    int    reports = 0;
    double last    = -1.0;
    const auto result = engine::transcribe(model, mono, reader->sampleRate, "en", [&](double p)
    {
        ++reports;
        last = p;
        return true;
    });
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.words.size() > 10);

    juce::String text;
    for (const auto& word : result.words)
        text << word.text << " ";
    INFO(text);
    REQUIRE(text.toLowerCase().contains("country"));

    // In order, inside the audio, each with a confidence.
    const double seconds = mono.size() / reader->sampleRate;
    for (size_t i = 0; i < result.words.size(); ++i)
    {
        const auto& w = result.words[i];
        REQUIRE(w.start <= w.end);
        REQUIRE(w.end <= seconds + 0.5);
        REQUIRE((w.confidence >= 0.0f && w.confidence <= 1.0f));
        if (i > 0)
            REQUIRE(w.start >= result.words[i - 1].start - 1e-9);
    }
    REQUIRE(reports > 0);

    // Stopping is honoured.
    const auto stopped = engine::transcribe(model, mono, reader->sampleRate, "en", [](double) { return false; });
    REQUIRE_FALSE(stopped.ok);
}
