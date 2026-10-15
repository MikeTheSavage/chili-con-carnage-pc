#pragma once
#include "psprecomp/guest_memory.hpp"
#include <vector>
namespace chili {
// Track earlier destination writes while preflighting one draw batch. A batch
// with no read-after-write texture dependencies samples its original texture
// bytes regardless of the native renderer's immediate write implementation.
// Dependent feedback remains strict; no stale texture snapshot is substituted.
class GeFeedbackGuard {
    std::vector<unsigned char> changed_;
    static bool vram(unsigned address){
        return (psprecomp::GuestMemory::canonical(address)&0xFF000000u)==0x04000000u;
    }
    static unsigned offset(unsigned address){
        return psprecomp::GuestMemory::canonical(address)&(psprecomp::GuestMemory::kVramSize-1u);
    }
public:
    GeFeedbackGuard():changed_(psprecomp::GuestMemory::kVramSize,0){}
    bool readable(unsigned address,unsigned size) const {
        if(!vram(address))return true;
        for(unsigned i=0;i<size;++i)
            if(changed_[(offset(address)+i)&(psprecomp::GuestMemory::kVramSize-1u)])return false;
        return true;
    }
    void written(unsigned address,unsigned size){
        if(!vram(address))return;
        for(unsigned i=0;i<size;++i)
            changed_[(offset(address)+i)&(psprecomp::GuestMemory::kVramSize-1u)]=1;
    }
    static bool overlaps(unsigned source,unsigned length,unsigned destination,unsigned bytes){
        if(!vram(source) || !bytes)return false;
        const unsigned start=offset(source),target=offset(destination);
        if(length>psprecomp::GuestMemory::kVramSize-start)return true;
        return start<target+bytes && target<start+length;
    }
};
}
