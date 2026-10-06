#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace soundsplice::engine::cdimage
{
/**
    A CD image: a BIN of raw CD audio (16-bit stereo, 44.1 kHz, little-endian,
    padded to whole sectors) and a CUE sheet saying where each track starts -
    which any burner takes to make an audio CD (Audacity's labels to CD, via
    a cue).

    Tracks start on CD frames (75 a second, 588 samples each), so a marker
    between frames moves to the nearest. The Red Book's limits are kept: no
    more than 99 tracks, none shorter than four seconds - a marker closer
    than that to the track before is left out rather than making a CD a
    player may refuse.
*/
inline constexpr int    kSampleRate      = 44100;
inline constexpr int    kSamplesPerFrame = 588; // one sector of 2352 bytes
inline constexpr int    kFramesPerSecond = 75;
inline constexpr int    kMaxTracks       = 99;
inline constexpr double kMinTrackSeconds = 4.0;

struct Track
{
    int64_t     startFrame = 0;
    std::string title;
};

/** The tracks for marks at @p seconds (with titles), over audio
    @p totalSeconds long. Track 1 always starts at 0. */
inline std::vector<Track> tracksFor(std::vector<std::pair<double, std::string>> marks, double totalSeconds)
{
    std::sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<Track> tracks;
    tracks.push_back({ 0, marks.empty() || marks.front().first > 0.5 / kFramesPerSecond ? std::string() : marks.front().second });

    const auto endFrame = (int64_t) std::floor(totalSeconds * kFramesPerSecond);
    const auto minimum  = (int64_t) std::ceil(kMinTrackSeconds * kFramesPerSecond);
    for (const auto& [seconds, title] : marks)
    {
        const auto frame = (int64_t) std::llround(seconds * kFramesPerSecond);
        if (frame - tracks.back().startFrame < minimum || endFrame - frame < minimum)
            continue; // too short a track either side
        if ((int) tracks.size() >= kMaxTracks)
            break;
        tracks.push_back({ frame, title });
    }
    return tracks;
}

/** "MM:SS:FF", as a cue sheet writes a time. */
inline std::string timeText(int64_t frames)
{
    const auto minutes = frames / (60 * kFramesPerSecond);
    const auto seconds = (frames / kFramesPerSecond) % 60;
    const auto rest    = frames % kFramesPerSecond;
    char text[16];
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld", (long long) minutes, (long long) seconds, (long long) rest);
    return text;
}

/** A cue sheet's quoted string: its quotes can't be in it. */
inline std::string quoted(std::string text)
{
    std::replace(text.begin(), text.end(), '"', '\'');
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r'; }), text.end());
    return "\"" + text + "\"";
}

/** The cue sheet for @p binName holding @p tracks, with CD-TEXT. */
inline std::string cueSheet(const std::string& binName, const std::vector<Track>& tracks, const std::string& title,
                            const std::string& performer)
{
    std::ostringstream cue;
    if (! performer.empty())
        cue << "PERFORMER " << quoted(performer) << "\r\n";
    if (! title.empty())
        cue << "TITLE " << quoted(title) << "\r\n";
    cue << "FILE " << quoted(binName) << " BINARY\r\n";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        char number[8];
        std::snprintf(number, sizeof number, "%02d", (int) i + 1);
        cue << "  TRACK " << number << " AUDIO\r\n";
        if (! tracks[i].title.empty())
            cue << "    TITLE " << quoted(tracks[i].title) << "\r\n";
        if (! performer.empty())
            cue << "    PERFORMER " << quoted(performer) << "\r\n";
        cue << "    INDEX 01 " << timeText(tracks[i].startFrame) << "\r\n";
    }
    return cue.str();
}

/** @p left and @p right (already 44.1 kHz, already dithered) as CD audio:
    16-bit little-endian interleaved, padded with silence to a whole sector. */
inline std::vector<uint8_t> binFor(const float* left, const float* right, int64_t samples)
{
    const auto padded = ((samples + kSamplesPerFrame - 1) / kSamplesPerFrame) * kSamplesPerFrame;
    std::vector<uint8_t> bin((size_t) padded * 4, 0);
    for (int64_t i = 0; i < samples; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x     = (ch == 0 ? left : right)[i];
            const auto  value = (int16_t) std::clamp(std::lround((double) x * 32768.0), -32768L, 32767L);
            bin[(size_t) (i * 4 + ch * 2)]     = (uint8_t) (value & 0xff);
            bin[(size_t) (i * 4 + ch * 2 + 1)] = (uint8_t) ((value >> 8) & 0xff);
        }
    return bin;
}

} // namespace soundsplice::engine::cdimage
