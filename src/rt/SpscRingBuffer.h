#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace looper::rt
{
// Conservative cache-line size. std::hardware_destructive_interference_size
// would be ideal but isn't reliably available across libc++/libstdc++, so we
// pin a value that's correct on every platform we target (x86-64 and arm64).
inline constexpr std::size_t kCacheLineSize = 64;

/**
    A bounded, wait-free single-producer / single-consumer ring buffer.

    This is the fundamental hand-off primitive between threads in the engine:
    the message thread pushes commands, the audio thread pops them (and vice
    versa for meters/events). Exactly one thread may call push() and exactly one
    thread may call pop(); with that contract neither ever blocks, locks, or
    allocates after construction.

    @tparam T  Must be trivially copyable — values are copied across the
               boundary, never constructed or destroyed on either side.

    The requested capacity is rounded up to a power of two so that index
    wrapping is a cheap mask. The usable capacity equals capacity().
*/
template <typename T>
class SpscRingBuffer
{
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRingBuffer<T> requires a trivially copyable T");

    explicit SpscRingBuffer(std::size_t minimumCapacity)
        : capacity_(roundUpToPowerOfTwo(minimumCapacity)),
          mask_(capacity_ - 1),
          storage_(capacity_)
    {
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /** Producer side. Copies @p value in; returns false if the buffer is full. */
    bool push(const T& value) noexcept
    {
        const auto write = writePos_.load(std::memory_order_relaxed);
        const auto read  = readPos_.load(std::memory_order_acquire);

        if (write - read >= capacity_)
            return false; // full

        storage_[write & mask_] = value;
        writePos_.store(write + 1, std::memory_order_release);
        return true;
    }

    /** Consumer side. Copies the next value into @p out; false if empty. */
    bool pop(T& out) noexcept
    {
        const auto read  = readPos_.load(std::memory_order_relaxed);
        const auto write = writePos_.load(std::memory_order_acquire);

        if (write == read)
            return false; // empty

        out = storage_[read & mask_];
        readPos_.store(read + 1, std::memory_order_release);
        return true;
    }

    /** Approximate number of queued items (safe to call from either side). */
    std::size_t size() const noexcept
    {
        const auto write = writePos_.load(std::memory_order_acquire);
        const auto read  = readPos_.load(std::memory_order_acquire);
        return write - read;
    }

    bool empty() const noexcept { return size() == 0; }

    /** Maximum number of items the buffer can hold. */
    std::size_t capacity() const noexcept { return capacity_; }

private:
    static std::size_t roundUpToPowerOfTwo(std::size_t v) noexcept
    {
        if (v < 2)
            return 2;
        --v;
        for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1)
            v |= v >> shift;
        return v + 1;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> storage_;

    // Keep the producer's and consumer's cursors on separate cache lines to
    // avoid false sharing between the two threads.
    alignas(kCacheLineSize) std::atomic<std::size_t> writePos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> readPos_{0};
};

} // namespace looper::rt
