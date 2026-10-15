#pragma once
#include <chrono>
#include <cstdint>
namespace chili {
// Host time the game thread spent on the things that can make one frame long, and how often they
// happened, added up since the start of the run. The frame timer takes the difference at every
// game frame boundary and keeps the frames that took too long (frame_timer.hpp, the stall log).
// Each field has one writer: the game thread, or for the renderer's fields the thread that runs the GE
// lists. A CostTimer costs two clock reads, so it goes around a whole list, a submission, a file read:
// never around a vertex or a draw.
struct FrameCosts {
    // GE lists from the first word to END: state, vertex work, texture decode, queueing for the GPU.
    std::uint64_t ge_ns{};
    // The end of a list that ends a picture: recording the queued draws and handing them to Direct3D.
    // present_ns, gpu_wait_ns, pipeline_ns and surface_ns are parts of it.
    std::uint64_t submit_ns{};
    std::uint64_t present_ns{};    // inside IDXGISwapChain::Present
    std::uint64_t gpu_wait_ns{};   // waiting for the graphics card to finish an earlier frame
    std::uint64_t pipeline_ns{};   // pipeline states created
    std::uint64_t surface_ns{};    // render targets, depth buffers and their copies created
    std::uint64_t texture_ns{};    // textures decoded from guest memory (a part of ge_ns)
    std::uint64_t file_ns{};       // sceIoRead: the disc archive and the memory stick
    std::uint64_t audio_ns{};      // sound blocks handed to the output ring
    // With the lists on a thread of their own (GeRuntime::start_worker) ge_ns and submit_ns are that
    // thread's time and no part of the game thread's frame; what the game thread spends is the time it
    // waits in sceGeListSync and sceGeDrawSync for lists that are not done.
    std::uint64_t ge_wait_ns{};
    bool ge_on_worker{};
    std::uint64_t texture_bytes{}, file_bytes{};
    std::uint32_t lists{}, draws{}, submits{}, pipelines{}, surfaces{}, textures{}, file_reads{}, audio_blocks{};
};
inline FrameCosts frame_costs;

class CostTimer {
public:
    explicit CostTimer(std::uint64_t &sink) noexcept : sink_(sink), start_(std::chrono::steady_clock::now()) {}
    ~CostTimer() {
        sink_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
    }
    CostTimer(const CostTimer &) = delete;
    CostTimer &operator=(const CostTimer &) = delete;
private:
    std::uint64_t &sink_;
    std::chrono::steady_clock::time_point start_;
};
}
