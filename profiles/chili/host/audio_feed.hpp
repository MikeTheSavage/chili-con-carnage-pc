#pragma once
#include "audio_stats.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>
namespace chili {
// The host side of the game's one audio stream (44.1 kHz, 16 bit, stereo).
//
// The game thread appends blocks to a ring; a feeder thread hands them to the
// audio device whenever the device asks, so the device is fed on its own clock
// and never waits for the game. The frames in the ring (the lead) are what
// rides out the time the game spends working on a frame. When the ring runs
// dry the feeder fades out, plays silence until the ring is back to
// `resume_frames`, then fades in: one clean gap instead of a train of clicks.
//
// write() and lead_frames() belong to the game thread; everything the device
// needs happens on the feeder thread. Nothing here reads guest state.
class HostPcmFeed {
public:
    static constexpr unsigned rate=44100u;
    // Frames of every fade and of the look-ahead the feeder keeps for the fade out.
    static constexpr unsigned fade_frames=132u;
    HostPcmFeed(unsigned resume_frames,AudioDeviceCounters &counters,WavWriter *capture);
    ~HostPcmFeed();
    HostPcmFeed(const HostPcmFeed &)=delete;
    HostPcmFeed &operator=(const HostPcmFeed &)=delete;
    // Appends interleaved stereo frames. False when the ring is full (two seconds).
    bool write(std::span<const short> interleaved) noexcept;
    // Frames written and not yet handed to the device, without the ones restart() gave up.
    unsigned lead_frames() const noexcept {
        const auto written=write_.load(std::memory_order_relaxed);
        return static_cast<unsigned>(written-std::max(read_.load(std::memory_order_acquire),std::min(drop_to_.load(std::memory_order_relaxed),written)));
    }
    // The game gave its output up and has taken it again: what it wrote before is not played behind
    // the new start. The feeder fades it out and skips the rest; the ring counts as empty at once.
    void restart() noexcept {drop_to_.store(write_.load(std::memory_order_relaxed),std::memory_order_release);}
    // What the feeder plays to: the device's name, or why there is none. Safe after the first block.
    std::string device_report() const;
private:
    static constexpr std::uint64_t capacity=1u<<17u;          // frames, a power of two (2.97 s)
    void run();
    void fill(short *request,unsigned requested) noexcept;
    void copy_out(short *out,std::uint64_t from,unsigned frames) const noexcept;
    struct Device;
    bool open(Device &device);
    bool play(Device &device);
    void play_without_device(unsigned milliseconds);
    std::vector<short> ring_;
    std::atomic<std::uint64_t> write_{},read_{},drop_to_{};
    std::atomic<bool> stop_{},opened_{};
    const unsigned resume_frames_;
    AudioDeviceCounters &counters_;
    WavWriter *capture_;
    // Feeder thread only.
    bool playing_{},started_{};
    unsigned fade_in_left_{};
    std::vector<short> scratch_;
    std::string report_;
    void *wake_{};
    std::thread thread_;
};
}
