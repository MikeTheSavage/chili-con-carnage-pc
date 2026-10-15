#pragma once
#include "partition_memory.hpp"
#include "thread_runtime.hpp"
#include <vector>
namespace chili {
class FixedPools {
public:
    FixedPools(psprecomp::Runtime &, PartitionMemory &, ThreadRuntime &);
private:
    struct Waiter { std::int32_t thread; std::uint32_t output, timeout; std::uint64_t deadline; };
    struct Pool {
        std::string name;
        std::uint32_t address, block_size, stride, attributes;
        std::vector<bool> allocated;
        std::vector<Waiter> waiters;
    };
    void create(psprecomp::AllegrexContext &);
    void allocate(psprecomp::AllegrexContext &, bool poll);
    void free(psprecomp::AllegrexContext &);
    void clear(psprecomp::AllegrexContext &, bool erase);
    void status(psprecomp::AllegrexContext &);
    bool take(Pool &, std::uint32_t output);
    void finish_wait(const Waiter &, std::uint32_t result);
    psprecomp::Runtime &runtime_;
    PartitionMemory &memory_;
    ThreadRuntime &threads_;
    std::map<std::uint32_t, Pool> pools_;
    std::uint32_t next_uid_{0x20000u};
};
}
