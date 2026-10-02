#pragma once

#include <cmath>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/AmplitudeAnalysis.h"
#include "engine/Loudness.h"

namespace soundsplice::app::renderreport
{
/**
    A render statistics report (REAPER's): beside an exported file, a page
    saying what's in it - its loudness and range, true and sample peaks, RMS
    and DC, the short-term loudness over time as a chart, and the clips that
    went into it. One self-contained HTML file: open it anywhere, send it to
    whoever asked for the master.
*/
struct ClipLine
{
    juce::String track, file;
    double       startSeconds = 0.0, lengthSeconds = 0.0;
};

struct Report
{
    juce::String                         fileName, format, project;
    double                               sampleRate = 0.0;
    int                                  channels   = 0;
    double                               seconds    = 0.0;
    engine::LoudnessReport               loudness;
    engine::AmplitudeStatistics::Report  amplitude;
    std::vector<std::pair<double, double>> shortTerm; // seconds, LUFS: one a second
    std::vector<ClipLine>                clips;
};

/** Measures @p audio for a report. */
inline Report analyse(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    Report report;
    report.sampleRate = sampleRate;
    report.channels   = audio.getNumChannels();
    const int frames  = audio.getNumSamples();
    report.seconds    = frames / sampleRate;
    if (frames == 0 || audio.getNumChannels() == 0)
        return report;

    engine::LoudnessMeter meter;
    meter.prepare(sampleRate, 2);
    const int last = audio.getNumChannels() - 1;
    const int step = (int) std::lround(sampleRate); // a point a second
    for (int at = 0; at < frames; at += step)
    {
        const int    n = juce::jmin(step, frames - at);
        const float* both[2] { audio.getReadPointer(0, at), audio.getReadPointer(juce::jmin(1, last), at) };
        meter.process(both, 2, n);
        report.shortTerm.emplace_back((at + n) / sampleRate, meter.shortTermLufs());
    }
    report.loudness = engine::LoudnessReport::of(meter, report.seconds);

    engine::AmplitudeStatistics statistics;
    statistics.prepare(sampleRate, audio.getNumChannels());
    statistics.process(audio.getArrayOfReadPointers(), audio.getNumChannels(), frames);
    report.amplitude = statistics.report();
    return report;
}

namespace detail
{
    inline juce::String escape(const juce::String& text)
    {
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
    }

    inline juce::String db(double value, const char* unit)
    {
        return std::isfinite(value) ? juce::String(value, 1) + " " + unit : juce::String("silent");
    }

    inline juce::String clock(double seconds)
    {
        const int minutes = (int) (seconds / 60.0);
        return juce::String(minutes) + ":" + juce::String(seconds - minutes * 60.0, 1).paddedLeft('0', 4);
    }

    /** The short-term loudness as an SVG line, -60 to 0 LUFS. */
    inline juce::String chart(const Report& report)
    {
        constexpr double width = 760.0, height = 200.0, floorLufs = -60.0;
        juce::String svg;
        svg << "<svg viewBox=\"0 0 " << width << " " << height << "\" role=\"img\" aria-label=\"Short-term loudness over time\">";
        for (const double lufs : { -10.0, -20.0, -30.0, -40.0, -50.0 })
        {
            const double y = height * lufs / floorLufs;
            svg << "<line x1=\"0\" x2=\"" << width << "\" y1=\"" << y << "\" y2=\"" << y << "\" class=\"grid\"/>"
                << "<text x=\"4\" y=\"" << (y - 3) << "\" class=\"axis\">" << (int) lufs << "</text>";
        }
        juce::String points;
        for (const auto& [seconds, lufs] : report.shortTerm)
        {
            const double x = report.seconds > 0.0 ? width * seconds / report.seconds : 0.0;
            const double v = std::isfinite(lufs) ? juce::jlimit(floorLufs, 0.0, lufs) : floorLufs;
            points << juce::String(x, 1) << "," << juce::String(height * v / floorLufs, 1) << " ";
        }
        svg << "<polyline points=\"" << points.trim() << "\" class=\"line\"/></svg>";
        return svg;
    }
}

/** The report as a standalone page. */
inline juce::String html(const Report& r)
{
    using namespace detail;
    juce::String page;
    page << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Render report: " << escape(r.fileName)
         << "</title><style>"
            "body{font-family:system-ui,sans-serif;margin:2em auto;max-width:800px;color:#222;background:#fff}"
            "h1{font-size:1.4em}table{border-collapse:collapse;width:100%;margin:1em 0}"
            "td,th{text-align:left;padding:4px 8px;border-bottom:1px solid #ddd}th{background:#f4f4f4}"
            "td.n{text-align:right;font-variant-numeric:tabular-nums}svg{width:100%;height:auto;background:#fafafa}"
            ".grid{stroke:#ddd}.axis{font-size:10px;fill:#888}.line{fill:none;stroke:#2a7ab0;stroke-width:1.5}"
            "@media(prefers-color-scheme:dark){body{background:#1e1e22;color:#ddd}th{background:#2a2a2e}td,th{border-color:#333}"
            "svg{background:#26262a}.grid{stroke:#333}}"
            "</style></head><body>";
    page << "<h1>" << escape(r.fileName) << "</h1><p>" << escape(r.project) << " &middot; " << escape(r.format) << ", "
         << (int) r.sampleRate << " Hz, " << r.channels << (r.channels == 1 ? " channel" : " channels") << ", "
         << clock(r.seconds) << " &middot; rendered " << juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M") << "</p>";

    page << "<table><tr><th>Measure</th><th>Value</th></tr>"
         << "<tr><td>Integrated loudness</td><td class=\"n\">" << db(r.loudness.integratedLufs, "LUFS") << "</td></tr>"
         << "<tr><td>Loudness range</td><td class=\"n\">" << juce::String(r.loudness.loudnessRangeLu, 1) << " LU</td></tr>"
         << "<tr><td>Short-term maximum</td><td class=\"n\">" << db(r.loudness.maxShortTermLufs, "LUFS") << "</td></tr>"
         << "<tr><td>Momentary maximum</td><td class=\"n\">" << db(r.loudness.maxMomentaryLufs, "LUFS") << "</td></tr>"
         << "<tr><td>True peak</td><td class=\"n\">" << db(r.loudness.truePeakDb, "dBTP") << "</td></tr>"
         << "<tr><td>Sample peak</td><td class=\"n\">" << db(r.amplitude.peakDb, "dBFS") << "</td></tr>"
         << "<tr><td>RMS</td><td class=\"n\">" << db(r.amplitude.rmsDb, "dBFS") << "</td></tr>"
         << "<tr><td>DC offset</td><td class=\"n\">" << juce::String(r.amplitude.dcOffsetPercent, 3) << " %</td></tr>"
         << "<tr><td>Dynamic range</td><td class=\"n\">" << juce::String(r.amplitude.dynamicRangeDb, 1) << " dB</td></tr>"
         << "</table>";

    page << "<h2>Short-term loudness</h2>" << chart(r);

    page << "<h2>Clips</h2>";
    if (r.clips.empty())
        page << "<p>None.</p>";
    else
    {
        page << "<table><tr><th>Track</th><th>File</th><th>Starts</th><th>Length</th></tr>";
        for (const auto& clip : r.clips)
            page << "<tr><td>" << escape(clip.track) << "</td><td>" << escape(clip.file) << "</td><td class=\"n\">"
                 << clock(clip.startSeconds) << "</td><td class=\"n\">" << clock(clip.lengthSeconds) << "</td></tr>";
        page << "</table>";
    }
    page << "<p><small>SoundSplice render report</small></p></body></html>";
    return page;
}

/** Where a file's report goes: beside it. */
inline juce::File fileFor(const juce::File& exported)
{
    return exported.getSiblingFile(exported.getFileNameWithoutExtension() + " report.html");
}

} // namespace soundsplice::app::renderreport
