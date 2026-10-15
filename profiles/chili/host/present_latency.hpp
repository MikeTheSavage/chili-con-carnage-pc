#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
namespace chili {
// Wall time from a pad read to the present call of the first picture drawn from it: what the port itself
// puts between the controls and the window (the frame's work, the wait for the frame end, the drawing).
// Two clock reads per game frame and a counter; the totals go to the log when the game ends.
class PresentLatency {
public:
    using Clock = std::chrono::steady_clock;
    // The pad is read on the game thread; the present may come from the GE list thread, and when that
    // thread is behind, after the next pad read. So a list carries the number of the pad read of its frame
    // from the moment the game hands it over (`stamp`, game thread) to its present.
    void sampled() noexcept { const std::lock_guard<std::mutex> lock(mutex_); times_[++samples_ % times_.size()] = Clock::now(); }
    std::uint32_t stamp() noexcept { const std::lock_guard<std::mutex> lock(mutex_); return samples_ & stamp_mask; }
    // `own`: the picture was drawn in the frame of the pad read `stamp`. Otherwise it is the scene of the
    // frame before it, which the game copies to the screen one frame end later.
    void presented(bool own, std::uint32_t stamp) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::uint32_t sample = (own ? stamp : stamp - 1u) & stamp_mask, age = (samples_ - sample) & stamp_mask;
        // One picture per pad read: a second present of the same frame is not a new answer to it.
        if (!samples_ || !sample || age >= times_.size() || (credited_ && sample == last_credited_)) return;
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - times_[sample % times_.size()]).count();
        if (!(ms >= 0.0)) return;
        ++bins_[ms < static_cast<double>(bins_.size() - 1u) ? static_cast<std::size_t>(ms) : bins_.size() - 1u];
        ++count_; total_ms_ += ms;
        if (ms > worst_ms_) worst_ms_ = ms;
        last_credited_ = sample; credited_ = true;
    }
    std::uint64_t count() const noexcept { return count_; }
    std::string report(const char *what) const {
        const auto rank = [this](double share) {
            const auto wanted = static_cast<std::uint64_t>(share * static_cast<double>(count_));
            std::uint64_t seen = 0u;
            for (std::size_t bin = 0; bin < bins_.size(); ++bin) { seen += bins_[bin]; if (seen > wanted) return bin; }
            return bins_.size() - 1u;
        };
        char line[192];
        std::snprintf(line, sizeof(line), "[render] pad read to present, %s pictures: count=%llu mean=%.1f ms p50=%zu p90=%zu p99=%zu max=%.1f",
            what, static_cast<unsigned long long>(count_), count_ ? total_ms_ / static_cast<double>(count_) : 0.0, rank(0.5), rank(0.9), rank(0.99), worst_ms_);
        return line;
    }

private:
    static constexpr std::uint32_t stamp_mask = 0x7FFFFFFFu;
    std::mutex mutex_;
    // When each of the last pad reads was taken, by its number.
    std::array<Clock::time_point, 64> times_{};
    std::uint32_t samples_{}, last_credited_{};
    std::array<std::uint64_t, 257> bins_{};
    std::uint64_t count_{};
    double total_ms_{}, worst_ms_{};
    bool credited_{};
};
}
