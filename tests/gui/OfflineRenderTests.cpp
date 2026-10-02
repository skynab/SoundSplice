#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/AudioEngine.h"
#include "engine/AudioExport.h"
#include "model/EffectParams.h"

#include <cmath>

using namespace soundsplice;
using Catch::Matchers::WithinAbs;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** A 4-second 440 Hz sine at half scale, as a WAV in a temporary file. */
    juce::File writeSine(const juce::TemporaryFile& temp)
    {
        constexpr double rate = 44100.0;
        juce::AudioBuffer<float> sine(2, (int) (rate * 4.0));
        for (int i = 0; i < sine.getNumSamples(); ++i)
        {
            const auto v = 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / rate);
            sine.setSample(0, i, v);
            sine.setSample(1, i, v);
        }
        engine::ExportOptions options;
        options.sampleRate    = rate;
        options.bitsPerSample = 32;
        REQUIRE(engine::writeAudioFile(temp.getFile(), sine, options));
        return temp.getFile();
    }

    /** One track playing @p file for 8 beats (4 s at 120). */
    void playFile(engine::AudioEngine& engine, const juce::File& file)
    {
        engine::AudioClipSpec clip;
        clip.file        = file;
        clip.lengthBeats = 8.0;
        clip.clipId      = 1;
        REQUIRE(engine.setTrackAudioClips(0, { clip }));
        engine.setActiveTrackCount(1);
    }

    /** The loudest sample in the middle of @p buffer, clear of any edges. */
    float middlePeak(const juce::AudioBuffer<float>& buffer)
    {
        const int from = buffer.getNumSamples() / 4, count = buffer.getNumSamples() / 4;
        return buffer.getMagnitude(0, from, count);
    }
}

TEST_CASE("An offline render runs a track's effects as they're set", "[gui][render]")
{
    // A render rebuilds every chain for its rate, from the chain's shape.
    // The settings went with the old nodes, so an export ran every track
    // effect at its defaults - which for most is off - while playback,
    // resent the settings on the next edit, sounded right.
    JuceFixture              fixture;
    const juce::TemporaryFile temp(".wav");
    engine::AudioEngine      engine(false); // no device: renders still work
    playFile(engine, writeSine(temp));

    auto amplify    = model::makeEffectSlot(model::EffectKind::Amplify);
    amplify.enabled = true;
    amplify.amplify.gainDb = -12.0f;
    engine::EffectSlotSpec shape;
    shape.kind = amplify.kind;
    engine.setTrackEffectChain(0, { shape });
    engine.setTrackEffectSlotParams(0, 0, model::effectParamValues(amplify));

    engine::AudioEngine::OfflineRenderOptions options;
    options.lengthBeats = 8.0;
    options.sampleRate  = 44100.0;
    const auto rendered = engine.renderOffline(options);

    REQUIRE(rendered.getNumSamples() > 0);
    REQUIRE_THAT(middlePeak(rendered), WithinAbs(0.5f * juce::Decibels::decibelsToGain(-12.0f), 0.01));

    // And again: the render's own rebuild on the way out kept them too.
    REQUIRE_THAT(middlePeak(engine.renderOffline(options)), WithinAbs(0.5f * juce::Decibels::decibelsToGain(-12.0f), 0.01));
}

TEST_CASE("An offline render is as long at any rate", "[gui][render]")
{
    // Its length was worked out from the tempo map before the map was set to
    // the render's rate, so a render at another rate than the last came out
    // too short or too long.
    JuceFixture              fixture;
    const juce::TemporaryFile temp(".wav");
    engine::AudioEngine      engine(false);
    playFile(engine, writeSine(temp));

    engine::AudioEngine::OfflineRenderOptions options;
    options.lengthBeats = 8.0; // 4 s at 120

    for (const double rate : { 44100.0, 48000.0, 96000.0, 44100.0 })
    {
        INFO(rate);
        options.sampleRate  = rate;
        const auto rendered = engine.renderOffline(options);
        REQUIRE(rendered.getNumSamples() == (int) std::lround(rate * 4.0));
        REQUIRE_THAT(middlePeak(rendered), WithinAbs(0.5, 0.01));
    }
}
