// The game's vibration call reaches the pad in use: the host function that goes over the pad object's
// empty SetControllerVibration (host/pad_rumble_hook.hpp), driven here the way generated code enters it,
// with a stand-in for the game's function and a made-up pad. The game itself was never seen to make the
// call (no vibration in 105 seconds of Story 1-1), so this is the only place the path is exercised.
#include "pad_rumble_hook.hpp"
#include "psprecomp/runtime.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks;
void require(bool ok, const char *what) { ++checks; if (!ok) throw std::runtime_error(what); }
constexpr std::uint32_t address = chili::PadRumbleHook::set_controller_vibration_address;
unsigned original_runs;
// What the game has at the address: nothing but the return to the caller.
void empty_function(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ++original_runs; ctx.pc = ctx.gpr[31]; }
}

int main() {
    try {
        psprecomp::Runtime runtime;
        require(runtime.function_at(address) == nullptr, "nothing at the address yet");
        chili::PadRumbleHook::install(runtime);
        require(runtime.function_at(address) == nullptr && !chili::PadRumbleHook::installed(), "without the game's code at the address nothing is installed");
        runtime.register_function(address, &empty_function, "stand-in for SetControllerVibration");
        chili::PadRumbleHook::install(runtime);
        const auto hooked = runtime.function_at(address);
        require(chili::PadRumbleHook::installed() && hooked != nullptr && hooked != &empty_function, "the host function is over the game's");

        // A made-up Xbox-style pad in slot 0 and motors that only write down what they were sent.
        struct Motors { unsigned pad; std::uint16_t slow, fast; };
        std::vector<Motors> motors;
        std::vector<std::string> log;
        double now = 50.0;
        auto &input = chili::PcInput::instance();
        input.configure(chili::PcInputConfig::defaults());
        chili::PcInputHost host;
        host.focused = [] { return true; };
        host.key_down = [](int) { return false; };
        host.pad = [](unsigned slot, XINPUT_STATE &state) { state = {}; return slot == 0u; };
        host.seconds = [&now] { return now; };
        host.pad_worker = false;
        host.vibrate = [&motors](unsigned index, std::uint16_t slow, std::uint16_t fast) { motors.push_back({index, slow, fast}); return true; };
        host.log = [&log](const std::string &line) { log.push_back(line); };
        input.set_host(std::move(host));
        input.scan_pads();
        now += 1.0 / 30.0;
        (void)input.sample();
        require(input.pad_text() == "xbox slot 0" && motors.empty(), "a pad is in use, its motors untouched");

        // The game's call: a0 the pad object, a1 the motor, f12 the strength, ra the caller.
        const auto call = [&](std::uint32_t motor, float strength) {
            psprecomp::AllegrexContext ctx{};
            ctx.gpr[4] = 0x08DF2650u; ctx.gpr[5] = motor; ctx.fpr[12] = strength; ctx.gpr[31] = 0x088C7A64u; ctx.pc = address;
            hooked(runtime, ctx);
            return ctx.pc;
        };
        const auto level = [](float value) { return static_cast<std::uint16_t>(std::lround(value * 65535.0f)); };
        require(call(1u, 0.75f) == 0x088C7A64u && original_runs == 1u, "the game's own function still runs and returns to its caller");
        require(input.report().game_vibration_calls == 1u && motors.size() == 1u && motors[0].pad == 0u && motors[0].slow == level(0.75f) && motors[0].fast == 0u,
            "motor 1 at 0.75 runs the pad's large motor");
        (void)call(0u, 0.5f);
        require(motors.size() == 2u && motors[1].slow == level(0.75f) && motors[1].fast == level(0.5f), "motor 0 runs the small one beside it");
        (void)call(1u, 0.0f);
        (void)call(0u, 0.0f);
        require(motors.size() == 4u && motors[3].slow == 0u && motors[3].fast == 0u && original_runs == 4u && input.report().game_vibration_calls == 4u,
            "both off again; every call counted");
        require(log.size() >= 5u && log[1] == "vibration: the game set motor 1 to 0.75", "the calls are logged with their values");
        input.release();
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
