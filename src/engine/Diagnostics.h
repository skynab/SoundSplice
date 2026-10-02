#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/AmplitudeAnalysis.h"
#include "engine/Repair.h"
#include "engine/SilenceDetection.h"

namespace soundsplice::engine
{
/**
    Audition's Diagnostics: one pass over a clip's audio that lists what's
    wrong with it - clicks, clipped stretches, silences and a DC offset - each
    with where it is, so it can be heard, selected and fixed one at a time or
    all at once.

    Fed a chunk at a time, like the other scans, so a long recording is never
    held in memory whole. Clicks are found on a mono mix in blocks that
    overlap by a little, as engine::repair::findClicks needs history to model
    the audio; the others reuse the analysers the Analyze menu has.
    JUCE-free.
*/
namespace diagnostics
{
    enum class Kind
    {
        Click,
        Clipping,
        Silence,
        DcOffset
    };

    struct Issue
    {
        Kind         kind  = Kind::Click;
        std::int64_t from  = 0; // frames, half-open
        std::int64_t to    = 0;
        double       value = 0.0; // a DC offset's size, as a share of full scale

        bool operator==(const Issue&) const = default;
    };

    struct Settings
    {
        double clickSensitivity  = 8.0;   // as Click Removal's Normal
        double clickMaxWidthMs   = 2.0;
        float  silenceDb         = -60.0f;
        double minSilenceSeconds = 1.0;
        double dcThreshold       = 0.001; // -60 dB: below it, nothing worth taking out
    };

    class Scanner
    {
    public:
        void prepare(double sampleRate, const Settings& settings)
        {
            rate_     = sampleRate > 0.0 ? sampleRate : 48000.0;
            settings_ = settings;
            silence_  = silence::PeakEnvelope(std::max(1, (int) std::lround(rate_ * 0.01)));
            clipping_ = ClippingDetector();
            stats_.prepare(rate_, 2);
            mono_.clear();
            monoStart_ = 0;
            frames_    = 0;
            clicks_.clear();
        }

        void process(const float* const* channels, int numChannels, int frames)
        {
            clipping_.process(channels, numChannels, frames);
            silence_.append(channels, numChannels, frames);
            stats_.process(channels, numChannels, frames);

            for (int i = 0; i < frames; ++i)
            {
                float sum = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    sum += channels[ch][i];
                mono_.push_back(sum / (float) std::max(1, numChannels));
            }
            frames_ += frames;

            if ((int) mono_.size() >= kBlock + kOverlap)
                findClicksIn((int) mono_.size() - kOverlap);
        }

        /** Everything found, in time order. */
        std::vector<Issue> finish()
        {
            findClicksIn((int) mono_.size());

            std::vector<Issue> issues = clicks_;
            for (const auto& run : clipping_.finish())
                issues.push_back({ Kind::Clipping, run.from, run.to, 0.0 });

            const auto& peaks = silence_.finish();
            for (const auto& run : silence::silentRuns(peaks, silence_.windowFrames(), frames_,
                                                       silence::gainForDecibels(settings_.silenceDb),
                                                       (std::int64_t) std::llround(settings_.minSilenceSeconds * rate_)))
                issues.push_back({ Kind::Silence, run.from, run.to, 0.0 });

            const auto report = stats_.report();
            const double dc   = std::abs(report.dcOffsetPercent) / 100.0;
            if (dc >= settings_.dcThreshold)
                issues.push_back({ Kind::DcOffset, 0, frames_, dc });

            std::stable_sort(issues.begin(), issues.end(), [](const Issue& a, const Issue& b) { return a.from < b.from; });
            return issues;
        }

    private:
        static constexpr int kBlock   = 1 << 16;
        static constexpr int kOverlap = 256; // history kept for the next block, and clicks straddling its edge

        /** Clicks in the mono buffer up to @p upTo, whose samples are then let
            go but for the overlap. */
        void findClicksIn(int upTo)
        {
            if (upTo <= 0)
                return;

            const int width = std::max(1, (int) std::lround(settings_.clickMaxWidthMs * 0.001 * rate_));
            // Only from where the last pass stopped, which the overlap left as history.
            const int from = monoStart_ == 0 ? 0 : kOverlap;
            for (const auto& [clickFrom, clickTo] : repair::findClicks(mono_, from, upTo, settings_.clickSensitivity, width))
                clicks_.push_back({ Kind::Click, monoStart_ + clickFrom, monoStart_ + clickTo, 0.0 });

            const int keep = std::min((int) mono_.size(), kOverlap);
            const int drop = upTo - keep > 0 ? upTo - keep : 0;
            if (drop > 0)
            {
                mono_.erase(mono_.begin(), mono_.begin() + drop);
                monoStart_ += drop;
            }
        }

        double                 rate_ = 48000.0;
        Settings               settings_;
        silence::PeakEnvelope  silence_ { 480 };
        ClippingDetector       clipping_;
        AmplitudeStatistics    stats_;
        std::vector<float>     mono_;
        std::int64_t           monoStart_ = 0; // the frame mono_[0] is
        std::int64_t           frames_    = 0;
        std::vector<Issue>     clicks_;
    };
}

} // namespace soundsplice::engine
