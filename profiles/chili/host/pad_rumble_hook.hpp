#pragma once

// Vibration: the game's own call, handed to the pad in use.
//
// The engine has it all the way down: ControlSetup::setvibration(integer, number) and the script handler
// master_control_realtime_input_ctrl::set_vibration call 0x088C79E8, which calls the pad object's
// SetControllerVibration at 0x088248A8 with a0 = the pad, a1 = the integer (a motor) and f12 = the number (a
// strength). On the PSP that last function is empty ("jr ra"). A host function over it gives the two
// values to PcInput::game_vibration and then runs the empty original.
//
// Measured: nothing calls it in 105 seconds of Story 1-1.
// So this is wired for a mission that does call it, and the log counts the calls
// ("[input] vibration: calls by the game=..").
//
// Not installed in a deterministic run: a host function over a guest address moves the guest clock of such
// a run (replays/README.md), and no pad is read there anyway. The address is in generated unit 0008, which
// the dual_analog scheme overrides already (Pad::GetAxisPosition), so a player's run pays nothing more.

#include "pc_input.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <iostream>

namespace chili {

class PadRumbleHook {
public:
    static constexpr std::uint32_t set_controller_vibration_address = 0x088248A8u;
    static void install(psprecomp::Runtime &runtime) {
        original() = runtime.function_at(set_controller_vibration_address);
        if (original() == nullptr) {
            std::cerr << "[input] vibration: no generated code at " << psprecomp::hex32(set_controller_vibration_address) << ", the game's calls are not taken\n";
            return;
        }
        runtime.register_function(set_controller_vibration_address, &hook, "chili_pad_rumble");
        std::cout << "[input] vibration: the game's calls (its pad's SetControllerVibration at " << psprecomp::hex32(set_controller_vibration_address)
                  << ") go to the pad in use\n";
    }
    // Whether install() put the host function over the game's.
    static bool installed() { return original() != nullptr; }

private:
    static psprecomp::Runtime::RecompiledFunction &original() {
        static psprecomp::Runtime::RecompiledFunction function{};
        return function;
    }
    // Entered with ctx.pc on the hooked address; the original returns to the caller.
    static void hook(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
        PcInput::instance().game_vibration(ctx.gpr[5], ctx.fpr[12]);
        original()(runtime, ctx);
    }
};

} // namespace chili
