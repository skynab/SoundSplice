#include <catch2/catch_test_macros.hpp>

#include <engine/SampleSequence.h>

using namespace soundsplice::engine::sequence;

namespace
{
    /** One stereo sequence of three spans from two files: a0..a99, b0..b49, a100..a199. */
    SampleSequence threeSpans()
    {
        SampleSequence sequence;
        sequence.sampleRate  = 48000.0;
        sequence.numChannels = 2;
        sequence.spans = { { "a.wav", 0, 100 }, { "b.wav", 0, 50 }, { "a.wav", 100, 100 } };
        return sequence;
    }
}

TEST_CASE("A sequence's length is the sum of its spans", "[sequence]")
{
    REQUIRE(threeSpans().length() == 250);
    REQUIRE(SampleSequence {}.length() == 0);
}

TEST_CASE("Spans covering a range are cut at both ends", "[sequence]")
{
    const auto sequence = threeSpans();

    // Across all three spans.
    REQUIRE(spansIn(sequence, 90, 160)
            == std::vector<Span> { { "a.wav", 90, 10 }, { "b.wav", 0, 50 }, { "a.wav", 100, 10 } });

    // Inside one span.
    REQUIRE(spansIn(sequence, 110, 120) == std::vector<Span> { { "b.wav", 10, 10 } });

    // Exactly on span boundaries: nothing empty on either side.
    REQUIRE(spansIn(sequence, 100, 150) == std::vector<Span> { { "b.wav", 0, 50 } });

    // Clamped, and empty for an empty range.
    REQUIRE(spansIn(sequence, -10, 5) == std::vector<Span> { { "a.wav", 0, 5 } });
    REQUIRE(spansIn(sequence, 240, 1000) == std::vector<Span> { { "a.wav", 190, 10 } });
    REQUIRE(spansIn(sequence, 30, 30).empty());
}

TEST_CASE("Replacing a range keeps the audio either side in the files it came from", "[sequence]")
{
    const auto sequence = threeSpans();

    SECTION("with shorter new audio")
    {
        const auto out = replaced(sequence, 40, 120, { { "new.wav", 0, 10 } });
        REQUIRE(out.spans
                == std::vector<Span> { { "a.wav", 0, 40 }, { "new.wav", 0, 10 }, { "b.wav", 20, 30 },
                                       { "a.wav", 100, 100 } });
        REQUIRE(out.length() == 250 - 80 + 10);
        REQUIRE(out.sampleRate == sequence.sampleRate);
        REQUIRE(out.numChannels == sequence.numChannels);
    }

    SECTION("with nothing, which is a delete")
    {
        const auto out = replaced(sequence, 100, 150, {});

        // The two halves of a.wav meet again and become one span.
        REQUIRE(out.spans == std::vector<Span> { { "a.wav", 0, 200 } });
    }

    SECTION("at a single point, which is an insert")
    {
        const auto out = replaced(sequence, 250, 250, { { "new.wav", 0, 5 } });
        REQUIRE(out.length() == 255);
        REQUIRE(out.spans.back() == Span { "new.wav", 0, 5 });
    }

    SECTION("the whole thing")
    {
        const auto out = replaced(sequence, 0, 250, { { "new.wav", 0, 7 } });
        REQUIRE(out.spans == std::vector<Span> { { "new.wav", 0, 7 } });
    }
}

TEST_CASE("Tidying joins spans that continue each other and drops empty ones", "[sequence]")
{
    const std::vector<Span> messy { { "a.wav", 0, 10 }, { "a.wav", 10, 0 }, { "a.wav", 10, 5 },
                                    { "a.wav", 20, 5 }, { "b.wav", 25, 5 } };

    // a 20..25 doesn't continue a 0..15, and b is another file.
    REQUIRE(tidied(messy) == std::vector<Span> { { "a.wav", 0, 15 }, { "a.wav", 20, 5 }, { "b.wav", 25, 5 } });
}

TEST_CASE("A sequence reads back exactly as it was written", "[sequence]")
{
    auto sequence = threeSpans();
    sequence.sampleRate = 44100.25;
    sequence.spans.push_back({ "take (2) block.wav", 3000000000LL, 12 }); // a space, and past 32 bits

    const auto text = serialize(sequence);

    SampleSequence loaded;
    std::string    error;
    REQUIRE(parse(text, loaded, &error));
    REQUIRE(loaded == sequence);

    // Windows line endings, blank lines and unknown keywords are all tolerated.
    std::string crlf;
    for (const char c : text)
        crlf += c == '\n' ? std::string("\r\n\r\n") : std::string(1, c);
    crlf += "future-field 1\r\n";

    REQUIRE(parse(crlf, loaded, &error));
    REQUIRE(loaded == sequence);
}

TEST_CASE("Anything that isn't a well-formed sequence is refused", "[sequence]")
{
    SampleSequence untouched = threeSpans();
    std::string    error;

    REQUIRE_FALSE(parse("RIFF....WAVE", untouched, &error));
    REQUIRE_FALSE(parse(std::string(kHeader) + "\nchannels 2\n", untouched, &error));
    REQUIRE_FALSE(parse(std::string(kHeader) + "\nrate 48000\nchannels 0\n", untouched, &error));
    REQUIRE_FALSE(parse(std::string(kHeader) + "\nrate 48000\nchannels 1\nspan 0 10\n", untouched, &error));
    REQUIRE_FALSE(parse(std::string(kHeader) + "\nrate 48000\nchannels 1\nspan 0 -1 a.wav\n", untouched, &error));
    REQUIRE_FALSE(error.empty());

    // A failed parse leaves the destination alone.
    REQUIRE(untouched == threeSpans());
}
