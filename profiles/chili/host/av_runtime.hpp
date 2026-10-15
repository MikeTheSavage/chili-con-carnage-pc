#pragma once
#include "psprecomp/runtime.hpp"
#include <array>
#include <functional>
namespace chili {
class SasRuntime;
class AvRuntime {
public:
    explicit AvRuntime(psprecomp::Runtime &, SasRuntime *sas = nullptr);
    void set_unload_guard(unsigned module, std::function<bool()> guard) {
        if(module<unload_guards_.size())unload_guards_[module]=std::move(guard);
    }
    bool loaded(unsigned module) const {return module<loaded_.size() && loaded_[module];}
private:
    void module(psprecomp::AllegrexContext &,bool load);
    SasRuntime *sas_;
    psprecomp::Runtime &runtime_;std::array<bool,8> loaded_{};
    std::array<std::function<bool()>,8> unload_guards_{};
};
}
