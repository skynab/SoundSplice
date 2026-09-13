#pragma once

#include <atomic>
#include <cassert>
#include <thread>

namespace looper::rt
{
/**
    Debug-only tracking of "the audio thread", used to assert that code which is
    allowed to allocate, lock, or block never runs on the real-time path.

    Call markCurrentThreadAsAudioThread() at the top of the audio callback. Then
    guard any non-real-time-safe function with LOOPER_ASSERT_NOT_AUDIO_THREAD().
    The assertions compile away entirely in release builds.
*/
inline std::atomic<bool> gAudioThreadValid{false};
inline std::thread::id   gAudioThreadId{};

inline void markCurrentThreadAsAudioThread() noexcept
{
    gAudioThreadId = std::this_thread::get_id();
    gAudioThreadValid.store(true, std::memory_order_release);
}

inline bool isAudioThread() noexcept
{
    return gAudioThreadValid.load(std::memory_order_acquire)
        && gAudioThreadId == std::this_thread::get_id();
}

} // namespace looper::rt

#if defined(NDEBUG)
    #define LOOPER_ASSERT_NOT_AUDIO_THREAD() ((void) 0)
#else
    #define LOOPER_ASSERT_NOT_AUDIO_THREAD() \
        assert(!::looper::rt::isAudioThread() && "This code path must not run on the audio thread")
#endif
