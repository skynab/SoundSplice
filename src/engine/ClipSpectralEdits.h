#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/SpectralEdit.h"

namespace soundsplice::engine
{
/**
    A spectral edit kept on a clip rather than written into its file, as
    REAPER keeps them: a box of time and frequency turned up, down or out.
    The file is untouched, so an edit can be removed or redone at any time;
    what plays is the file with every one of the clip's boxes applied (see
    app/SpectralRender.h, which makes and caches that).

    Times are seconds into the file, like the clip's volume curve, so trims
    and splits leave each box on the audio it was drawn over.
*/
struct SpectralRegion
{
    double startSeconds = 0.0;
    double endSeconds   = 0.0;
    double lowHz        = 0.0;
    double highHz       = 0.0;
    float  gainDb       = 0.0f; // kSilenceDb or below removes the band

    static constexpr float kSilenceDb = -96.0f;

    float gain() const noexcept { return gainDb <= kSilenceDb ? 0.0f : std::pow(10.0f, gainDb / 20.0f); }

    bool isValid() const noexcept { return endSeconds > startSeconds && highHz > lowHz; }

    bool overlaps(double from, double to) const noexcept { return startSeconds < to && endSeconds > from; }

    bool operator==(const SpectralRegion&) const = default;
};

using SpectralRegions = std::vector<SpectralRegion>;

namespace clipspectral
{
    /** Samples either side of a region its edit reads, so the part it leaves
        alone is rebuilt exactly: more than the longest window. */
    constexpr int kContextFrames = 16384;

    /** Applies each of @p regions that falls in @p samples, which hold one
        channel of the file from frame @p firstFrame on. False if one was too
        short to edit, which leaves that one out. */
    inline bool apply(std::vector<float>& samples, std::int64_t firstFrame, double sampleRate,
                      const SpectralRegions& regions)
    {
        bool allApplied = true;
        for (const auto& region : regions)
        {
            if (! region.isValid() || sampleRate <= 0.0)
                continue;
            const auto from = (std::int64_t) std::llround(region.startSeconds * sampleRate) - firstFrame;
            const auto to   = (std::int64_t) std::llround(region.endSeconds * sampleRate) - firstFrame;
            if (to <= 0 || from >= (std::int64_t) samples.size())
                continue;
            if (! spectral::scaleBand(samples, (int) std::max<std::int64_t>(0, from),
                                      (int) std::min<std::int64_t>((std::int64_t) samples.size(), to), sampleRate,
                                      region.lowHz, region.highHz, region.gain()))
                allApplied = false;
        }
        return allApplied;
    }

    /** The stretches of a file of @p totalFrames the regions touch, context
        included, merged where they meet: what has to be read, edited and
        written; everything between is copied as it is. */
    inline std::vector<std::pair<std::int64_t, std::int64_t>> spans(const SpectralRegions& regions, double sampleRate,
                                                                    std::int64_t totalFrames)
    {
        std::vector<std::pair<std::int64_t, std::int64_t>> result;
        for (const auto& region : regions)
            if (region.isValid())
                result.emplace_back(
                    std::clamp<std::int64_t>((std::int64_t) std::llround(region.startSeconds * sampleRate) - kContextFrames,
                                             0, totalFrames),
                    std::clamp<std::int64_t>((std::int64_t) std::llround(region.endSeconds * sampleRate) + kContextFrames,
                                             0, totalFrames));

        std::sort(result.begin(), result.end());
        std::vector<std::pair<std::int64_t, std::int64_t>> merged;
        for (const auto& span : result)
        {
            if (span.second <= span.first)
                continue;
            if (! merged.empty() && span.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, span.second);
            else
                merged.push_back(span);
        }
        return merged;
    }
}

} // namespace soundsplice::engine
