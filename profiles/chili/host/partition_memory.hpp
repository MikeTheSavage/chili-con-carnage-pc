#pragma once
#include "psprecomp/runtime.hpp"
#include <map>

namespace chili {
// One arena for partition blocks and thread stacks. Loaded ELF segments and
// the bootstrap stack are excluded by the caller's arena boundaries.
class PartitionMemory {
public:
    PartitionMemory(psprecomp::Runtime &, std::uint32_t begin, std::uint32_t end);
    std::uint32_t reserve_stack(std::uint32_t size);
    std::uint32_t reserve_region(std::uint32_t size, std::uint32_t alignment, bool high);
    bool release_region(std::uint32_t address);
private:
    struct Block { std::uint32_t address, size; };
    std::uint32_t allocate(std::uint32_t size, unsigned type, std::uint32_t address);
    void release(Block block);
    psprecomp::Runtime &runtime_;
    std::map<std::uint32_t, std::uint32_t> free_;
    std::map<std::uint32_t, Block> blocks_;
    std::map<std::uint32_t, std::uint32_t> regions_;
    std::uint32_t next_uid_{0x1000u};
};
}
