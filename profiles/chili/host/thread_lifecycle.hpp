#pragma once
#include "thread_runtime.hpp"
#include <vector>

namespace chili {
// Owned by the host alongside ThreadRuntime. The scheduler calls on_ended()
// before switching away from a normally returned or explicitly exited thread.
class ThreadLifecycle {
public:
    ThreadLifecycle(psprecomp::Runtime &, ThreadRuntime &,
                    std::function<bool(std::uint32_t)> release_stack,
                    const std::uint32_t *compiled_sdk_version = nullptr);
    ~ThreadLifecycle();
    ThreadLifecycle(const ThreadLifecycle &) = delete;
    ThreadLifecycle &operator=(const ThreadLifecycle &) = delete;
    void on_ended(std::int32_t target, std::uint32_t status);
private:
    struct Join { std::int32_t waiter; std::uint32_t timeout; std::uint64_t deadline; };
    void join(psprecomp::AllegrexContext &);
    void exit(psprecomp::AllegrexContext &);
    void remove(psprecomp::AllegrexContext &);
    psprecomp::Runtime &runtime_;
    ThreadRuntime &threads_;
    std::function<bool(std::uint32_t)> release_stack_;
    const std::uint32_t *compiled_sdk_version_;
    std::map<std::int32_t, std::vector<Join>> joins_;
};
}
