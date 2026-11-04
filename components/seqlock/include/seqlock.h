#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Single-writer slot for handing plain data to readers on any task or core without locks.
// Double buffer plus a sequence counter that is odd while a write is in progress: the
// writer never blocks. Write n (sequence 2n-1 while in progress, 2n when done) fills
// buffer n & 1, so a write in progress never touches the buffer holding the last complete
// value. A reader copies that buffer even while a write is in progress, and retries only if
// a later write started on the buffer it was copying (the writer lapped it).
// Exactly one task may call write(). Any number of tasks may call read().
template <typename T>
class SeqlockSlot {
    static_assert(std::is_trivially_copyable<T>::value, "SeqlockSlot needs plain data");

public:
    void write(const T& value) {
        const uint32_t seq = seq_.load(std::memory_order_relaxed);
        // Release: a reader that sees this odd value also sees the previous write's data.
        seq_.store(seq + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&buf_[((seq + 2) / 2) & 1], &value, sizeof(T));
        seq_.store(seq + 2, std::memory_order_release);
    }

    // Copies the latest complete value into `out` (at most one write old if a write finished
    // during the copy). Returns false only if the writer lapped every attempt; the caller
    // tries again later.
    bool read(T* out, int max_tries = 8) const {
        int tries = 0;
        return read(out, max_tries, &tries);
    }

    // As above; `tries_used` receives the number of attempts made (1 = no retry).
    bool read(T* out, int max_tries, int* tries_used) const {
        for (int i = 0; i < max_tries; ++i) {
            *tries_used = i + 1;
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            // Last complete value: buffer (s1 / 2) & 1, whether s1 is even (idle) or odd (the
            // write in progress fills the other buffer).
            std::memcpy(out, &buf_[(s1 / 2) & 1], sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t s2 = seq_.load(std::memory_order_relaxed);
            // The copied buffer is next written by the write that starts at sequence s1 + 3
            // (s1 even) or s1 + 2 (s1 odd). Until then the copy is whole. Unsigned: wraps safely.
            if (s2 - s1 < ((s1 & 1u) != 0 ? 2u : 3u)) {
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
