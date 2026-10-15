#pragma once
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
namespace chili {
// XXH64 (Yann Collet's public algorithm), reimplemented so no dependency is added.
inline std::uint64_t xxh64(std::span<const std::uint8_t> data,std::uint64_t seed=0u) noexcept {
    constexpr std::uint64_t p1=0x9E3779B185EBCA87ull,p2=0xC2B2AE3D27D4EB4Full,p3=0x165667B19E3779F9ull,
        p4=0x85EBCA77C2B2AE63ull,p5=0x27D4EB2F165667C5ull;
    const auto read64=[](const std::uint8_t *p){std::uint64_t v;std::memcpy(&v,p,8u);return v;};
    const auto read32=[](const std::uint8_t *p){std::uint32_t v;std::memcpy(&v,p,4u);return v;};
    const auto round=[](std::uint64_t acc,std::uint64_t input){return std::rotl(acc+input*p2,31)*p1;};
    const std::uint8_t *p=data.data();std::size_t left=data.size();std::uint64_t h;
    if(left>=32u){
        std::uint64_t v1=seed+p1+p2,v2=seed+p2,v3=seed,v4=seed-p1;
        do{
            v1=round(v1,read64(p));v2=round(v2,read64(p+8));v3=round(v3,read64(p+16));v4=round(v4,read64(p+24));
            p+=32;left-=32u;
        }while(left>=32u);
        h=std::rotl(v1,1)+std::rotl(v2,7)+std::rotl(v3,12)+std::rotl(v4,18);
        for(const auto v:{v1,v2,v3,v4})h=(h^round(0u,v))*p1+p4;
    }else h=seed+p5;
    h+=data.size();
    for(;left>=8u;p+=8,left-=8u)h=std::rotl(h^round(0u,read64(p)),27)*p1+p4;
    if(left>=4u){h=std::rotl(h^(read32(p)*p1),23)*p2+p3;p+=4;left-=4u;}
    for(;left;++p,--left)h=std::rotl(h^(*p*p5),11)*p1;
    h^=h>>33u;h*=p2;h^=h>>29u;h*=p3;h^=h>>32u;return h;
}

// Guest state that must repeat between deterministic runs: main RAM, the
// scratchpad, the running thread's CPU context and the scheduler clock. VRAM is
// left out on purpose; a hardware renderer does not write it.
struct StateHash {std::uint64_t combined{},ram{},cpu{};};
// A main-RAM byte range left out of the hash: for guest fields known to carry
// host facts (free disk space in the savedata size reply, for example).
struct HashExclusion {std::uint32_t address{},length{};};
// Hash of main RAM bytes [offset, offset+length) minus the exclusions, which
// must be sorted by address and must not overlap.
inline std::uint64_t hash_ram_range(const psprecomp::GuestMemory &memory,std::size_t offset,std::size_t length,
    std::span<const HashExclusion> exclusions={}) noexcept {
    const auto &ram=memory.bytes();
    const auto end=std::min(ram.size(),offset+length);
    std::uint64_t hash=0u;std::size_t cursor=std::min(offset,end);
    for(const auto &skip:exclusions){
        const std::size_t first=psprecomp::GuestMemory::canonical(skip.address)-psprecomp::GuestMemory::kPhysicalBase;
        if(first>=end)break;
        if(first+skip.length<=cursor)continue;
        if(first>cursor)hash=xxh64({ram.data()+cursor,first-cursor},hash);
        cursor=std::min(end,std::max(cursor,first+skip.length));
    }
    return xxh64({ram.data()+cursor,end-cursor},hash);
}
inline std::uint64_t hash_cpu_context(const psprecomp::AllegrexContext &c) noexcept {
    std::array<std::uint32_t,32u+3u+32u+1u+128u+16u> words{};
    std::size_t n=0;
    for(const auto v:c.gpr)words[n++]=v;
    words[n++]=c.hi;words[n++]=c.lo;words[n++]=c.pc;
    for(const auto v:c.fpr)words[n++]=std::bit_cast<std::uint32_t>(v);
    words[n++]=c.fcr31;
    for(const auto v:c.vfpu)words[n++]=std::bit_cast<std::uint32_t>(v);
    for(const auto v:c.vfpu_ctrl)words[n++]=v;
    return xxh64({reinterpret_cast<const std::uint8_t*>(words.data()),n*sizeof(std::uint32_t)});
}
inline StateHash hash_guest_state(const psprecomp::GuestMemory &memory,const psprecomp::AllegrexContext &cpu,std::uint64_t clock_us,
    std::span<const HashExclusion> exclusions={}) noexcept {
    StateHash result;
    result.ram=hash_ram_range(memory,0u,memory.bytes().size(),exclusions);
    result.cpu=hash_cpu_context(cpu);
    std::uint64_t scratchpad{};
    if(const auto *bytes=memory.raw_pointer(psprecomp::GuestMemory::kScratchpadBase,psprecomp::GuestMemory::kScratchpadSize))
        scratchpad=xxh64({bytes,psprecomp::GuestMemory::kScratchpadSize});
    const std::array<std::uint64_t,4> parts{result.ram,result.cpu,scratchpad,clock_us};
    result.combined=xxh64({reinterpret_cast<const std::uint8_t*>(parts.data()),sizeof(parts)});
    return result;
}
// One hash per MiB of main RAM, for narrowing down where two runs diverge.
inline std::vector<std::uint64_t> hash_ram_regions(const psprecomp::GuestMemory &memory,std::span<const HashExclusion> exclusions={}){
    constexpr std::size_t region=1024u*1024u;
    std::vector<std::uint64_t> out;
    for(std::size_t offset=0;offset<memory.bytes().size();offset+=region)out.push_back(hash_ram_range(memory,offset,region,exclusions));
    return out;
}
}
