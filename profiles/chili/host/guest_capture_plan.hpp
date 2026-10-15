#pragma once
#include "psprecomp/runtime.hpp"
#include "psprecomp/common.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
namespace chili {
// Opt-in post-stop snapshots of actual guest requests and owned resource bytes.
inline void dump_guest_capture_plan(psprecomp::Runtime &runtime) {
    const char *plan_path=std::getenv("PSPRECOMP_GUEST_DUMP_PLAN");
    if(!plan_path || !*plan_path)return;
    const char *prefix=std::getenv("PSPRECOMP_GUEST_DUMP_PREFIX");
    if(!prefix || !*prefix)throw psprecomp::Error("Guest capture plan requires PSPRECOMP_GUEST_DUMP_PREFIX");
    std::ifstream file{std::filesystem::path(plan_path)};
    if(!file)throw psprecomp::Error("Cannot open guest capture plan");
    struct Region{std::uint32_t address;std::size_t size;std::string label;};
    std::vector<Region> regions;std::size_t total{};std::string line;
    auto number=[](const std::string &value){std::size_t end{};const auto n=std::stoull(value,&end,0);if(end!=value.size())throw psprecomp::Error("Invalid guest capture number");return n;};
    while(std::getline(file,line)) {
        if(line.size()>256 || regions.size()>=32)throw psprecomp::Error("Guest capture plan size limit exceeded");
        std::istringstream input(line);std::string a,b,label,extra;
        if(!(input>>a) || a[0]=='#')continue;
        if(!(input>>b>>label) || input>>extra)throw psprecomp::Error("Guest capture requires address size label");
        auto address=number(a[0]=='@'?a.substr(1):a);const auto size=number(b);
        if(a[0]=='@') {
            if(address>0xFFFFFFFFull || (address&3u) || !runtime.memory().contains(static_cast<std::uint32_t>(address),4u))
                throw psprecomp::Error("Invalid guest capture pointer field");
            address=runtime.memory().load32(static_cast<std::uint32_t>(address));
        }
        if(address>0xFFFFFFFFull || !size || size>4u*1024u*1024u-total || label.empty() || label.size()>64 ||
           label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos ||
           !runtime.memory().contains(static_cast<std::uint32_t>(address),static_cast<std::size_t>(size)))
            throw psprecomp::Error("Invalid guest capture range, label or aggregate4MiB limit");
        for(const auto &r:regions)if(r.label==label)throw psprecomp::Error("Duplicate guest capture label");
        regions.push_back({static_cast<std::uint32_t>(address),static_cast<std::size_t>(size),label});total+=size;
    }
    if(file.bad())throw psprecomp::Error("Cannot read guest capture plan");
    for(const auto &region:regions) {
        std::vector<std::uint8_t> bytes(region.size);runtime.memory().copy_out(region.address,bytes);
        const std::filesystem::path path(std::string(prefix)+"_"+region.label+".bin");
        std::ofstream output(path,std::ios::binary|std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));output.close();
        std::cout<<"[guest-plan] address="<<psprecomp::hex32(region.address)<<" bytes="<<region.size<<" label="<<region.label<<" saved="<<!output.fail()<<'\n';
    }
}
}
