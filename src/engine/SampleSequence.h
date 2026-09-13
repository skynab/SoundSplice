#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace soundsplice::engine::sequence
{
/**
    A clip's audio as a list of runs of samples taken from audio files, rather
    than one file of its own.

    Every destructive edit used to read the whole file, change a few seconds of
    it and write the whole file out again, so cutting a cough out of a two-hour
    recording cost two hours of disk and a full decode. With a sequence, an edit
    writes new audio only for the samples it changed and splices it in between
    runs of the audio it didn't touch, which stay exactly where they were in the
    files they came from. Those files are never written to again, so every older
    sequence (an undo step) still plays: copy-on-write, per block.

    The first edit on an imported file makes a sequence whose runs point into
    that file, so the import itself is never copied either.

    This half is JUCE-free and pure (splicing, slicing and the text form) so the
    boundary arithmetic, where every bug here would live, is tested on its own.
    Reading one as audio and writing blocks is in engine/SequenceAudioFormat.h.

    Positions are sample frames, half-open [from, to), as everywhere else.
*/

/** Samples [start, start + length) of one audio file. */
struct Span
{
    std::string  file;
    std::int64_t start  = 0;
    std::int64_t length = 0;

    bool operator==(const Span&) const = default;
};

struct SampleSequence
{
    double            sampleRate  = 0.0;
    int               numChannels = 0;
    std::vector<Span> spans;

    std::int64_t length() const
    {
        std::int64_t total = 0;
        for (const auto& span : spans)
            total += span.length;
        return total;
    }

    bool operator==(const SampleSequence&) const = default;
};

/** @p spans with empty spans dropped, and neighbours that continue one another
    in the same file joined back into one. Undoing a split by hand, or deleting
    then pasting the same audio back, then leaves the same list it started as. */
inline std::vector<Span> tidied(const std::vector<Span>& spans)
{
    std::vector<Span> out;
    for (const auto& span : spans)
    {
        if (span.length <= 0)
            continue;

        if (! out.empty() && out.back().file == span.file && out.back().start + out.back().length == span.start)
            out.back().length += span.length;
        else
            out.push_back(span);
    }
    return out;
}

/** The spans covering samples [from, to) of @p sequence, clamped to it. */
inline std::vector<Span> spansIn(const SampleSequence& sequence, std::int64_t from, std::int64_t to)
{
    const auto total = sequence.length();
    from = std::clamp<std::int64_t>(from, 0, total);
    to   = std::clamp<std::int64_t>(to, from, total);

    std::vector<Span> out;
    std::int64_t      position = 0;

    for (const auto& span : sequence.spans)
    {
        const auto spanFrom = position;
        const auto spanTo   = position + span.length;
        position = spanTo;

        const auto first = std::max(from, spanFrom);
        const auto last  = std::min(to, spanTo);
        if (last <= first)
            continue;

        out.push_back({ span.file, span.start + (first - spanFrom), last - first });
    }

    return out;
}

/** @p sequence with samples [from, to) replaced by @p replacement, which may be
    longer or shorter, or nothing at all (a deletion). Everything either side
    is kept as the same runs of the same files. */
inline SampleSequence replaced(const SampleSequence& sequence, std::int64_t from, std::int64_t to,
                               const std::vector<Span>& replacement)
{
    const auto total = sequence.length();
    from = std::clamp<std::int64_t>(from, 0, total);
    to   = std::clamp<std::int64_t>(to, from, total);

    auto spans = spansIn(sequence, 0, from);
    spans.insert(spans.end(), replacement.begin(), replacement.end());

    const auto after = spansIn(sequence, to, total);
    spans.insert(spans.end(), after.begin(), after.end());

    auto out  = sequence;
    out.spans = tidied(spans);
    return out;
}

/** The first line of every sequence file. */
inline constexpr const char* kHeader = "SOUNDSPLICE-SEQUENCE 1";

/** @p sequence as text, one line per span. Paths are written as given (the
    caller decides whether they are relative) and run to the end of the line,
    so a path with spaces needs no quoting. */
inline std::string serialize(const SampleSequence& sequence)
{
    std::ostringstream out;
    out.imbue(std::locale::classic()); // "48000.5", never "48000,5"
    out << kHeader << '\n'
        << "rate " << std::setprecision(17) << sequence.sampleRate << '\n'
        << "channels " << sequence.numChannels << '\n';

    for (const auto& span : sequence.spans)
        out << "span " << span.start << ' ' << span.length << ' ' << span.file << '\n';

    return out.str();
}

/** Reads what serialize wrote. False, with @p error saying why, for anything
    that isn't a well-formed sequence; @p out is only written on success. */
inline bool parse(const std::string& text, SampleSequence& out, std::string* error = nullptr)
{
    const auto fail = [error](const std::string& why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };

    std::istringstream lines(text);
    std::string        line;

    const auto nextLine = [&lines, &line]
    {
        if (! std::getline(lines, line))
            return false;
        if (! line.empty() && line.back() == '\r') // written on one system, read on another
            line.pop_back();
        return true;
    };

    if (! nextLine() || line != kHeader)
        return fail("not a SoundSplice sequence");

    SampleSequence sequence;
    bool           haveRate = false, haveChannels = false;

    while (nextLine())
    {
        if (line.empty())
            continue;

        std::istringstream fields(line);
        fields.imbue(std::locale::classic());

        std::string keyword;
        fields >> keyword;

        if (keyword == "rate")
        {
            haveRate = static_cast<bool>(fields >> sequence.sampleRate) && sequence.sampleRate > 0.0;
            if (! haveRate)
                return fail("bad sample rate");
        }
        else if (keyword == "channels")
        {
            haveChannels = static_cast<bool>(fields >> sequence.numChannels) && sequence.numChannels > 0;
            if (! haveChannels)
                return fail("bad channel count");
        }
        else if (keyword == "span")
        {
            Span span;
            if (! (fields >> span.start >> span.length) || span.start < 0 || span.length <= 0)
                return fail("bad span: " + line);

            fields >> std::ws;
            std::getline(fields, span.file);
            if (span.file.empty())
                return fail("span without a file: " + line);

            sequence.spans.push_back(std::move(span));
        }
        // Anything else is from a newer version; skipped rather than refused,
        // so adding a field later doesn't make older builds lose the audio.
    }

    if (! haveRate || ! haveChannels)
        return fail("missing sample rate or channel count");

    out = std::move(sequence);
    return true;
}

} // namespace soundsplice::engine::sequence
