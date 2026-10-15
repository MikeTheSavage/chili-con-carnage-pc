#pragma once
#include "psprecomp/runtime.hpp"
#include <memory>
namespace chili {
class SasRuntime {
public:
    explicit SasRuntime(psprecomp::Runtime &);
    ~SasRuntime();
    bool load();
    bool unload();
    bool loaded() const noexcept;
    SasRuntime(const SasRuntime &) = delete;
    SasRuntime &operator=(const SasRuntime &) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
