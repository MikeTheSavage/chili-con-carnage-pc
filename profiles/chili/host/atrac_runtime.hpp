#pragma once
#include "psprecomp/runtime.hpp"
#include <memory>
namespace chili {
class AvRuntime;
class ThreadRuntime;
class AtracRuntime {
public:
    static constexpr std::uint32_t decode_resume_trampoline=0x08000020u;
    AtracRuntime(psprecomp::Runtime &,AvRuntime &,ThreadRuntime * = nullptr);
    ~AtracRuntime();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
