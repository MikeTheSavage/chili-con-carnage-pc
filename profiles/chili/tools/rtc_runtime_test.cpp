#include "../host/rtc_runtime.hpp"
#include <iostream>
#include <stdexcept>
#include <chrono>
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        psprecomp::Runtime rt;
        chili::register_rtc(rt);
        constexpr auto date = 0x09000000u, out = date + 32u;
        rt.memory().store16(date, 1970u); rt.memory().store16(date + 2u, 1u); rt.memory().store16(date + 4u, 1u);
        rt.cpu().gpr[4] = date; rt.cpu().gpr[5] = out;
        rt.invoke_import("sceRtc", 0x27C4594Cu, rt.cpu());
        require(rt.cpu().gpr[2] == 0u && rt.memory().load32(out) == 0u, "Unix epoch mismatch");
        rt.memory().store16(date, 2000u); rt.memory().store16(date + 2u, 2u); rt.memory().store16(date + 4u, 29u);
        rt.invoke_import("sceRtc", 0x27C4594Cu, rt.cpu());
        require(rt.cpu().gpr[2] == 0u && rt.memory().load32(out) == 951782400u, "Leap-day mismatch");
        rt.memory().store16(date, 2001u);
        rt.invoke_import("sceRtc", 0x27C4594Cu, rt.cpu());
        require(rt.cpu().gpr[2] != 0u && rt.memory().load32(out) == 951782400u, "Invalid date wrote output");
        rt.cpu().gpr[4] = date;
        rt.invoke_import("sceRtc", 0xE7C27D1Bu, rt.cpu());
        require(rt.cpu().gpr[2] == 0u && rt.memory().load16(date) >= 1u &&
            rt.memory().load16(date + 2u) <= 12u && rt.memory().load32(date + 12u) < 1000000u, "Local clock layout mismatch");
        rt.cpu().gpr[4] = out;
        rt.invoke_import("sceRtc", 0x3F7AD767u, rt.cpu());
        const auto tick = rt.memory().load32(out) | (static_cast<std::uint64_t>(rt.memory().load32(out + 4u)) << 32u);
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto delta = static_cast<std::int64_t>(tick - 62135596800000000ull) - now;
        require(rt.cpu().gpr[2] == 0u && delta <= 0 && delta > -5000000ll, "RTC epoch or tick scale mismatch");
        std::cout << "PASS: RTC Unix epoch, leap date, invalid-date preservation, local layout and current tick\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
