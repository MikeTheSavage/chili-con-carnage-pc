#include "../host/partition_memory.hpp"
#include <iostream>
#include <stdexcept>
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::uint32_t call(psprecomp::Runtime &rt, std::uint32_t nid, std::uint32_t arg = 0) {
    rt.cpu().gpr[4] = arg;
    rt.invoke_import("SysMemUserForUser", nid, rt.cpu());
    return rt.cpu().gpr[2];
}
std::uint32_t allocate(psprecomp::Runtime &rt, unsigned type, std::uint32_t size, std::uint32_t address = 0) {
    rt.cpu().gpr[6] = type; rt.cpu().gpr[7] = size; rt.cpu().gpr[8] = address;
    return call(rt, 0x237DBD4Fu, 2u);
}
int main() {
    try {
        psprecomp::Runtime rt;
        chili::PartitionMemory memory(rt, 0x09000000u, 0x09002000u);
        require(memory.reserve_stack(0x1000u) == 0x09001000u, "Stack reservation failed");
        const auto low = allocate(rt, 0, 1u);
        require(call(rt, 0x9D9A5BA1u, low) == 0x09000000u, "Low address mismatch");
        const auto high = allocate(rt, 1, 0x101u);
        require(call(rt, 0x9D9A5BA1u, high) == 0x09000E00u, "High address or rounding mismatch");
        const auto fixed = allocate(rt, 2, 0x100u, 0x09000800u);
        require(call(rt, 0x9D9A5BA1u, fixed) == 0x09000800u, "Fixed allocation mismatch");
        require(allocate(rt, 2, 0x100u, 0x09000800u) == 0x80020190u, "Overlapping block accepted");
        require(allocate(rt, 0, 0x1001u) == 0x80020190u, "Stack overlap accepted");
        require(call(rt, 0xF919F628u) == 0xC00u, "Free total mismatch");
        require(call(rt, 0xA291F107u) == 0x700u, "Largest gap mismatch");
        require(call(rt, 0xB6D61D02u, fixed) == 0u, "Free failed");
        require(call(rt, 0xB6D61D02u, low) == 0u, "Low free failed");
        require(call(rt, 0xB6D61D02u, high) == 0u, "High free failed");
        require(call(rt, 0xA291F107u) == 0x1000u, "Adjacent gaps did not merge");
        require(call(rt, 0xB6D61D02u, low) == 0x800200CBu, "Double free accepted");
        require(call(rt, 0x9D9A5BA1u, low) == 0x800200CBu, "Freed UID remained valid");
        const auto aligned_low = allocate(rt, 3, 0x100u, 0x400u);
        require(call(rt, 0x9D9A5BA1u, aligned_low) == 0x09000000u, "Aligned low allocation mismatch");
        const auto aligned_high = allocate(rt, 4, 0x100u, 0x400u);
        require(call(rt, 0x9D9A5BA1u, aligned_high) == 0x09000C00u, "Aligned high allocation mismatch");
        require(allocate(rt, 3, 0x100u, 3u) == 0x800200D3u, "Non-power-of-two alignment accepted");
        std::cout << "PASS: partition low/high/fixed allocation, rounding, fragmentation, coalescing and stack exclusion\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
