#pragma once
#include "psprecomp/runtime.hpp"
#include <iostream>
namespace chili {
// Development-unit LEDs/switches have no physical PC equivalent. Retain the
// output latch for diagnostics and expose the production PSP input value.
class DebugIO {
public:
    explicit DebugIO(psprecomp::Runtime &rt) {
        rt.register_hle("UtilsForUser",0x6AD345D7u,[this](auto &,auto &c) {
            output_=c.gpr[4];
            std::cout << "[debug-io] GPO=" << output_ << '\n';
            // This is a void firmware call; preserve the result registers.
        });
        rt.register_hle("UtilsForUser",0x37FB5C42u,[](auto &,auto &c) { c.set_gpr(2,0u); });
    }
    std::uint32_t output() const { return output_; }
private:
    std::uint32_t output_{};
};
}
