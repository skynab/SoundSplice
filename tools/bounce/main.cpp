#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/ClipData.h"
#include "engine/EffectChain.h"
#include "engine/MixerTrack.h"
#include "engine/OfflineRenderer.h"
#include "engine/PluginHost.h"
#include "engine/PluginNode.h"

/*
    soundsplice-bounce: renders a generated tone through a track to a WAV
    with no audio device, and exits non-zero if it's silent - a smoke test of
    the clip player and track path that CI runs on every platform (CTest's
    bounce_smoke).

    It also hosts a real plugin, if the machine has one, since that needs the
    plugin-host build flags this tool has and the test binaries don't. The
    engine's other end-to-end checks are Catch2 tests now, in
    tests/gui/RenderChecksTests.cpp.

    Usage: soundsplice-bounce [out.wav]
*/

using namespace soundsplice::engine;

namespace
{
    constexpr double kBpm        = 120.0;
    constexpr double kSampleRate = 44100.0;
    constexpr int    kBlock      = 512;

    /** @p seconds of a stereo tone stepping up an arpeggio, a half-second a
        note, as a clip ready to play. */
    ClipData arpeggio(double seconds)
    {
        ClipData clip;
        clip.sourceSampleRate = kSampleRate;
        clip.numChannels      = 2;
        clip.lengthSamples    = (int) (seconds * kSampleRate);
        clip.audio.setSize(2, clip.lengthSamples);

        const int    noteSamples = (int) (0.5 * kSampleRate);
        const double ratios[]    = { 1.0, 1.25, 1.5, 2.0 };
        double       phase       = 0.0;
        for (int i = 0; i < clip.lengthSamples; ++i)
        {
            const double hz = 261.63 * ratios[(i / noteSamples) % 4];
            phase += 2.0 * juce::MathConstants<double>::pi * hz / kSampleRate;
            const float value = 0.3f * (float) std::sin(phase);
            clip.audio.setSample(0, i, value);
            clip.audio.setSample(1, i, value);
        }
        return clip;
    }

    /** A second of the arpeggio through one track, through @p plugin if
        there is one. */
    juce::AudioBuffer<float> renderThrough(std::unique_ptr<juce::AudioPluginInstance> plugin, bool bypassed)
    {
        const int                totalSamples = (int) kSampleRate;
        juce::AudioBuffer<float> mix(2, totalSamples);
        mix.clear();

        MixerTrack track;
        track.prepare(kSampleRate, kBlock);
        if (plugin != nullptr)
        {
            auto chain = std::make_unique<EffectChain>();
            auto node  = std::make_unique<PluginNode>(std::move(plugin));
            node->setBypassed(bypassed);
            chain->add(std::move(node));
            chain->prepare(kSampleRate, kBlock);
            track.setEffectChain(chain.release());
        }

        track.audioPlayer.submitSingleClip(new ClipData(arpeggio(1.0)));

        for (int pos = 0; pos < totalSamples; pos += kBlock)
        {
            const int      n = std::min(kBlock, totalSamples - pos);
            ProcessContext context;
            context.sampleRate        = kSampleRate;
            context.numSamples        = n;
            context.transport.playing = true;
            OfflineRenderer::fillTransport(context, pos, n, kBpm, kSampleRate);
            context.transport.timeSigNumerator   = 4;
            context.transport.timeSigDenominator = 4;

            juce::AudioBuffer<float> blockView(mix.getArrayOfWritePointers(), 2, pos, n);
            track.render(blockView, context, false);
        }
        return mix;
    }

    /** Scans this machine's effect plugins, hosts the first stereo one as a
        chain node, and requires it to run cleanly: every sample finite, and
        bypassed, identical to no plugin at all. True with none to host - a
        machine without plugins isn't this code's fault - and it says so. */
    bool pluginHostingWorks()
    {
        const juce::ScopedJuceInitialiser_GUI juce; // plugin formats want a message loop

        PluginHost host;
        const auto deadMansPedal = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("soundsplice_bounce_plugin_scan.tmp");
        for (const auto& format : host.availableFormats())
            host.scanFormat(format, deadMansPedal, 24); // capped: probing instantiates each one
        deadMansPedal.deleteFile();

        const auto entries = host.knownPlugins();
        const auto chosen  = std::find_if(entries.begin(), entries.end(), [](const PluginEntry& entry)
        {
            return ! entry.isInstrument && entry.numInputs >= 2 && entry.numOutputs >= 2;
        });
        if (chosen == entries.end())
        {
            std::cout << "plugin hosting: no stereo effect among " << entries.size() << " plugins, not checked\n";
            return true;
        }

        std::string error;
        auto        instance = host.createInstance(chosen->format, chosen->identifier, kSampleRate, kBlock, &error);
        if (instance == nullptr)
        {
            std::cerr << "plugin hosting: " << chosen->name << " would not load: " << error << "\n";
            return false;
        }

        // At its defaults a plugin may well be transparent, so what's asserted
        // is that it ran: no NaN or overrun poisoning the mix. Bypass, unlike
        // the plugin's sound, is deterministic - it's PluginNode's own path.
        const auto hosted   = renderThrough(std::move(instance), false);
        const auto bypassed = renderThrough(host.createInstance(chosen->format, chosen->identifier, kSampleRate, kBlock), true);
        const auto none     = renderThrough(nullptr, false);

        bool  finite = true;
        float peak   = 0.0f;
        for (int ch = 0; ch < hosted.getNumChannels(); ++ch)
            for (int i = 0; i < hosted.getNumSamples(); ++i)
            {
                finite &= std::isfinite(hosted.getSample(ch, i));
                peak = std::max(peak, std::abs(hosted.getSample(ch, i)));
            }

        float bypassDelta = 0.0f;
        for (int i = 0; i < none.getNumSamples(); ++i)
            bypassDelta = std::max(bypassDelta, std::abs(bypassed.getSample(0, i) - none.getSample(0, i)));

        std::cout << "plugin hosting: " << chosen->name << " (" << chosen->format << "), peak=" << peak
                  << ", bypassDelta=" << bypassDelta << "\n";
        return finite && peak > 0.0f && bypassDelta < 1.0e-9f;
    }
}

int main(int argc, char** argv)
{
    const auto mix = OfflineRenderer::renderAudioClip(arpeggio(4.0), 0.0, 0.0f, kBpm, kSampleRate, 4.0);
    const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(argc > 1 ? argv[1] : "bounce.wav");
    if (! OfflineRenderer::writeWav(out, mix, kSampleRate))
    {
        std::cerr << "Failed to write " << out.getFullPathName() << "\n";
        return 1;
    }

    const float rms = mix.getRMSLevel(0, 0, mix.getNumSamples());
    std::cout << "wrote " << out.getFullPathName() << "  frames=" << mix.getNumSamples() << "  rms=" << rms << "\n";

    const bool sounds = rms > 0.0f && std::isfinite(rms);
    if (! sounds)
        std::cerr << "the render is silent\n";

    const bool hosts = pluginHostingWorks();
    return sounds && hosts ? 0 : 2;
}
