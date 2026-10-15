#pragma once
#include "psprecomp/runtime.hpp"
#include <filesystem>
#include <memory>
#include <ostream>
namespace chili {
class ThreadRuntime;
class PerformanceTelemetry;
struct AudioOptions {
    // Open no audio device; output blocks complete on the scheduler clock
    // instead (deterministic runs). Requires the scheduler.
    bool guest_clock_sink{};
    // CHILI_AUDIO_WAV: everything that reaches the device, silence included.
    std::filesystem::path capture;
    // CHILI_AUDIO_TRACE: one CSV row per output call, written at exit.
    std::filesystem::path trace;
    // With a device (pcm_queue.hpp, AudioPolicy): frames kept in the host ring,
    // the band above them in which the guest clock times the game's queue, and
    // whether the game may run ahead of its clock to refill a ring that is low.
    unsigned lead_frames{3072u},band_frames{1024u};
    bool catch_up{true};
};
class AudioRuntime {
public:
    explicit AudioRuntime(psprecomp::Runtime &,ThreadRuntime *threads = nullptr,bool guest_clock_sink = false);
    AudioRuntime(psprecomp::Runtime &,ThreadRuntime *threads,const AudioOptions &options);
    ~AudioRuntime();
    // Setup-time optional observer; caller owns its lifetime.
    void set_pacing_observer(PerformanceTelemetry *observer) noexcept;
    // One line of totals: blocks, rates, waits, underruns.
    void report(std::ostream &out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
