#include "psprecomp/runtime.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        psprecomp::Runtime runtime;
        psprecomp::register_generated_functions(runtime);
        const float input[4]{4.0f, -2.0f, 7.0f, 1.0f};
        const float expected[4][4]{
            {-2.0f, 4.0f, 1.0f, 7.0f}, {1.0f, -2.0f, 7.0f, 4.0f},
            {4.0f, -2.0f, 7.0f, 1.0f}, {4.0f, 7.0f, -2.0f, 1.0f}};
        for (std::uint32_t index = 0; index < 4; ++index) {
            for (bool mask_and_saturate : {false, true}) {
                auto &ctx = runtime.cpu();
                ctx.write_vfpu_vector(input, 0u, 4u);
                ctx.vfpu_ctrl[0] = 0u; // Source swizzle would broadcast lane 0; sorts ignore it.
                ctx.vfpu_ctrl[1] = 0u;
                ctx.vfpu_ctrl[2] = mask_and_saturate ? (1u << 9u) | 3u : 0u;
                ctx.gpr[31] = 0u;
                runtime.run(0x08804000u + index * 0x20u, 100u);
                float result[4]{};
                ctx.read_vfpu_vector(result, 0u, 4u);
                for (unsigned lane = 0; lane < 4; ++lane) {
                    float wanted = expected[index][lane];
                    if (mask_and_saturate && lane == 0u)
                        wanted = wanted < -1.0f ? -1.0f : wanted > 1.0f ? 1.0f : wanted;
                    if (mask_and_saturate && lane == 1u) wanted = input[1];
                    if (result[lane] != wanted) throw std::runtime_error("VFPU sort result mismatch");
                }
                if (ctx.vfpu_ctrl[0] != 0xE4u || ctx.vfpu_ctrl[1] != 0xE4u || ctx.vfpu_ctrl[2] != 0u)
                    throw std::runtime_error("VFPU sort did not consume prefixes");
            }
        }
        std::cout << "PASS: native AOT sorts, source-prefix handling, destination mask/saturation, aliasing\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
