#pragma once
#include "thread_runtime.hpp"
namespace chili {
struct ControllerInput {std::uint32_t buttons{};std::uint8_t x{128u},y{128u};};
ControllerInput poll_host_controller();
class ControllerRuntime {
public:
    ControllerRuntime(psprecomp::Runtime &,ThreadRuntime &,std::function<ControllerInput()> input=poll_host_controller);
    ~ControllerRuntime();
private:
    void read(psprecomp::AllegrexContext &);
    static void resumed(psprecomp::Runtime &,psprecomp::AllegrexContext &);
    static ControllerRuntime *active_;
    psprecomp::Runtime &runtime_;ThreadRuntime &threads_;std::function<ControllerInput()> input_;
    std::map<std::int32_t,psprecomp::AllegrexContext> pending_;
    std::uint32_t mode_{};std::uint64_t next_sample_{};
};
}
