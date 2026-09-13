#include <catch2/catch_test_macros.hpp>

#include <rt/SpscRingBuffer.h>

#include <thread>

using looper::rt::SpscRingBuffer;

TEST_CASE("SpscRingBuffer preserves FIFO order", "[rt][spsc]")
{
    SpscRingBuffer<int> q(8);
    for (int i = 0; i < 5; ++i)
        REQUIRE(q.push(i));

    int v = -1;
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(q.pop(v));
        REQUIRE(v == i);
    }
    REQUIRE_FALSE(q.pop(v));
}

TEST_CASE("SpscRingBuffer reports empty and full correctly", "[rt][spsc]")
{
    SpscRingBuffer<int> q(4);
    REQUIRE(q.capacity() == 4);
    REQUIRE(q.empty());

    for (int i = 0; i < 4; ++i)
        REQUIRE(q.push(i));

    REQUIRE(q.size() == 4);
    REQUIRE_FALSE(q.push(99)); // full

    int v = 0;
    REQUIRE(q.pop(v));
    REQUIRE(v == 0);
    REQUIRE(q.push(99)); // space freed up
}

TEST_CASE("SpscRingBuffer rounds capacity up to a power of two", "[rt][spsc]")
{
    REQUIRE(SpscRingBuffer<int>(1).capacity() == 2);
    REQUIRE(SpscRingBuffer<int>(3).capacity() == 4);
    REQUIRE(SpscRingBuffer<int>(5).capacity() == 8);
    REQUIRE(SpscRingBuffer<int>(1000).capacity() == 1024);
}

TEST_CASE("SpscRingBuffer survives many wrap-arounds", "[rt][spsc]")
{
    SpscRingBuffer<int> q(4);
    int v = 0;
    for (int i = 0; i < 1000; ++i)
    {
        REQUIRE(q.push(i));
        REQUIRE(q.pop(v));
        REQUIRE(v == i);
    }
    REQUIRE(q.empty());
}

TEST_CASE("SpscRingBuffer is correct under concurrent producer/consumer", "[rt][spsc][thread]")
{
    constexpr int count = 1'000'000;
    SpscRingBuffer<int> q(1024);

    std::thread producer([&]
    {
        for (int i = 0; i < count; ++i)
            while (! q.push(i)) { /* spin until the consumer frees space */ }
    });

    long long sum = 0;
    int received = 0;
    int expected = 0;
    bool ordered = true;
    int v = 0;

    while (received < count)
    {
        if (q.pop(v))
        {
            if (v != expected)
                ordered = false;
            ++expected;
            sum += v;
            ++received;
        }
    }

    producer.join();

    const long long n = count;
    REQUIRE(ordered);                       // no reordering across the boundary
    REQUIRE(received == count);             // nothing lost
    REQUIRE(sum == (n - 1) * n / 2);        // nothing duplicated or corrupted
}
