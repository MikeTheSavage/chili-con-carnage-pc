#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace chili {
class AvRuntime;
class ThreadRuntime;

// Native MPEG service. Encoded media comes from the guest's ringbuffer callback;
// no filename lookup, substitute frames, or silent-success fallback is used.
class MpegRuntime {
public:
    static constexpr std::uint32_t callback_return_trampoline = 0x08000018u;
    static constexpr std::uint32_t decode_resume_trampoline = 0x0800001Cu;
    MpegRuntime(psprecomp::Runtime &, AvRuntime &, ThreadRuntime * = nullptr);
    ~MpegRuntime();
    MpegRuntime(const MpegRuntime &) = delete;
    MpegRuntime &operator=(const MpegRuntime &) = delete;
    // Runs after each decoded picture has been copied into guest memory at
    // `destination` (`stride` pixels per row): `rgba` is the same picture,
    // tightly packed. A hardware renderer takes its copy of the surface here.
    using FrameObserver = std::function<void(std::uint32_t destination, std::uint32_t stride, std::uint32_t width,
                                             std::uint32_t height, std::span<const std::uint8_t> rgba)>;
    void set_frame_observer(FrameObserver observer);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
