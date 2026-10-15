#pragma once
#include "thread_runtime.hpp"
#include <vector>
namespace chili {
class EventFlags {
public:
    EventFlags(psprecomp::Runtime &, ThreadRuntime &);
private:
    struct Waiter { std::int32_t thread; std::uint32_t bits, mode, output, timeout; std::uint64_t deadline; };
    struct Flag { std::string name; std::uint32_t attributes, initial, pattern; std::vector<Waiter> waiters; };
    void create(psprecomp::AllegrexContext &);
    void wait(psprecomp::AllegrexContext &, bool poll);
    void modify(psprecomp::AllegrexContext &, bool set);
    void remove(psprecomp::AllegrexContext &, bool cancel);
    void status(psprecomp::AllegrexContext &);
    bool match(Flag &, const Waiter &);
    void finish(const Waiter &, std::uint32_t result);
    psprecomp::Runtime &runtime_;
    ThreadRuntime &threads_;
    std::map<std::uint32_t, Flag> flags_;
    std::uint32_t next_uid_{0x40000u};
};
}
