#pragma once
#include "thread_runtime.hpp"
#include <vector>
namespace chili {
class Semaphores {
public:
    Semaphores(psprecomp::Runtime &,ThreadRuntime &);
private:
    struct Waiter {std::int32_t thread;std::uint32_t count,timeout;std::uint64_t deadline;};
    struct Sema {std::string name;std::uint32_t attr,initial,count,maximum;std::vector<Waiter> waiters;};
    void create(psprecomp::AllegrexContext &);
    void wait(psprecomp::AllegrexContext &,bool poll);
    void signal(psprecomp::AllegrexContext &);
    void remove(psprecomp::AllegrexContext &,bool cancel);
    void status(psprecomp::AllegrexContext &);
    void finish(const Waiter &,std::uint32_t result);
    void drain_after_timeout(Sema &);
    psprecomp::Runtime &runtime_;ThreadRuntime &threads_;
    std::map<std::uint32_t,Sema> semaphores_;std::uint32_t next_uid_{0x60000u};
};
}
