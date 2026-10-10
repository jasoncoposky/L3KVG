#pragma once

#include <atomic>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace l3kvg {

/**
 * @brief High-performance reader-writer spinlock satisfying C++ SharedLockable.
 * Optimized for low-contention, low-latency in-memory graph operations.
 */
class SharedSpinLock {
    // Low 16 bits = reader count, Bit 16 = writer flag
    static constexpr uint32_t WRITER_BIT = 0x10000;
    std::atomic<uint32_t> state_{0};

public:
    SharedSpinLock() noexcept = default;
    ~SharedSpinLock() = default;

    SharedSpinLock(const SharedSpinLock&) = delete;
    SharedSpinLock& operator=(const SharedSpinLock&) = delete;

    void lock() noexcept {
        while (true) {
            uint32_t expected = 0;
            if (state_.compare_exchange_weak(expected, WRITER_BIT, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        }
    }

    bool try_lock() noexcept {
        uint32_t expected = 0;
        return state_.compare_exchange_strong(expected, WRITER_BIT, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void unlock() noexcept {
        state_.store(0, std::memory_order_release);
    }

    void lock_shared() noexcept {
        while (true) {
            uint32_t s = state_.load(std::memory_order_relaxed);
            if ((s & WRITER_BIT) == 0) {
                if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                    return;
                }
            }
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        }
    }

    bool try_lock_shared() noexcept {
        uint32_t s = state_.load(std::memory_order_relaxed);
        if ((s & WRITER_BIT) != 0) {
            return false;
        }
        return state_.compare_exchange_strong(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void unlock_shared() noexcept {
        state_.fetch_sub(1, std::memory_order_release);
    }
};

} // namespace l3kvg
