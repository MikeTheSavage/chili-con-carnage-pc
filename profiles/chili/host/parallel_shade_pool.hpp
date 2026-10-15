#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace chili {
#ifdef CHILI_PARALLEL_SHADE_DIAGNOSTIC
struct ParallelShadeDiagnostic {std::size_t rectangles{}, jobs{};unsigned initial_csr_flags{}, job_csr_flags{};bool attempted{}, succeeded{}, fallback{};};
inline thread_local ParallelShadeDiagnostic last_parallel_shade;
#endif
// Synchronous CPU-only shading executor. Context is borrowed only through run;
// no guest CPU dispatch, writes, callbacks or GE completion happen in workers.
class ParallelShadePool {
public:
    using Callback = void (*)(void *, std::size_t) noexcept;
    explicit ParallelShadePool(unsigned workers) {
        workers_.reserve(workers);
        try {
            for (unsigned n = 0; n < workers; ++n)
                workers_.emplace_back([this] { worker(); });
        } catch (...) {
            stop_and_join();
            throw;
        }
    }
    ~ParallelShadePool() { stop_and_join(); }
    ParallelShadePool(const ParallelShadePool &) = delete;
    ParallelShadePool &operator=(const ParallelShadePool &) = delete;

    void run(std::size_t count, Callback callback, void *opaque) {
        // Acquire before publishing a borrowed context. A later call cannot
        // reuse its storage until all workers have retired the previous call.
        std::lock_guard<std::mutex> serial(serial_);
        count_ = count;
        callback_ = callback;
        opaque_ = opaque;
        next_.store(0, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();
        work();
        auto retired = done_.load(std::memory_order_acquire);
        while (retired != workers_.size()) {
            done_.wait(retired, std::memory_order_acquire);
            retired = done_.load(std::memory_order_acquire);
        }
        callback_ = nullptr;
        opaque_ = nullptr;
        count_ = 0;
    }
private:
    void work() noexcept {
        for (;;) {
            const auto index = next_.fetch_add(1, std::memory_order_relaxed);
            if (index >= count_) return;
            callback_(opaque_, index);
        }
    }
    void worker() noexcept {
        std::size_t observed = 0;
        for (;;) {
            generation_.wait(observed, std::memory_order_acquire);
            if (stopping_.load(std::memory_order_acquire)) return;
            observed = generation_.load(std::memory_order_acquire);
            work();
            done_.fetch_add(1, std::memory_order_release);
            done_.notify_one();
        }
    }
    void stop_and_join() noexcept {
        stopping_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();
        for (auto &thread : workers_)
            if (thread.joinable()) thread.join();
    }
    std::mutex serial_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> generation_{0}, next_{0}, done_{0};
    std::atomic<bool> stopping_{false};
    std::size_t count_{};
    Callback callback_{};
    void *opaque_{};
};
}
