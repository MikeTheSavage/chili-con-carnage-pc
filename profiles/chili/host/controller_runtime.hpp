#pragma once
#include "thread_runtime.hpp"
namespace chili {
class ControllerSamplingProvenance;
// x,y: the PSP stick. rx,ry: a second stick the PSP does not have; it never
// reaches the guest's pad sample and only feeds the dual-analog control overrides.
// turn,pitch: degrees the mouse moved the view since the previous sample (right and down positive), which the
// dual-analog layer applies as that angle in the frame of the sample; they never reach the guest's pad sample either.
struct ControllerInput {std::uint32_t buttons{};std::uint8_t x{128u},y{128u},rx{128u},ry{128u};float turn{},pitch{};};
ControllerInput poll_host_controller();
class ControllerRuntime {
public:
    ControllerRuntime(psprecomp::Runtime &,ThreadRuntime &,std::function<ControllerInput()> input=poll_host_controller);
    ~ControllerRuntime();
    // Setup-time optional passive observer; caller owns its lifetime.
    void set_sampling_observer(ControllerSamplingProvenance *observer) noexcept { sampling_observer_ = observer; }
    // Off (the default): a read waits until 16,667 us after the previous sample.
    // On: one sample per vblank interval; a read in an interval whose sample was
    // already taken waits for the next vblank, and never longer.
    void set_vblank_latch(bool on) noexcept { vblank_latch_ = on; }
private:
    void read(psprecomp::AllegrexContext &);
    static void resumed(psprecomp::Runtime &,psprecomp::AllegrexContext &);
    static ControllerRuntime *active_;
    psprecomp::Runtime &runtime_;ThreadRuntime &threads_;std::function<ControllerInput()> input_;
    std::map<std::int32_t,psprecomp::AllegrexContext> pending_;
    std::uint32_t mode_{};std::uint64_t next_sample_{},sampled_vblank_{};bool vblank_latch_{},sampled_{};
    ControllerSamplingProvenance *sampling_observer_{};
};
}
