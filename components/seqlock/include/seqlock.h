#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Single-writer slot for handing plain data to readers on any task or core without locks.
// Double buffer plus a sequence counter that is odd while a write is in progress: the
// writer never blocks, and a reader retries if a write overlapped its copy.
// Exactly one task may call write(). Any number of tasks may call read().
template <typename T>
class SeqlockSlot {
    static_assert(std::is_trivially_copyable<T>::value, "SeqlockSlot needs plain data");

public:
    void write(const T& value) {
        const uint32_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&buf_[((seq + 2) / 2) & 1], &value, sizeof(T));
        seq_.store(seq + 2, std::memory_order_release);
    }

    // Copies the latest complete value into `out`. Returns false if every attempt
    // overlapped a write; the caller tries again later.
    bool read(T* out, int max_tries = 8) const {
        for (int i = 0; i < max_tries; ++i) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if ((s1 & 1u) != 0) {
                continue;
            }
            std::memcpy(out, &buf_[(s1 / 2) & 1], sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                return true;
            }
        }
        return false;
    }

    // Number of completed writes (wraps).
    uint32_t writes() const { return seq_.load(std::memory_order_acquire) / 2; }

private:
    T buf_[2]{};
    std::atomic<uint32_t> seq_{0};
};
