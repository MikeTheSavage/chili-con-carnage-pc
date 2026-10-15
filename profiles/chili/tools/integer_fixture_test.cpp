#include "psprecomp/runtime.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        psprecomp::Runtime rt;
        psprecomp::register_generated_functions(rt);
        auto &ctx = rt.cpu();
        ctx.gpr[4] = 7u; ctx.gpr[31] = 0u;
        rt.run(0x08804000u, 100u);
        if (ctx.gpr[2] != 6u) throw std::runtime_error("ADDI negative immediate failed");
        ctx.gpr[4] = 0x80000000u; ctx.gpr[2] = 0xABCDEFFFu;
        rt.run(0x08804000u, 100u);
        if (ctx.gpr[2] != 0xABCDEFFFu || rt.stop_reason().find("overflow") == std::string::npos)
            throw std::runtime_error("ADDI overflow did not preserve destination and trap");
        const std::uint64_t expected[]{0xFFFFFFFFFFFFFFFCull, 0x00000006FFFFFFFCull,
                                       38ull, 0xFFFFFFF900000026ull};
        for (std::uint32_t i = 0; i < 4u; ++i) {
            ctx.gpr[4] = 0xFFFFFFFDu; ctx.gpr[5] = 7u;
            ctx.hi = 0u; ctx.lo = 17u; ctx.gpr[31] = 0u;
            rt.run(0x08804020u + i * 0x20u, 100u);
            const auto result = (static_cast<std::uint64_t>(ctx.hi) << 32u) | ctx.lo;
            if (result != expected[i]) throw std::runtime_error("Multiply accumulate signedness/result failed");
        }
        ctx.gpr[4] = 1u; ctx.gpr[5] = 1u; ctx.hi = 0xFFFFFFFFu; ctx.lo = 0xFFFFFFFFu;
        rt.run(0x08804020u, 100u);
        if (ctx.hi || ctx.lo) throw std::runtime_error("MADD did not wrap the 64-bit accumulator");
        std::cout << "PASS: native ADDI, overflow preservation, signed/unsigned multiply accumulation, wrapping\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
