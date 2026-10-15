#include "partition_memory.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <iostream>

namespace chili {
PartitionMemory::PartitionMemory(psprecomp::Runtime &runtime, std::uint32_t begin, std::uint32_t end)
    : runtime_(runtime) {
    begin = (begin + 255u) & ~255u;
    end &= ~255u;
    if (end <= begin || !runtime.memory().contains(begin, end - begin))
        throw psprecomp::Error("Invalid Chili partition arena");
    free_.emplace(begin, end - begin);
    runtime.register_hle("SysMemUserForUser", 0x237DBD4Fu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::cout << "[sysmem] request partition=" << ctx.gpr[4] << " type=" << ctx.gpr[6]
                      << " bytes=" << ctx.gpr[7] << '\n';
            if (ctx.gpr[4] != 2u || ctx.gpr[6] > 4u || !ctx.gpr[7] ||
                ctx.gpr[7] > 0x02000000u ||
                (ctx.gpr[6] >= 3u && (!ctx.gpr[8] || (ctx.gpr[8] & (ctx.gpr[8] - 1u))))) {
                ctx.set_gpr(2u, 0x800200D3u); return;
            }
            const auto size = (ctx.gpr[7] + 255u) & ~255u;
            const auto address = allocate(size, ctx.gpr[6], ctx.gpr[8]);
            if (!address) { ctx.set_gpr(2u, 0x80020190u); return; }
            const auto uid = next_uid_++;
            blocks_.emplace(uid, Block{address, size});
            std::cout << "[sysmem] allocate uid=" << uid << " bytes=" << size
                      << " address=" << psprecomp::hex32(address) << '\n';
            ctx.set_gpr(2u, uid);
        });
    runtime.register_hle("SysMemUserForUser", 0x9D9A5BA1u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto block = blocks_.find(ctx.gpr[4]);
            ctx.set_gpr(2u, block == blocks_.end() ? 0x800200CBu : block->second.address);
        });
    runtime.register_hle("SysMemUserForUser", 0xB6D61D02u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto block = blocks_.find(ctx.gpr[4]);
            if (block == blocks_.end()) { ctx.set_gpr(2u, 0x800200CBu); return; }
            release(block->second);
            blocks_.erase(block);
            ctx.set_gpr(2u, 0u);
        });
    runtime.register_hle("SysMemUserForUser", 0xA291F107u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::uint32_t largest = 0;
            for (const auto &[address, size] : free_) largest = std::max(largest, size);
            ctx.set_gpr(2u, largest);
        });
    runtime.register_hle("SysMemUserForUser", 0xF919F628u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            std::uint32_t total = 0;
            for (const auto &[address, size] : free_) total += size;
            ctx.set_gpr(2u, total);
        });
}

std::uint32_t PartitionMemory::allocate(std::uint32_t size, unsigned type, std::uint32_t requested) {
    auto chosen = free_.end();
    std::uint32_t address = 0;
    const auto alignment = type >= 3u ? std::max(256u, requested) : 256u;
    const bool high = type == 1u || type == 4u;
    if (type == 2u) requested = psprecomp::GuestMemory::canonical(requested) & ~255u;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size) continue;
        const auto end = it->first + it->second;
        if (type == 2u) {
            if (requested < it->first || requested > end - size) continue;
            address = requested;
        } else {
            if (high) address = (end - size) & ~(alignment - 1u);
            else {
                const auto aligned = (static_cast<std::uint64_t>(it->first) + alignment - 1u) &
                    ~static_cast<std::uint64_t>(alignment - 1u);
                if (aligned > end - size) continue;
                address = static_cast<std::uint32_t>(aligned);
            }
            if (address < it->first) continue;
        }
        chosen = it;
        if (!high) break;
    }
    if (chosen == free_.end()) return 0;
    const auto begin = chosen->first, end = begin + chosen->second;
    free_.erase(chosen);
    if (address > begin) free_.emplace(begin, address - begin);
    if (address + size < end) free_.emplace(address + size, end - address - size);
    return address;
}

void PartitionMemory::release(Block block) {
    auto next = free_.lower_bound(block.address);
    if (next != free_.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == block.address) {
            block.address = previous->first;
            block.size += previous->second;
            free_.erase(previous);
        }
    }
    next = free_.lower_bound(block.address);
    if (next != free_.end() && block.address + block.size == next->first) {
        block.size += next->second;
        free_.erase(next);
    }
    free_.emplace(block.address, block.size);
}

std::uint32_t PartitionMemory::reserve_stack(std::uint32_t size) {
    return reserve_region(size, 256u, true);
}

std::uint32_t PartitionMemory::reserve_region(std::uint32_t size, std::uint32_t alignment, bool high) {
    if (!size || size > 0x02000000u) return 0;
    if (!alignment || (alignment & (alignment - 1u))) return 0;
    size = (size + 255u) & ~255u;
    const auto address = allocate(size, high ? 4u : 3u, alignment);
    if (address) regions_.emplace(address, size);
    return address;
}

bool PartitionMemory::release_region(std::uint32_t address) {
    const auto found = regions_.find(address);
    if (found == regions_.end()) return false;
    release(Block{address, found->second});
    regions_.erase(found);
    return true;
}
}
