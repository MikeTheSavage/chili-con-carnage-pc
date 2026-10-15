#pragma once
#include "psprecomp/runtime.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace chili {
class ThreadRuntime;

// Isolated preparation; utility/native users must share this same resource.
class VolatileMemoryRuntime {
public:
    static constexpr std::uint32_t address = 0x08400000u;
    static constexpr std::uint32_t bytes = 0x00400000u;
    static constexpr std::uint32_t resume_pc = 0x08000024u;
    enum class Context { Normal, DispatchDisabled, Interrupt, UnknownCannotWait };
    using ContextQuery = std::function<Context()>;

    explicit VolatileMemoryRuntime(psprecomp::Runtime &, ThreadRuntime * = nullptr, ContextQuery = {});
    ~VolatileMemoryRuntime();
    VolatileMemoryRuntime(const VolatileMemoryRuntime &) = delete;
    VolatileMemoryRuntime &operator=(const VolatileMemoryRuntime &) = delete;
    bool locked() const;
    std::int32_t last_acquiring_uid() const;
    std::size_t waiting() const;

    // Host utility ownership hooks. No guest result, UI decision or scheduling
    // is fabricated. A native release makes actual blocked threads ready.
    bool try_acquire_native(std::uint64_t token);
    bool release_native(std::uint64_t token);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
