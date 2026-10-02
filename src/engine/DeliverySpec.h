#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "engine/Loudness.h"

namespace soundsplice::engine::delivery
{
/**
    Delivery specs: what a platform asks of a file, checked against a mix.

    Each spec says what it measures and the range it wants; check() measures
    a rendered mix once (loudness, true and sample peak, RMS, noise floor,
    the silence at the head and tail) and says pass or fail for each, with
    the value measured. The numbers are the platforms' own published ones:

      ACX (Audible): RMS -23 to -18 dBFS, sample peak at most -3 dBFS, noise
        floor at most -60 dBFS, 0.5-1 s of room tone at the head and 1-5 s at
        the tail.
      Spotify, YouTube: -14 LUFS (they turn louder down), true peak -1 dBTP.
      Apple Podcasts: -16 LUFS +/-1, true peak -1 dBTP.
      EBU R128: -23 LUFS +/-0.5, true peak -1 dBTP.
      ATSC A/85: -24 LKFS +/-2, true peak -2 dBTP.

    What fixes a failing mix is the spec's export target (exportLufs,
    exportCeilingDb): Export Audio's loudness normalization with that
    ceiling reaches the loudness and the peak at once. The noise floor and
    the silences it can't fix - check() says what to do about them.

    JUCE-free; a spec is plain data so a test can pin each one.
*/
struct Spec
{
    const char* name;
    const char* description;

    // Loudness: either integrated LUFS (lufsMin..lufsMax) or, for ACX, RMS.
    double lufsMin = -std::numeric_limits<double>::infinity(), lufsMax = std::numeric_limits<double>::infinity();
    double rmsMin = -std::numeric_limits<double>::infinity(), rmsMax = std::numeric_limits<double>::infinity();
    double truePeakMax   = std::numeric_limits<double>::infinity(); // dBTP
    double samplePeakMax = std::numeric_limits<double>::infinity(); // dBFS
    double noiseFloorMax = std::numeric_limits<double>::infinity(); // dBFS
    double headMin = 0.0, headMax = std::numeric_limits<double>::infinity(); // seconds of quiet at the start
    double tailMin = 0.0, tailMax = std::numeric_limits<double>::infinity(); // and the end

    // What an export to this spec aims for.
    double exportLufs      = -16.0;
    double exportCeilingDb = -1.0;
    int    exportRate      = 44100;
    const char* exportFormat = "mp3"; // the file the platform takes
};

inline const std::vector<Spec>& all()
{
    static const std::vector<Spec> specs = []
    {
        std::vector<Spec> s;
        Spec acx { "ACX (Audible)", "Audiobooks for Audible, Amazon and iTunes" };
        acx.rmsMin = -23.0; acx.rmsMax = -18.0;
        acx.samplePeakMax = -3.0;
        acx.noiseFloorMax = -60.0;
        acx.headMin = 0.5; acx.headMax = 1.0;
        acx.tailMin = 1.0; acx.tailMax = 5.0;
        acx.exportLufs = -19.0; acx.exportCeilingDb = -3.5; acx.exportRate = 44100; acx.exportFormat = "mp3";
        s.push_back(acx);

        Spec spotify { "Spotify", "Music and podcasts: played at -14 LUFS" };
        spotify.lufsMin = -15.0; spotify.lufsMax = -13.0; spotify.truePeakMax = -1.0;
        spotify.exportLufs = -14.0; spotify.exportCeilingDb = -1.0; spotify.exportRate = 44100; spotify.exportFormat = "flac";
        s.push_back(spotify);

        Spec apple { "Apple Podcasts", "Spoken word: -16 LUFS" };
        apple.lufsMin = -17.0; apple.lufsMax = -15.0; apple.truePeakMax = -1.0;
        apple.exportLufs = -16.0; apple.exportCeilingDb = -1.0; apple.exportRate = 44100; apple.exportFormat = "mp3";
        s.push_back(apple);

        Spec youtube { "YouTube", "Played at -14 LUFS" };
        youtube.lufsMin = -15.0; youtube.lufsMax = -13.0; youtube.truePeakMax = -1.0;
        youtube.exportLufs = -14.0; youtube.exportCeilingDb = -1.0; youtube.exportRate = 48000; youtube.exportFormat = "wav";
        s.push_back(youtube);

        Spec ebu { "EBU R128 broadcast", "European television and radio" };
        ebu.lufsMin = -23.5; ebu.lufsMax = -22.5; ebu.truePeakMax = -1.0;
        ebu.exportLufs = -23.0; ebu.exportCeilingDb = -1.0; ebu.exportRate = 48000; ebu.exportFormat = "wav";
        s.push_back(ebu);

        Spec atsc { "ATSC A/85 broadcast", "US television" };
        atsc.lufsMin = -26.0; atsc.lufsMax = -22.0; atsc.truePeakMax = -2.0;
        atsc.exportLufs = -24.0; atsc.exportCeilingDb = -2.0; atsc.exportRate = 48000; atsc.exportFormat = "wav";
        s.push_back(atsc);
        return s;
    }();
    return specs;
}

/** What check() measures. */
struct Measurements
{
    double integratedLufs = LoudnessMeter::kSilence;
    double truePeakDb     = LoudnessMeter::kSilence;
    double samplePeakDb   = LoudnessMeter::kSilence;
    double rmsDb          = LoudnessMeter::kSilence;
    double noiseFloorDb   = LoudnessMeter::kSilence; // the quietest half second that isn't digital silence
    double headSeconds    = 0.0;                      // before the first sound
    double tailSeconds    = 0.0;                      // after the last
    double seconds        = 0.0;
};

namespace detail
{
    inline double decibels(double linear) { return linear > 0.0 ? 20.0 * std::log10(linear) : LoudnessMeter::kSilence; }

    /** Above this, in 10 ms windows, is sound rather than room tone: a
        speaking voice is far above it, a quiet room far below. */
    inline constexpr double kSoundThresholdDb = -45.0;
}

/** Measures @p left and @p right (the same pointer for mono). */
inline Measurements measure(const float* left, const float* right, int frames, double sampleRate)
{
    Measurements m;
    if (frames <= 0 || sampleRate <= 0.0)
        return m;
    m.seconds = frames / sampleRate;

    LoudnessMeter meter;
    meter.prepare(sampleRate, 2);
    const float* both[2] { left, right };
    meter.process(both, 2, frames);
    m.integratedLufs = meter.integratedLufs();
    m.truePeakDb     = meter.truePeakDb();
    m.samplePeakDb   = meter.samplePeakDb();

    double squares = 0.0;
    for (int i = 0; i < frames; ++i)
        squares += 0.5 * ((double) left[i] * left[i] + (double) right[i] * right[i]);
    m.rmsDb = detail::decibels(std::sqrt(squares / frames));

    // Noise floor: the quietest half second, digital silence left out (a
    // gap of zeros isn't a room, and ACX rejects those anyway).
    const int half = std::max(1, (int) (sampleRate * 0.5));
    double quietest = std::numeric_limits<double>::infinity();
    for (int at = 0; at + half <= frames; at += half / 2)
    {
        double sum = 0.0;
        for (int i = at; i < at + half; ++i)
            sum += 0.5 * ((double) left[i] * left[i] + (double) right[i] * right[i]);
        const double db = detail::decibels(std::sqrt(sum / half));
        if (db > -120.0)
            quietest = std::min(quietest, db);
    }
    if (std::isfinite(quietest))
        m.noiseFloorDb = quietest;

    // Head and tail: the quiet before the first 10 ms that's sound, and
    // after the last.
    const int window = std::max(1, (int) (sampleRate * 0.01));
    const auto loud  = [&](int at)
    {
        double peak = 0.0;
        for (int i = at; i < std::min(frames, at + window); ++i)
            peak = std::max(peak, (double) std::max(std::abs(left[i]), std::abs(right[i])));
        return detail::decibels(peak) > detail::kSoundThresholdDb;
    };
    int first = -1, last = -1;
    for (int at = 0; at < frames; at += window)
        if (loud(at))
        {
            first = at;
            break;
        }
    for (int at = ((frames - 1) / window) * window; at >= 0; at -= window)
        if (loud(at))
        {
            last = std::min(frames, at + window);
            break;
        }
    if (first >= 0)
    {
        m.headSeconds = first / sampleRate;
        m.tailSeconds = (frames - last) / sampleRate;
    }
    else
        m.headSeconds = m.tailSeconds = m.seconds;
    return m;
}

/** One line of a check. */
struct Result
{
    std::string measure;  // "Integrated loudness"
    std::string measured; // "-18.2 LUFS"
    std::string wanted;   // "-17 to -15 LUFS"
    bool        pass = true;
    std::string advice;   // what to do when it fails
};

namespace detail
{
    inline std::string number(double value, const char* unit)
    {
        if (! std::isfinite(value))
            return "silent";
        char text[48];
        std::snprintf(text, sizeof text, "%.1f %s", value, unit);
        return text;
    }

    inline std::string range(double low, double high, const char* unit)
    {
        char text[64];
        if (std::isfinite(low) && std::isfinite(high))
            std::snprintf(text, sizeof text, "%.1f to %.1f %s", low, high, unit);
        else if (std::isfinite(high))
            std::snprintf(text, sizeof text, "at most %.1f %s", high, unit);
        else
            std::snprintf(text, sizeof text, "at least %.1f %s", low, unit);
        return text;
    }

    inline bool within(double value, double low, double high)
    {
        return std::isfinite(value) && value >= low - 1e-9 && value <= high + 1e-9;
    }
}

/** @p m against @p spec: a line for each thing the spec asks about. */
inline std::vector<Result> check(const Spec& spec, const Measurements& m)
{
    using namespace detail;
    std::vector<Result> results;
    const char* exportFix = "Export with this spec's loudness target (Make It Pass)";

    if (std::isfinite(spec.lufsMin) || std::isfinite(spec.lufsMax))
        results.push_back({ "Integrated loudness", number(m.integratedLufs, "LUFS"), range(spec.lufsMin, spec.lufsMax, "LUFS"),
                            within(m.integratedLufs, spec.lufsMin, spec.lufsMax), exportFix });
    if (std::isfinite(spec.rmsMin) || std::isfinite(spec.rmsMax))
        results.push_back({ "RMS level", number(m.rmsDb, "dBFS"), range(spec.rmsMin, spec.rmsMax, "dBFS"),
                            within(m.rmsDb, spec.rmsMin, spec.rmsMax), exportFix });
    if (std::isfinite(spec.truePeakMax))
        results.push_back({ "True peak", number(m.truePeakDb, "dBTP"), range(-INFINITY, spec.truePeakMax, "dBTP"),
                            ! std::isfinite(m.truePeakDb) || m.truePeakDb <= spec.truePeakMax + 1e-9, exportFix });
    if (std::isfinite(spec.samplePeakMax))
        results.push_back({ "Peak", number(m.samplePeakDb, "dBFS"), range(-INFINITY, spec.samplePeakMax, "dBFS"),
                            ! std::isfinite(m.samplePeakDb) || m.samplePeakDb <= spec.samplePeakMax + 1e-9, exportFix });
    if (std::isfinite(spec.noiseFloorMax))
        results.push_back({ "Noise floor", number(m.noiseFloorDb, "dBFS"), range(-INFINITY, spec.noiseFloorMax, "dBFS"),
                            ! std::isfinite(m.noiseFloorDb) || m.noiseFloorDb <= spec.noiseFloorMax + 1e-9,
                            "Reduce the noise (Edit > Adaptive Noise Reduction, or Essential Sound's Reduce Noise)" });
    if (spec.headMin > 0.0 || std::isfinite(spec.headMax))
        results.push_back({ "Room tone at the start", number(m.headSeconds, "s"), range(spec.headMin, spec.headMax, "s"),
                            within(m.headSeconds, spec.headMin, spec.headMax),
                            m.headSeconds < spec.headMin ? "Add room tone before the first word (Generate > Room Tone)"
                                                         : "Trim the silence at the start" });
    if (spec.tailMin > 0.0 || std::isfinite(spec.tailMax))
        results.push_back({ "Room tone at the end", number(m.tailSeconds, "s"), range(spec.tailMin, spec.tailMax, "s"),
                            within(m.tailSeconds, spec.tailMin, spec.tailMax),
                            m.tailSeconds < spec.tailMin ? "Add room tone after the last word (Generate > Room Tone)"
                                                         : "Trim the silence at the end" });
    return results;
}

inline bool passes(const std::vector<Result>& results)
{
    return std::all_of(results.begin(), results.end(), [](const Result& r) { return r.pass; });
}

} // namespace soundsplice::engine::delivery
