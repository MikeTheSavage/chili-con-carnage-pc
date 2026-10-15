#include "rtc_runtime.hpp"
#include <chrono>
#include <ctime>
namespace chili {
void register_rtc(psprecomp::Runtime &runtime, UnixMicrosecondsSource source) {
    runtime.register_hle("sceRtc", 0x3F7AD767u,
        [source](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto address = ctx.gpr[4];
            if (!address || !rt.memory().contains(address, 8u)) { ctx.set_gpr(2u, 0x800200D3u); return; }
            const auto tick = (source ? source() : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())) + rtc_unix_epoch_offset;
            rt.memory().store32(address, static_cast<std::uint32_t>(tick));
            rt.memory().store32(address + 4u, static_cast<std::uint32_t>(tick >> 32u));
            ctx.set_gpr(2u, 0u);
        });
    runtime.register_hle("sceRtc", 0x27C4594Cu,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const auto address = ctx.gpr[4], output = ctx.gpr[5];
                if (!address || !output || !rt.memory().contains(address, 16u) || !rt.memory().contains(output, 4u)) {
                    ctx.set_gpr(2u, 0x800200D3u); return;
                }
                const auto year = rt.memory().load16(address);
                const auto month = rt.memory().load16(address + 2u);
                const auto day = rt.memory().load16(address + 4u);
                const auto hour = rt.memory().load16(address + 6u);
                const auto minute = rt.memory().load16(address + 8u);
                const auto second = rt.memory().load16(address + 10u);
                const std::chrono::year_month_day date{std::chrono::year{year},
                    std::chrono::month{month}, std::chrono::day{day}};
                if (year < 1u || year > 9999u || !date.ok() || hour > 23u || minute > 59u ||
                    second > 59u || rt.memory().load32(address + 12u) > 999999u) {
                    ctx.set_gpr(2u, 0x800200D3u); return;
                }
                const auto unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::sys_days{date}.time_since_epoch()).count() + hour * 3600ll + minute * 60ll + second;
                if (unix_seconds < 0 || unix_seconds > 0xFFFFFFFFll) {
                    ctx.set_gpr(2u, 0x800200D3u); return;
                }
                rt.memory().store32(output, static_cast<std::uint32_t>(unix_seconds));
                ctx.set_gpr(2u, 0u);
            });
    runtime.register_hle("sceRtc", 0xE7C27D1Bu,
            [source](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const auto address = ctx.gpr[4];
                if (!address || !rt.memory().contains(address, 16u)) {
                    ctx.set_gpr(2u, 0x800200D3u); return;
                }
                if (source) {
                    const std::chrono::sys_time<std::chrono::microseconds> instant{std::chrono::microseconds{source()}};
                    const auto day = std::chrono::floor<std::chrono::days>(instant);
                    const std::chrono::year_month_day date{day};
                    const std::chrono::hh_mm_ss time{instant - day};
                    const unsigned values[] = {static_cast<unsigned>(static_cast<int>(date.year())),
                        static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                        static_cast<unsigned>(time.hours().count()), static_cast<unsigned>(time.minutes().count()),
                        static_cast<unsigned>(time.seconds().count())};
                    for (unsigned i = 0; i < 6; ++i) rt.memory().store16(address + 2u * i, static_cast<std::uint16_t>(values[i]));
                    rt.memory().store32(address + 12u, static_cast<std::uint32_t>(time.subseconds().count()));
                    ctx.set_gpr(2u, 0u);
                    return;
                }
                const auto now = std::chrono::system_clock::now();
                const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
                const auto epoch = std::chrono::system_clock::to_time_t(seconds);
                std::tm local{};
                if (localtime_s(&local, &epoch) != 0) { ctx.set_gpr(2u, 0x800200D3u); return; }
                const unsigned values[] = {static_cast<unsigned>(local.tm_year + 1900),
                    static_cast<unsigned>(local.tm_mon + 1), static_cast<unsigned>(local.tm_mday),
                    static_cast<unsigned>(local.tm_hour), static_cast<unsigned>(local.tm_min),
                    static_cast<unsigned>(local.tm_sec)};
                for (unsigned i = 0; i < 6; ++i) rt.memory().store16(address + 2u * i, values[i]);
                rt.memory().store32(address + 12u, static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count()));
                ctx.set_gpr(2u, 0u);
            });
}
}
