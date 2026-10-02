// soundsplice-cli: SoundSplice from the command line.
//
//   soundsplice-cli convert <in> <out> [--rate HZ] [--bits 16|24|32] [--quality N] [--no-dither]
//   soundsplice-cli analyze <file>...
//   soundsplice-cli apply <macro> <file or folder>... --out <folder> [--loudness LUFS] [--bits N]
//   soundsplice-cli macros
//   soundsplice-cli render <project> <out> [--rate HZ] [--bits N] [--stems | --stems-only] [--app PATH]
//
// Everything here goes through the app's own code: the same readers and
// writers, the same effect renderer and loudness meter, the macros the app
// saved. `render` doesn't mix anything itself - it runs the app with
// --render, so a project renders exactly as File > Export Audio would.

#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "app/BatchProcess.h"
#include "app/Macros.h"
#include "app/SettingsLocation.h"
#include "engine/AmplitudeAnalysis.h"
#include "engine/AudioExport.h"
#include "engine/AudioFormats.h"
#include "engine/Loudness.h"
#include "engine/Resample.h"

using namespace soundsplice;

namespace
{
constexpr int kOk = 0, kFailed = 1, kUsage = 2;

const char* const kUsageText =
    "soundsplice-cli - SoundSplice from the command line\n"
    "\n"
    "  convert <in> <out> [--rate HZ] [--bits 16|24|32] [--quality N] [--no-dither]\n"
    "      Write an audio file in another format (from <out>'s extension: wav, aiff,\n"
    "      flac, ogg, mp3), sample rate or bit depth.\n"
    "  analyze <file>...\n"
    "      Loudness (EBU R128), true peak, peak, RMS, DC offset and dynamic range.\n"
    "  apply <macro> <file or folder>... --out <folder> [--loudness LUFS] [--bits N]\n"
    "      Run a macro of effects (saved in the app) over files, writing new ones.\n"
    "      --macro-file <xml> instead of <macro> reads macros from a file.\n"
    "  macros\n"
    "      List the macros saved in the app.\n"
    "  render <project> <out> [--rate HZ] [--bits N] [--stems | --stems-only] [--app PATH]\n"
    "      Render a project as File > Export Audio does, by running the app with\n"
    "      no window. --app (or SOUNDSPLICE_APP) says where it is if it isn't\n"
    "      installed beside this tool.\n";

/** The command line, with --name value options taken out as they're asked for. */
struct Args
{
    juce::StringArray words;

    std::optional<juce::String> take(const juce::String& name)
    {
        const int at = words.indexOf(name);
        if (at < 0 || at + 1 >= words.size())
            return std::nullopt;
        const auto value = words[at + 1];
        words.removeRange(at, 2);
        return value;
    }

    bool flag(const juce::String& name)
    {
        const int at = words.indexOf(name);
        if (at < 0)
            return false;
        words.remove(at);
        return true;
    }
};

int fail(const juce::String& message)
{
    std::cerr << "soundsplice-cli: " << message << std::endl;
    return kFailed;
}

juce::String db(double value)
{
    return std::isfinite(value) ? juce::String(value, 1) : juce::String("-inf");
}

/** Reads @p file whole. */
bool readWhole(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error)
{
    juce::AudioFormatManager formats;
    engine::audioformats::registerAll(formats);
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
    {
        error = file.getFullPathName() + " can't be read as audio";
        return false;
    }
    if (reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        error = file.getFullPathName() + " is too long to read in one piece";
        return false;
    }
    audio.setSize((int) juce::jlimit(1u, 8u, reader->numChannels), (int) reader->lengthInSamples);
    sampleRate = reader->sampleRate;
    if (! reader->read(&audio, 0, audio.getNumSamples(), 0, true, true))
    {
        error = file.getFullPathName() + " can't be read";
        return false;
    }
    return true;
}

std::optional<engine::ExportFormat> formatFor(const juce::File& file)
{
    for (int i = 0; i < engine::kNumExportFormats; ++i)
        if (file.hasFileExtension(engine::extensionFor((engine::ExportFormat) i)))
            return (engine::ExportFormat) i;
    if (file.hasFileExtension("aif"))
        return engine::ExportFormat::Aiff;
    return std::nullopt;
}

bool bitsOption(Args& args, engine::ExportOptions& options, juce::String& error)
{
    if (const auto bits = args.take("--bits"))
    {
        const int value = bits->getIntValue();
        if (value != 16 && value != 24 && value != 32)
        {
            error = "--bits is 16, 24 or 32";
            return false;
        }
        options.bitsPerSample = value;
    }
    return true;
}

// ---- convert -----------------------------------------------------------------

int convert(Args args)
{
    engine::ExportOptions options;
    juce::String          error;
    if (! bitsOption(args, options, error))
        return fail(error);
    const auto rate    = args.take("--rate");
    const auto quality = args.take("--quality");
    options.dither     = ! args.flag("--no-dither");
    if (args.words.size() != 2)
        return kUsage;

    const juce::File in(juce::File::getCurrentWorkingDirectory().getChildFile(args.words[0]));
    const juce::File out(juce::File::getCurrentWorkingDirectory().getChildFile(args.words[1]));
    const auto       format = formatFor(out);
    if (! format)
        return fail("can't tell the format from " + out.getFileName() + ": use .wav, .aiff, .flac, .ogg or .mp3");
    options.format = *format;
    if (quality)
        options.qualityIndex = quality->getIntValue();

    juce::AudioBuffer<float> audio;
    double                   sourceRate = 0.0;
    if (! readWhole(in, audio, sourceRate, error))
        return fail(error);

    options.sampleRate = rate ? rate->getDoubleValue() : sourceRate;
    if (options.sampleRate < 8000.0 || options.sampleRate > 384000.0)
        return fail("--rate is between 8000 and 384000");

    // The offline resampler (a windowed sinc), as Resample Track uses.
    if (std::abs(options.sampleRate - sourceRate) > 0.5)
    {
        const engine::Resampler resampler(sourceRate, options.sampleRate);
        juce::AudioBuffer<float> converted(audio.getNumChannels(),
                                           (int) resampler.outputLength(audio.getNumSamples()));
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const std::vector<float> input(audio.getReadPointer(ch), audio.getReadPointer(ch) + audio.getNumSamples());
            const auto output = resampler.processAll(input);
            converted.copyFrom(ch, 0, output.data(), juce::jmin((int) output.size(), converted.getNumSamples()));
        }
        audio = std::move(converted);
    }

    if (! engine::writeAudioFile(out, audio, options))
        return fail("couldn't write " + out.getFullPathName());
    std::cout << "Wrote " << out.getFullPathName() << std::endl;
    return kOk;
}

// ---- analyze -----------------------------------------------------------------

int analyze(Args args)
{
    if (args.words.isEmpty())
        return kUsage;

    int result = kOk;
    for (const auto& name : args.words)
    {
        const juce::File         file(juce::File::getCurrentWorkingDirectory().getChildFile(name));
        juce::AudioBuffer<float> audio;
        double                   sampleRate = 0.0;
        juce::String             error;
        if (! readWhole(file, audio, sampleRate, error))
        {
            fail(error);
            result = kFailed;
            continue;
        }

        const int channels = audio.getNumChannels();
        const int frames   = audio.getNumSamples();

        engine::LoudnessMeter meter;
        meter.prepare(sampleRate, 2);
        const float* both[2] { audio.getReadPointer(0), audio.getReadPointer(juce::jmin(1, channels - 1)) };
        meter.process(both, 2, frames);
        const auto loudness = engine::LoudnessReport::of(meter, frames / sampleRate);

        engine::AmplitudeStatistics statistics;
        statistics.prepare(sampleRate, channels);
        statistics.process(audio.getArrayOfReadPointers(), channels, frames);
        const auto amplitude = statistics.report();

        std::cout << file.getFileName() << "\n"
                  << "  length          " << juce::String(frames / sampleRate, 3) << " s, "
                  << juce::String(sampleRate, 0) << " Hz, " << channels << (channels == 1 ? " channel" : " channels") << "\n"
                  << "  integrated      " << db(loudness.integratedLufs) << " LUFS\n"
                  << "  loudness range  " << juce::String(loudness.loudnessRangeLu, 1) << " LU\n"
                  << "  short-term max  " << db(loudness.maxShortTermLufs) << " LUFS\n"
                  << "  true peak       " << db(loudness.truePeakDb) << " dBTP\n"
                  << "  peak            " << db(amplitude.peakDb) << " dBFS\n"
                  << "  RMS             " << db(amplitude.rmsDb) << " dBFS\n"
                  << "  DC offset       " << juce::String(amplitude.dcOffsetPercent, 3) << " %\n"
                  << "  dynamic range   " << juce::String(amplitude.dynamicRangeDb, 1) << " dB\n";
    }
    std::cout.flush();
    return result;
}

// ---- macros and apply ----------------------------------------------------------

std::vector<macros::Macro> savedMacros()
{
    juce::PropertiesFile settings(app::settingsOptions());
    return macros::deserialize(settings.getValue("macros"));
}

int listMacros()
{
    const auto list = savedMacros();
    if (list.empty())
        std::cout << "No macros saved. Make one in SoundSplice: Tools > Record Macro or Tools > Macros.\n";
    for (const auto& macro : list)
    {
        std::cout << macro.name << "  (" << macro.steps.size() << (macro.steps.size() == 1 ? " step" : " steps")
                  << (macros::effectsOnly(macro) ? "" : "; has commands, so it runs in the app only") << ")\n";
        for (const auto& step : macro.steps)
            std::cout << "    " << macros::describe(step) << "\n";
    }
    std::cout.flush();
    return kOk;
}

int apply(Args args)
{
    const auto out      = args.take("--out");
    const auto file     = args.take("--macro-file");
    const auto loudness = args.take("--loudness");
    batch::Settings settings;
    juce::String    error;
    if (! bitsOption(args, settings.output, error))
        return fail(error);
    if (! out || args.words.size() < (file ? 1 : 2))
        return kUsage;

    std::vector<macros::Macro> available;
    juce::String               name;
    if (file)
    {
        available = macros::deserialize(juce::File::getCurrentWorkingDirectory().getChildFile(*file).loadFileAsString());
        if (available.empty())
            return fail(*file + " holds no macros");
        // The first, unless a name is given before the files.
        name = available.front().name;
        for (const auto& macro : available)
            if (juce::String(macro.name) == args.words[0])
            {
                name = args.words[0];
                args.words.remove(0);
                break;
            }
    }
    else
    {
        available = savedMacros();
        name      = args.words[0];
        args.words.remove(0);
    }

    const macros::Macro* macro = nullptr;
    for (const auto& candidate : available)
        if (juce::String(candidate.name) == name)
            macro = &candidate;
    if (macro == nullptr)
        return fail("no macro called \"" + name + "\" - soundsplice-cli macros lists them");
    const auto chain = macros::effectsOnly(*macro);
    if (! chain)
        return fail("\"" + name + "\" has command steps, which act on a project: only a macro of effects can run over files");

    settings.chain        = *chain;
    settings.loudnessLufs = loudness ? loudness->getDoubleValue() : 0.0;
    if (settings.loudnessLufs > 0.0)
        return fail("--loudness is a negative LUFS target, like -16");
    settings.folder = juce::File::getCurrentWorkingDirectory().getChildFile(*out);
    if (! settings.folder.createDirectory())
        return fail("can't make the folder " + settings.folder.getFullPathName());

    std::vector<juce::File> inputs;
    for (const auto& word : args.words)
    {
        const auto input = juce::File::getCurrentWorkingDirectory().getChildFile(word);
        if (input.isDirectory())
            for (const auto& found : batch::audioFilesIn(input))
                inputs.push_back(found);
        else
            inputs.push_back(input);
    }
    if (inputs.empty())
        return fail("no audio files to process");

    engine::PluginHost plugins;
    int failures = 0;
    for (const auto& input : inputs)
    {
        const auto result = batch::processFile(input, settings, plugins);
        if (result.ok)
            std::cout << "Wrote " << result.written.getFullPathName() << "\n";
        else
        {
            std::cerr << input.getFileName() << " " << result.error << "\n";
            ++failures;
        }
    }
    std::cout << inputs.size() - (size_t) failures << " of " << inputs.size() << " files processed" << std::endl;
    return failures == 0 ? kOk : kFailed;
}

// ---- render --------------------------------------------------------------------

/** The SoundSplice app: --app, SOUNDSPLICE_APP, beside this tool, or where
    a build tree puts it. */
juce::File findApp(const std::optional<juce::String>& given)
{
    if (given)
        return juce::File::getCurrentWorkingDirectory().getChildFile(*given);
    if (const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable("SOUNDSPLICE_APP", {}); fromEnvironment.isNotEmpty())
        return juce::File(fromEnvironment);

    const auto here = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
   #if JUCE_WINDOWS
    const juce::String exe = "SoundSplice.exe";
   #else
    const juce::String exe = "SoundSplice";
   #endif
    const juce::File candidates[] {
        here.getChildFile(exe),
        here.getChildFile("SoundSplice.app/Contents/MacOS/SoundSplice"),
        here.getChildFile("../SoundSplice.app/Contents/MacOS/SoundSplice"),
        // A build tree: tools/cli/<target>_artefacts/<config>/ beside src/app/SoundSplice_artefacts/<config>/.
        here.getChildFile("../../../../src/app/SoundSplice_artefacts").getChildFile(here.getFileName()).getChildFile(exe),
        here.getChildFile("../../../../src/app/SoundSplice_artefacts").getChildFile(here.getFileName())
            .getChildFile("SoundSplice.app/Contents/MacOS/SoundSplice"),
    };
    for (const auto& candidate : candidates)
        if (candidate.existsAsFile())
            return candidate;
    return {};
}

int render(Args args)
{
    const auto app  = findApp(args.take("--app"));
    const auto rate = args.take("--rate");
    engine::ExportOptions options;
    juce::String          error;
    if (! bitsOption(args, options, error))
        return fail(error);
    const int contents = args.flag("--stems-only") ? (int) engine::ExportContents::StemsOnly
                       : args.flag("--stems")      ? (int) engine::ExportContents::MasterMixAndStems
                                                   : (int) engine::ExportContents::MasterMix;
    if (args.words.size() != 2)
        return kUsage;

    if (! app.existsAsFile())
        return fail("can't find the SoundSplice app to render with - say where with --app or SOUNDSPLICE_APP");

    const juce::File project(juce::File::getCurrentWorkingDirectory().getChildFile(args.words[0]));
    const juce::File out(juce::File::getCurrentWorkingDirectory().getChildFile(args.words[1]));
    if (! project.existsAsFile())
        return fail("no project at " + project.getFullPathName());
    if (! formatFor(out))
        return fail("can't tell the format from " + out.getFileName() + ": use .wav, .aiff, .flac, .ogg or .mp3");

    const juce::TemporaryFile report(".txt");
    juce::ChildProcess        child;
    const juce::StringArray   command { app.getFullPathName(), "--render", project.getFullPathName(), out.getFullPathName(),
                                        juce::String(rate ? rate->getDoubleValue() : 48000.0),
                                        juce::String(options.bitsPerSample), juce::String(contents),
                                        report.getFile().getFullPathName() };
    if (! child.start(command, 0))
        return fail("couldn't start " + app.getFullPathName());
    child.waitForProcessToFinish(-1);

    const auto text = report.getFile().loadFileAsString();
    const auto code = child.getExitCode();
    (code == 0 ? std::cout : std::cerr) << text << (text.endsWith("\n") || text.isEmpty() ? "" : "\n");
    if (code != 0 && text.isEmpty())
        return fail("the render failed (the app exited with " + juce::String((int) code) + ")");
    return code == 0 ? kOk : kFailed;
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce; // the message thread some plugin formats need

    Args args;
    for (int i = 1; i < argc; ++i)
        args.words.add(juce::CharPointer_UTF8(argv[i]));

    if (args.words.isEmpty() || args.flag("--help") || args.flag("-h") || args.words[0] == "help")
    {
        std::cout << kUsageText;
        return args.words.isEmpty() ? kUsage : kOk;
    }

    const auto command = args.words[0];
    args.words.remove(0);

    int result = kUsage;
    if (command == "convert")      result = convert(args);
    else if (command == "analyze") result = analyze(args);
    else if (command == "apply")   result = apply(args);
    else if (command == "macros")  result = listMacros();
    else if (command == "render")  result = render(args);
    else
        std::cerr << "soundsplice-cli: no command \"" << command << "\"\n";

    if (result == kUsage)
        std::cerr << kUsageText;
    return result;
}
